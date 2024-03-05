// Copyright 2017 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRAS_SRC_TESTS_DEV_IO_STUBS_H_
#define CRAS_SRC_TESTS_DEV_IO_STUBS_H_

#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

extern "C" {
#include "cras/src/server/cras_iodev.h"
#include "cras/src/server/cras_rstream.h"
#include "cras/src/server/dev_io.h"
#include "cras/src/server/dev_stream.h"
#include "cras_shm.h"
#include "cras_types.h"
#include "third_party/utlist/utlist.h"
}

#include "cras/src/tests/iodev_stub.h"
#include "cras/src/tests/rstream_stub.h"

#define RSTREAM_FAKE_POLL_FD 33

using DevStreamPtr = std::unique_ptr<dev_stream, decltype(free)*>;
using IodevPtr = std::unique_ptr<cras_iodev, decltype(free)*>;
using IonodePtr = std::unique_ptr<cras_ionode, decltype(free)*>;
using OpendevPtr = std::unique_ptr<open_dev, decltype(free)*>;
using RstreamPtr = std::unique_ptr<cras_rstream, decltype(free)*>;

void destroy_shm(struct cras_audio_shm* shm);
using ShmPtr = std::unique_ptr<cras_audio_shm, decltype(destroy_shm)*>;
ShmPtr create_shm(size_t cb_threshold);

// Holds the rstream and devstream pointers for an attached stream.
struct Stream {
  Stream(ShmPtr shm, RstreamPtr rstream, DevStreamPtr dstream)
      : shm(std::move(shm)),
        rstream(std::move(rstream)),
        dstream(std::move(dstream)) {}
  ShmPtr shm;
  RstreamPtr rstream;
  DevStreamPtr dstream;
};
using StreamPtr = std::unique_ptr<Stream>;

// Holds the iodev and ionode pointers for an attached device.
struct Device {
  Device(IodevPtr dev, IonodePtr node, OpendevPtr odev)
      : dev(std::move(dev)), node(std::move(node)), odev(std::move(odev)) {}
  IodevPtr dev;
  IonodePtr node;
  OpendevPtr odev;
};
using DevicePtr = std::unique_ptr<Device>;

RstreamPtr create_rstream(cras_stream_id_t id,
                          CRAS_STREAM_DIRECTION direction,
                          size_t cb_threshold,
                          const cras_audio_format* format,
                          cras_audio_shm* shm);
DevStreamPtr create_dev_stream(unsigned int dev_id, cras_rstream* rstream);
StreamPtr create_stream(cras_stream_id_t id,
                        unsigned int dev_id,
                        CRAS_STREAM_DIRECTION direction,
                        size_t cb_threshold,
                        const cras_audio_format* format);
void AddFakeDataToStream(Stream* stream, unsigned int frames);
int delay_frames_stub(const struct cras_iodev* iodev);
IonodePtr create_ionode(CRAS_NODE_TYPE type);
IodevPtr create_open_iodev(CRAS_STREAM_DIRECTION direction,
                           size_t cb_threshold,
                           cras_audio_format* format,
                           cras_ionode* active_node);
DevicePtr create_device(CRAS_STREAM_DIRECTION direction,
                        size_t cb_threshold,
                        cras_audio_format* format,
                        CRAS_NODE_TYPE active_node_type);
void add_stream_to_dev(IodevPtr& dev, const StreamPtr& stream);
void fill_audio_format(cras_audio_format* format, unsigned int rate);

#endif
