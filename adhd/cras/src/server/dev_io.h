/* Copyright 2017 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * `dev_io` Handles playback to and capture from open devices.  It runs only on
 * the audio thread.
 */

#ifndef CRAS_SRC_SERVER_DEV_IO_H_
#define CRAS_SRC_SERVER_DEV_IO_H_

#include "cras/src/server/cras_iodev.h"
#include "cras/src/server/polled_interval_checker.h"
#include "cras_types.h"

/*
 * Open input/output devices.
 */
struct open_dev {
  // The device.
  struct cras_iodev* dev;
  // The last timestamp audio thread woke up and there is stream
  // on this open device.
  struct timespec last_wake;
  // The longest time between consecutive audio thread wakes
  // in this open_dev's life cycle.
  struct timespec longest_wake;
  // When callback is needed to avoid xrun.
  struct timespec wake_ts;
  struct polled_interval* non_empty_check_pi;
  struct polled_interval* empty_pi;
  // Hack for when the sample rate needs heavy correction.
  int coarse_rate_adjust;
  struct open_dev *prev, *next;
};

/*
 * Fetches streams from each device in `odev_list`.
 *    odev_list - The list of open devices.
 */
void dev_io_playback_fetch(struct open_dev* odev_list);

/*
 * Writes the samples fetched from the streams to the playback devices.
 *    odev_list - The list of open devices.  Devices will be removed when
 *                writing returns an error.
 */
int dev_io_playback_write(struct open_dev** odevs,
                          struct cras_fmt_conv* output_converter);

// Only public for testing.
int write_output_samples(struct open_dev** odevs,
                         struct open_dev* adev,
                         struct cras_fmt_conv* output_converter);

/*
 * Captures samples from each device in the list.
 *    list - Pointer to the list of input devices.  Devices that fail to read
 *           will be removed from the list.
 *    olist - Pointer to the list of output devices.
 */
int dev_io_capture(struct open_dev** list, struct open_dev** olist);

/*
 * Send samples that have been captured to their streams.
 */
int dev_io_send_captured_samples(struct open_dev* idev_list);

// Reads and/or writes audio samples from/to the devices.
void dev_io_run(struct open_dev** odevs,
                struct open_dev** idevs,
                struct cras_fmt_conv* output_converter);

/*
 * Checks the non-empty device state in active output lists and return
 * if there's at least one non-empty device.
 */
int dev_io_check_non_empty_state_transition(struct open_dev* adevs);

/*
 * Fills min_ts with the next time the system should wake to service input.
 * Returns the number of devices waiting.
 */
int dev_io_next_input_wake(struct open_dev** idevs, struct timespec* min_ts);

/*
 * Fills min_ts with the next time the system should wake to service output.
 * Returns the number of devices waiting.
 */
int dev_io_next_output_wake(struct open_dev** odevs, struct timespec* min_ts);

/*
 * Removes a device from a list of devices.
 *    odev_list - A pointer to the list to modify.
 *    dev_to_rm - Find this device in the list and remove it.
 */
void dev_io_rm_open_dev(struct open_dev** odev_list,
                        struct open_dev* dev_to_rm);

// Returns a pointer to an open_dev if it is in the list, otherwise NULL.
struct open_dev* dev_io_find_open_dev(struct open_dev* odev_list,
                                      unsigned int dev_idx);

// Append a new stream to a specified set of iodevs.
int dev_io_append_stream(struct open_dev** odevs,
                         struct open_dev** idevs,
                         struct cras_rstream* stream,
                         struct cras_iodev** iodevs,
                         unsigned int num_iodevs);

// Remove a stream from the provided list of devices.
int dev_io_remove_stream(struct open_dev** dev_list,
                         struct cras_rstream* stream,
                         struct cras_iodev* dev);

#endif  // CRAS_SRC_SERVER_DEV_IO_H_
