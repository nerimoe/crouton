// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::ffi::CString;
use std::fmt::Display;
use std::os::raw::c_char;
use std::ptr;
use std::time::Duration;

use dbus::blocking::Connection;
use dbus::blocking::Proxy;
use protobuf::Message;
use system_api::client::OrgChromiumDlcServiceInterface;
use system_api::dlcservice::DlcState;
use system_api::dlcservice::DlcState_State;
use system_api::dlcservice::InstallRequest;
use thiserror::Error;

const DBUS_TIMEOUT: Duration = Duration::from_millis(500);

#[derive(Error, Debug)]
enum Error {
    #[error("D-Bus failure: {0:#}")]
    DBus(#[from] dbus::Error),
    #[error("protocol buffers failure: {0:#}")]
    Protobuf(#[from] protobuf::ProtobufError),
    #[error("CString failure: {0:#}")]
    CString(#[from] std::ffi::NulError),
}

/// All supported DLCs in CRAS.
#[repr(C)]
#[derive(Clone, Copy)]
pub enum CrasDlcId {
    CrasDlcSrBt,
    CrasDlcNcAp,
    NumCrasDlc,
}

impl CrasDlcId {
    fn as_str(&self) -> &'static str {
        match self {
            CrasDlcId::CrasDlcSrBt => "sr-bt-dlc",
            CrasDlcId::CrasDlcNcAp => "nc-ap-dlc",
            CrasDlcId::NumCrasDlc => "num",
        }
    }
}

impl Display for CrasDlcId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

type Result<T> = std::result::Result<T, Error>;

fn get_dlcservice_connection_path(connection: &Connection) -> Proxy<&Connection> {
    connection.with_proxy(
        "org.chromium.DlcService",
        "/org/chromium/DlcService",
        DBUS_TIMEOUT,
    )
}

fn install_dlc(id: CrasDlcId) -> Result<()> {
    let connection = Connection::new_system()?;
    let conn_path = get_dlcservice_connection_path(&connection);

    let mut request = InstallRequest::new();
    request.set_id(id.to_string());
    request.set_reserve(true);
    Ok(conn_path.install(request.write_to_bytes()?)?)
}

fn get_dlc_state(id: CrasDlcId) -> Result<DlcState> {
    let connection = Connection::new_system()?;
    let conn_path = get_dlcservice_connection_path(&connection);

    let res = conn_path.get_dlc_state(id.as_str())?;
    let mut dlc_state = DlcState::new();
    dlc_state.merge_from_bytes(&res)?;
    Ok(dlc_state)
}

fn get_dlc_root_path(id: CrasDlcId) -> Result<CString> {
    let dlc_state = get_dlc_state(id)?;
    CString::new(dlc_state.root_path).map_err(|e| e.into())
}

fn sr_bt_is_available() -> Result<DlcState_State> {
    install_dlc(CrasDlcId::CrasDlcSrBt)?;
    let dlc_state = get_dlc_state(CrasDlcId::CrasDlcSrBt)?;
    Ok(dlc_state.state)
}

fn sr_bt_get_root() -> Result<CString> {
    let dlc_state = get_dlc_state(CrasDlcId::CrasDlcSrBt)?;
    CString::new(dlc_state.root_path).map_err(|e| e.into())
}

/// Returns `true` if the "sr-bt-dlc" packge is ready for use, otherwise
/// retuns `false`.
#[no_mangle]
pub extern "C" fn cras_dlc_sr_bt_is_available() -> bool {
    match sr_bt_is_available() {
        Ok(state) => state == DlcState_State::INSTALLED,
        Err(_) => false,
    }
}

/// Returns Dlc root_path for the "sr-bt-dlc" package.
///
/// # Safety
///
/// This function leaks memory if called from C.
/// There is no valid way to free the returned string in C.
/// TODO(b/277566731): Fix it.
#[no_mangle]
pub unsafe extern "C" fn cras_dlc_sr_bt_get_root() -> *const c_char {
    match sr_bt_get_root() {
        Ok(root_path) => root_path.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

/// Returns `true` if the installation request is successfully sent,
/// otherwise returns `false`.
#[no_mangle]
pub extern "C" fn cras_dlc_install(id: CrasDlcId) -> bool {
    match install_dlc(id) {
        Ok(()) => true,
        Err(err) => {
            log::warn!("cras_dlc_install({}) failed: {}", id, err);
            false
        }
    }
}

/// Returns `true` if the DLC package is ready for use, otherwise
/// returns `false`.
#[no_mangle]
pub extern "C" fn cras_dlc_is_available(id: CrasDlcId) -> bool {
    match get_dlc_state(id) {
        Ok(dlc_state) => dlc_state.state == DlcState_State::INSTALLED,
        Err(_) => false,
    }
}

/// Returns the root path of the DLC package.
///
/// # Safety
///
/// This function leaks memory if called from C.
/// There is no valid way to free the returned string in C.
/// TODO(b/277566731): Fix it.
#[no_mangle]
pub unsafe extern "C" fn cras_dlc_get_root_path(id: CrasDlcId) -> *const c_char {
    match get_dlc_root_path(id) {
        Ok(root_path) => root_path.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}
