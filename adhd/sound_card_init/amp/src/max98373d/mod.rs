// Copyright 2020 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//! `max98373d` module implements the required initialization workflows for sound
//! cards that use max98373d smart amp.
//! It currently supports boot time calibration for max98373d.
#![deny(missing_docs)]
mod dsm_param;
mod error;
mod settings;

use std::fmt;
use std::fmt::Debug;
use std::fmt::Formatter;
use std::fs;
use std::path::Path;
use std::thread;
use std::time::Duration;

use cros_alsa::Card;
use cros_alsa::IntControl;
use dsm::CalibData;
use dsm::RDCRange;
use dsm::SpeakerStatus;
use dsm::ZeroPlayer;
use dsm::DSM;
use dsm_param::*;
pub use error::Error;
use log::info;
use settings::AmpCalibSettings;
use settings::DeviceSettings;

use crate::Amp;
use crate::Result;

/// It implements the amplifier boot time calibration flow.
pub struct Max98373 {
    card: Card,
    setting: AmpCalibSettings,
}

/// `Max98373CalibData` represents the Max98373 calibration data.
#[derive(Clone, Copy)]
struct Max98373CalibData {
    /// The calibrated raw value of DC resistance of the speaker.
    pub rdc: i32,
    /// The ambient temperature in celsius unit at which the rdc is measured.
    pub temp: f32,
}

impl CalibData for Max98373CalibData {
    fn new(rdc: i32, temp: f32) -> Self {
        Self { rdc, temp }
    }

    fn rdc(&self) -> i32 {
        self.rdc
    }

    fn temp(&self) -> f32 {
        self.temp
    }
    /// Converts the calibrated value to real DC resistance in ohm unit.
    /// Rdc (ohm) = [ID:0x12] * 3.66 / 2^27
    #[inline]
    fn rdc_to_ohm(x: i32) -> f32 {
        (3.66 * x as f32) / (1 << 27) as f32
    }
}

impl Max98373CalibData {
    /// Converts the ambient temperature from celsius to the DsmSetAPI::DsmAmbientTemp unit.
    #[inline]
    fn celsius_to_dsm_unit(celsius: f32) -> i32 {
        // Temperature (℃) = [ID:0x12] / 2^19
        (celsius * (1 << 19) as f32) as i32
    }
}

impl Debug for Max98373CalibData {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        self.debug_fmt(f)
    }
}

impl Amp for Max98373 {
    /// Performs max98373d boot time calibration.
    ///
    /// # Errors
    ///
    /// If any amplifiers fail to complete the calibration.
    fn boot_time_calibration(&mut self) -> Result<()> {
        if !Path::new(&self.setting.dsm_param).exists() {
            return Err(Error::MissingDSMParam.into());
        }

        let num_channels = self.setting.num_channels();
        let dsm = DSM::new(
            self.card.name(),
            num_channels,
            Self::TEMP_UPPER_LIMIT_CELSIUS,
            Self::TEMP_LOWER_LIMIT_CELSIUS,
        );

        // Needs Rdc updates to be done after internal speaker is ready otherwise
        // it would be overwritten by the DSM blob update.
        dsm.wait_for_speakers_ready()?;

        self.set_volume(VolumeMode::Low)?;

        let calib = if !self.setting.boot_time_calibration_enabled {
            info!("skip boot time calibration and use vpd values");
            dsm.get_all_vpd_calibration_value()?
        } else {
            match dsm.check_speaker_over_heated_workflow()? {
                SpeakerStatus::Hot(previous_calib) => previous_calib,
                SpeakerStatus::Cold => {
                    let all_temp = self.get_ambient_temp()?;
                    let all_rdc = self.do_rdc_calibration()?;
                    all_rdc
                        .iter()
                        .zip(all_temp)
                        .enumerate()
                        .map(|(ch, (&rdc, temp))| {
                            dsm.decide_calibration_value_workflow(
                                ch,
                                Max98373CalibData { rdc, temp },
                            )
                            .map_err(crate::Error::DSMError)
                        })
                        .collect::<Result<Vec<_>>>()?
                }
            }
        };
        self.apply_calibration_value(&calib)?;
        info!("applied {:?}", calib);
        self.set_volume(VolumeMode::High)?;
        Ok(())
    }

    fn get_applied_rdc(&mut self, ch: usize) -> Result<f32> {
        let rdc = self.get_rdc()?;
        if ch >= rdc.len() {
            return Err(dsm::Error::InvalidChannelNumer(ch).into());
        }
        Ok(Max98373CalibData::rdc_to_ohm(rdc[ch]))
    }

    fn get_current_rdc(&mut self, ch: usize) -> Result<Option<f32>> {
        let rdc = self.get_adaptive_rdc()?;
        if ch >= rdc.len() {
            return Err(dsm::Error::InvalidChannelNumer(ch).into());
        }
        Ok(Some(Max98373CalibData::rdc_to_ohm(rdc[ch])))
    }

    fn num_channels(&mut self) -> usize {
        self.setting.num_channels()
    }

    fn rdc_ranges(&mut self) -> Vec<RDCRange> {
        self.setting.rdc_ranges.clone()
    }
}

impl Max98373 {
    const TEMP_CALIB_WARM_UP_TIME: Duration = Duration::from_millis(10);
    const RDC_CALIB_WARM_UP_TIME: Duration = Duration::from_millis(500);
    const RDC_CALIB_INTERVAL: Duration = Duration::from_millis(200);
    const CALIB_REPEAT_TIMES: usize = 5;

    const TEMP_UPPER_LIMIT_CELSIUS: f32 = 40.0;
    const TEMP_LOWER_LIMIT_CELSIUS: f32 = 0.0;

    /// Creates an `Max98373`.
    /// # Arguments
    ///
    /// * `card_name` - card_name.
    /// * `config_path` - config file path.
    ///
    /// # Results
    ///
    /// * `Max98373` - It implements the Max98373 functions of boot time calibration.
    ///
    /// # Errors
    ///
    /// * If `Card` creation from sound card name fails.
    pub fn new(card_name: &str, config_path: &Path) -> Result<Self> {
        let conf = fs::read_to_string(config_path)
            .map_err(|e| dsm::Error::FileIOFailed(config_path.to_path_buf(), e))?;
        let settings = DeviceSettings::from_yaml_str(&conf)?;
        Ok(Self {
            card: Card::new(card_name)?,
            setting: settings.amp_calibrations,
        })
    }

    /// Triggers the amplifier calibration and reads the calibrated rdc.
    /// To get accurate calibration results, the main thread calibrates the amplifier while
    /// the `zero_player` starts another thread to play zeros to the speakers.
    fn do_rdc_calibration(&mut self) -> Result<Vec<i32>> {
        let mut zero_player: ZeroPlayer = Default::default();
        zero_player.start(Self::RDC_CALIB_WARM_UP_TIME)?;
        // Playback of zeros is started for Self::RDC_CALIB_WARM_UP_TIME, and the main thread
        // can start the calibration.
        self.set_spt_mode(SPTMode::OFF)?;
        self.set_calibration_mode(CalibMode::ON)?;
        // Playback of zeros is started, and the main thread can start the calibration.
        let mut avg_rdc = vec![0; self.setting.num_channels()];
        for _ in 0..Self::CALIB_REPEAT_TIMES {
            let rdc = self.get_adaptive_rdc()?;
            for i in 0..self.setting.num_channels() {
                avg_rdc[i] += rdc[i];
            }
            thread::sleep(Self::RDC_CALIB_INTERVAL);
        }
        self.set_spt_mode(SPTMode::ON)?;
        self.set_calibration_mode(CalibMode::OFF)?;
        zero_player.stop()?;

        avg_rdc = avg_rdc
            .iter()
            .map(|val| val / Self::CALIB_REPEAT_TIMES as i32)
            .collect();
        Ok(avg_rdc)
    }

    /// Sets the card volume control to the given VolumeMode.
    fn set_volume(&mut self, mode: VolumeMode) -> Result<()> {
        let mut dsm_param = DSMParam::new(
            &mut self.card,
            self.setting.num_channels(),
            &self.setting.dsm_param_read_ctrl,
        )?;

        dsm_param.set_volume_mode(mode);

        self.card
            .control_tlv_by_name(&self.setting.dsm_param_write_ctrl)?
            .save(dsm_param.into())
            .map_err(Error::DSMParamUpdateFailed)?;
        Ok(())
    }

    /// Applies the calibration value to the amp.
    fn apply_calibration_value(&mut self, calib: &[Max98373CalibData]) -> Result<()> {
        let mut dsm_param = DSMParam::new(
            &mut self.card,
            self.setting.num_channels(),
            &self.setting.dsm_param_read_ctrl,
        )?;
        for (ch, chan_calib) in calib.iter().enumerate().take(self.setting.num_channels()) {
            dsm_param.set_rdc(ch, chan_calib.rdc);
            dsm_param.set_ambient_temp(ch, Max98373CalibData::celsius_to_dsm_unit(chan_calib.temp));
        }
        self.card
            .control_tlv_by_name(&self.setting.dsm_param_write_ctrl)?
            .save(dsm_param.into())
            .map_err(Error::DSMParamUpdateFailed)?;
        Ok(())
    }

    /// Returns the ambient temperature in celsius degree.
    fn get_ambient_temp(&mut self) -> Result<Vec<f32>> {
        let mut zero_player: ZeroPlayer = Default::default();
        zero_player.start(Self::TEMP_CALIB_WARM_UP_TIME)?;
        let mut temps = Vec::new();
        for x in 0..self.setting.num_channels() as usize {
            let temp = self
                .card
                .control_by_name::<IntControl>(&self.setting.temp_ctrl[x])?
                .get()?;
            let celsius = Self::measured_temp_to_celsius(temp);
            temps.push(celsius);
        }
        zero_player.stop()?;

        Ok(temps)
    }

    /// Converts the measured ambient temperature to celsius unit.
    #[inline]
    fn measured_temp_to_celsius(temp: i32) -> f32 {
        // Measured Temperature (°C) = ([Mixer Val] * 1.28) - 29
        (temp as f32 * 1.28) - 29.0
    }

    /// Sets the amp to the given smart pilot signal mode.
    fn set_spt_mode(&mut self, mode: SPTMode) -> Result<()> {
        let mut dsm_param = DSMParam::new(
            &mut self.card,
            self.setting.num_channels(),
            &self.setting.dsm_param_read_ctrl,
        )?;
        dsm_param.set_spt_mode(mode);
        self.card
            .control_tlv_by_name(&self.setting.dsm_param_write_ctrl)?
            .save(dsm_param.into())
            .map_err(Error::DSMParamUpdateFailed)?;
        Ok(())
    }

    /// Sets the amp to the given the calibration mode.
    fn set_calibration_mode(&mut self, mode: CalibMode) -> Result<()> {
        let mut dsm_param = DSMParam::new(
            &mut self.card,
            self.setting.num_channels(),
            &self.setting.dsm_param_read_ctrl,
        )?;
        dsm_param.set_calibration_mode(mode);
        self.card
            .control_tlv_by_name(&self.setting.dsm_param_write_ctrl)?
            .save(dsm_param.into())
            .map_err(Error::DSMParamUpdateFailed)?;
        Ok(())
    }

    /// Reads the calibrated rdc.
    /// Must be called when the calibration mode in on.
    fn get_adaptive_rdc(&mut self) -> Result<Vec<i32>> {
        let dsm_param = DSMParam::new(
            &mut self.card,
            self.setting.num_channels(),
            &self.setting.dsm_param_read_ctrl,
        )?;
        Ok(dsm_param.get_adaptive_rdc())
    }

    /// Reads the calibrated rdc.
    fn get_rdc(&mut self) -> Result<Vec<i32>> {
        let dsm_param = DSMParam::new(
            &mut self.card,
            self.setting.num_channels(),
            &self.setting.dsm_param_read_ctrl,
        )?;
        Ok(dsm_param.get_rdc())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn celsius_to_dsm_unit() {
        assert_eq!(Max98373CalibData::celsius_to_dsm_unit(37.0), 0x01280000);
        assert_eq!(Max98373CalibData::celsius_to_dsm_unit(50.0), 0x01900000);
    }

    #[test]
    fn rdc_to_ohm() {
        assert_eq!(
            <Max98373CalibData as CalibData>::rdc_to_ohm(0x05cea0c7),
            2.656767
        );
    }

    #[test]
    fn measured_temp_to_celsius() {
        assert_eq!(Max98373::measured_temp_to_celsius(56), 42.68);
    }
}
