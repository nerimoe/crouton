// Copyright 2012 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <map>
#include <stdio.h>
#include <sys/param.h>
#include <syslog.h>

#include "cras/include/cras_types.h"

extern "C" {
#include "cras/src/server/cras_alsa_card.h"
#include "cras/src/server/cras_alsa_io.h"
#include "cras/src/server/cras_alsa_mixer.h"
#include "cras/src/server/cras_alsa_ucm.h"
#include "cras/src/server/cras_alsa_usb_io.h"
#include "cras/src/server/cras_features.h"
#include "cras/src/server/cras_features_override.h"
#include "cras/src/server/cras_iodev.h"
#include "cras_types.h"
#include "cras_util.h"
#include "third_party/utlist/utlist.h"
}

namespace {

static size_t cras_alsa_mixer_create_called;
static struct cras_alsa_mixer* cras_alsa_mixer_create_return;
static size_t cras_alsa_mixer_destroy_called;
static size_t cras_alsa_iodev_create_called;
static size_t cras_alsa_usb_iodev_create_called;
static struct cras_iodev** cras_alsa_iodev_create_return;
static struct cras_iodev** cras_alsa_usb_iodev_create_return;
static struct cras_iodev fake_dev1, fake_dev2, fake_dev3, fake_dev4;
static struct cras_iodev* cras_alsa_iodev_create_default_return[] = {
    &fake_dev1,
    &fake_dev2,
    &fake_dev3,
    &fake_dev4,
};
static struct cras_iodev* cras_alsa_usb_iodev_create_default_return[] = {
    &fake_dev1,
    &fake_dev2,
    &fake_dev3,
    &fake_dev4,
};
static size_t cras_alsa_iodev_create_return_size;
static size_t cras_alsa_usb_iodev_create_return_size;
static size_t cras_alsa_iodev_legacy_complete_init_called;
static size_t cras_alsa_usb_iodev_legacy_complete_init_called;
static size_t cras_alsa_iodev_ucm_add_nodes_and_jacks_called;
static size_t cras_alsa_usb_iodev_ucm_add_nodes_and_jacks_called;
static size_t cras_alsa_iodev_ucm_complete_init_called;
static size_t cras_alsa_usb_iodev_ucm_complete_init_called;
static size_t cras_alsa_iodev_destroy_called;
static size_t cras_alsa_usb_iodev_destroy_called;
static struct cras_iodev* cras_alsa_iodev_destroy_arg;
static struct cras_iodev* cras_alsa_usb_iodev_destroy_arg;
static size_t cras_alsa_iodev_index_called;
static size_t cras_alsa_usb_iodev_index_called;
static std::map<struct cras_iodev*, unsigned int> cras_alsa_iodev_index_return;
static std::map<struct cras_iodev*, unsigned int>
    cras_alsa_usb_iodev_index_return;
static int alsa_iodev_has_hctl_jacks_return;
static int cras_alsa_usb_iodev_has_hctl_jacks_return;
static size_t snd_ctl_open_called;
static size_t snd_ctl_open_return;
static size_t snd_ctl_close_called;
static size_t snd_ctl_close_return;
static size_t snd_ctl_pcm_next_device_called;
static bool snd_ctl_pcm_next_device_return_error;
static int* snd_ctl_pcm_next_device_set_devs;
static size_t snd_ctl_pcm_next_device_set_devs_size;
static size_t snd_ctl_pcm_next_device_set_devs_index;
static size_t snd_ctl_pcm_info_called;
static int* snd_ctl_pcm_info_rets;
static size_t snd_ctl_pcm_info_rets_size;
static size_t snd_ctl_pcm_info_rets_index;
static size_t snd_ctl_card_info_called;
static int snd_ctl_card_info_ret;
static size_t snd_hctl_open_called;
static int snd_hctl_open_return_value;
static int snd_hctl_close_called;
static size_t snd_hctl_nonblock_called;
static snd_hctl_t* snd_hctl_open_pointer_val;
static size_t snd_hctl_load_called;
static int snd_hctl_load_return_value;
static struct pollfd* snd_hctl_poll_descriptors_fds;
static size_t snd_hctl_poll_descriptors_num_fds;
static size_t snd_hctl_poll_descriptors_called;
static size_t cras_system_add_select_fd_called;
static std::vector<int> cras_system_add_select_fd_values;
static size_t cras_system_rm_select_fd_called;
static std::vector<int> cras_system_rm_select_fd_values;
static size_t snd_hctl_handle_events_called;
static size_t iniparser_freedict_called;
static size_t iniparser_load_called;
static struct cras_device_blocklist* fake_blocklist;
static int cras_device_blocklist_check_retval;
static unsigned ucm_create_called;
static char ucm_create_name[100];
static unsigned ucm_destroy_called;
static size_t ucm_get_dev_for_mixer_called;
static size_t ucm_get_flag_called;
static char ucm_get_flag_name[65];
static char* device_config_dir;
static const char* cras_card_config_dir;
static struct mixer_name* ucm_get_coupled_mixer_names_return_value;
static struct mixer_name* coupled_output_names_value;
static int ucm_has_fully_specified_ucm_flag_return_value;
static int ucm_get_sections_called;
static struct mixer_name* ucm_get_main_volume_names_return_value;
static struct ucm_section* ucm_get_sections_return_value;
static size_t cras_alsa_mixer_add_controls_in_section_called;
static int cras_alsa_mixer_add_controls_in_section_return_value;
static int cras_alsa_mixer_add_main_volume_control_by_name_called;
static int cras_alsa_mixer_add_main_volume_control_by_name_return_value;
static int ucm_get_echo_reference_dev_name_for_dev_called;
static size_t cras_system_check_ignore_ucm_suffix_called;
static bool cras_system_check_ignore_ucm_suffix_value;
static const char* ucm_get_echo_reference_dev_name_for_dev_return_value[4];
static int cras_alsa_mixer_add_controls_by_name_matching_usb_called;
static int cras_alsa_mixer_add_controls_by_name_matching_internal_called;

static void ResetStubData() {
  cras_alsa_mixer_create_called = 0;
  cras_alsa_mixer_create_return = reinterpret_cast<struct cras_alsa_mixer*>(1);
  cras_alsa_mixer_destroy_called = 0;
  cras_alsa_iodev_destroy_arg = NULL;
  cras_alsa_usb_iodev_destroy_arg = NULL;
  cras_alsa_iodev_create_called = 0;
  cras_alsa_usb_iodev_create_called = 0;
  cras_alsa_iodev_create_return = cras_alsa_iodev_create_default_return;
  cras_alsa_iodev_create_return_size =
      ARRAY_SIZE(cras_alsa_iodev_create_default_return);
  cras_alsa_usb_iodev_create_return = cras_alsa_usb_iodev_create_default_return;
  cras_alsa_usb_iodev_create_return_size =
      ARRAY_SIZE(cras_alsa_usb_iodev_create_default_return);
  cras_alsa_iodev_legacy_complete_init_called = 0;
  cras_alsa_usb_iodev_legacy_complete_init_called = 0;
  cras_alsa_iodev_ucm_add_nodes_and_jacks_called = 0;
  cras_alsa_usb_iodev_ucm_add_nodes_and_jacks_called = 0;
  cras_alsa_iodev_ucm_complete_init_called = 0;
  cras_alsa_usb_iodev_ucm_complete_init_called = 0;
  cras_alsa_iodev_destroy_called = 0;
  cras_alsa_usb_iodev_destroy_called = 0;
  cras_alsa_iodev_index_called = 0;
  cras_alsa_usb_iodev_index_called = 0;
  cras_alsa_iodev_index_return.clear();
  cras_alsa_usb_iodev_index_return.clear();
  alsa_iodev_has_hctl_jacks_return = 1;
  cras_alsa_usb_iodev_has_hctl_jacks_return = 1;
  snd_ctl_open_called = 0;
  snd_ctl_open_return = 0;
  snd_ctl_close_called = 0;
  snd_ctl_close_return = 0;
  snd_ctl_pcm_next_device_called = 0;
  snd_ctl_pcm_next_device_return_error = false;
  snd_ctl_pcm_next_device_set_devs_size = 0;
  snd_ctl_pcm_next_device_set_devs_index = 0;
  snd_ctl_pcm_info_called = 0;
  snd_ctl_pcm_info_rets_size = 0;
  snd_ctl_pcm_info_rets_index = 0;
  snd_ctl_card_info_called = 0;
  snd_ctl_card_info_ret = 0;
  snd_hctl_open_called = 0;
  snd_hctl_open_return_value = 0;
  snd_hctl_open_pointer_val = reinterpret_cast<snd_hctl_t*>(0x4323);
  snd_hctl_load_called = 0;
  snd_hctl_load_return_value = 0;
  snd_hctl_close_called = 0;
  snd_hctl_nonblock_called = 0;
  snd_hctl_poll_descriptors_num_fds = 0;
  snd_hctl_poll_descriptors_called = 0;
  snd_hctl_handle_events_called = 0;
  snd_hctl_poll_descriptors_num_fds = 0;
  snd_hctl_poll_descriptors_called = 0;
  cras_system_add_select_fd_called = 0;
  cras_system_add_select_fd_values.clear();
  cras_system_rm_select_fd_called = 0;
  cras_system_rm_select_fd_values.clear();
  iniparser_freedict_called = 0;
  iniparser_load_called = 0;
  fake_blocklist = reinterpret_cast<struct cras_device_blocklist*>(3);
  cras_device_blocklist_check_retval = 0;
  ucm_create_called = 0;
  memset(ucm_create_name, 0, sizeof(ucm_get_flag_name));
  ucm_destroy_called = 0;
  ucm_get_dev_for_mixer_called = 0;
  ucm_get_flag_called = 0;
  memset(ucm_get_flag_name, 0, sizeof(ucm_get_flag_name));
  device_config_dir = reinterpret_cast<char*>(3);
  cras_card_config_dir = NULL;
  ucm_get_coupled_mixer_names_return_value = NULL;
  ucm_get_main_volume_names_return_value = NULL;
  mixer_name_free(coupled_output_names_value);
  coupled_output_names_value = NULL;
  ucm_has_fully_specified_ucm_flag_return_value = 0;
  ucm_get_sections_called = 0;
  ucm_get_sections_return_value = NULL;
  cras_alsa_mixer_add_controls_in_section_called = 0;
  cras_alsa_mixer_add_controls_in_section_return_value = 0;
  cras_alsa_mixer_add_main_volume_control_by_name_called = 0;
  cras_alsa_mixer_add_main_volume_control_by_name_return_value = 0;
  ucm_get_echo_reference_dev_name_for_dev_called = 0;
  cras_system_check_ignore_ucm_suffix_called = 0;
  cras_system_check_ignore_ucm_suffix_value = 0;
  fake_dev1.nodes = NULL;
  fake_dev2.nodes = NULL;
  fake_dev3.nodes = NULL;
  fake_dev4.nodes = NULL;
  cras_alsa_mixer_add_controls_by_name_matching_usb_called = 0;
  cras_alsa_mixer_add_controls_by_name_matching_internal_called = 0;
  cras_features_set_override(CrOSLateBootCrasSplitAlsaUSBInternal, true);
}

TEST(AlsaCard, CreateFailInvalidCard) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 55;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
}

TEST(AlsaCard, CreateFailMixerInit) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  cras_alsa_mixer_create_return = static_cast<struct cras_alsa_mixer*>(NULL);
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(1, cras_alsa_mixer_create_called);
  EXPECT_EQ(0, cras_alsa_mixer_destroy_called);
}

TEST(AlsaCard, CreateFailCtlOpen) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_ctl_open_return = -1;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(1, snd_ctl_open_called);
  EXPECT_EQ(0, snd_ctl_close_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
  EXPECT_EQ(0, cras_alsa_mixer_create_called);
}

TEST(AlsaCard, CreateFailHctlOpen) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_hctl_open_pointer_val = NULL;
  snd_hctl_open_return_value = -1;

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(1, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_close_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
  EXPECT_EQ(1, snd_hctl_open_called);
  EXPECT_EQ(0, snd_hctl_nonblock_called);
  EXPECT_EQ(0, snd_hctl_load_called);
  EXPECT_EQ(1, cras_alsa_mixer_create_called);
  cras_alsa_card_destroy(c);
}

TEST(AlsaCard, CreateFailHctlLoad) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_hctl_load_return_value = -1;

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(1, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_close_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
  EXPECT_EQ(1, snd_hctl_open_called);
  EXPECT_EQ(1, snd_hctl_nonblock_called);
  EXPECT_EQ(1, snd_hctl_load_called);
  EXPECT_EQ(0, cras_alsa_mixer_create_called);
}

TEST(AlsaCard, AddSelectForHctlNoDevices) {
  struct pollfd poll_fds[] = {
      {3, 0, 0},
  };

  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_hctl_poll_descriptors_fds = poll_fds;
  snd_hctl_poll_descriptors_num_fds = ARRAY_SIZE(poll_fds);

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(1, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_close_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
  EXPECT_EQ(1, snd_hctl_open_called);
  EXPECT_EQ(1, snd_hctl_nonblock_called);
  EXPECT_EQ(1, snd_hctl_load_called);
  EXPECT_EQ(1, cras_alsa_mixer_create_called);
  EXPECT_EQ(0, cras_system_add_select_fd_called);
  cras_alsa_card_destroy(c);
  EXPECT_EQ(0, cras_system_rm_select_fd_called);
}

TEST(AlsaCard, AddSelectForHctlWithDevices) {
  struct pollfd poll_fds[] = {
      {3, 0, 0},
  };
  int dev_nums[] = {0};
  int info_rets[] = {0, -1};

  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_hctl_poll_descriptors_fds = poll_fds;
  snd_hctl_poll_descriptors_num_fds = ARRAY_SIZE(poll_fds);

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(2, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(1, cras_alsa_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_iodev_index_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, ucm_create_called);
  EXPECT_EQ(1, ucm_get_dev_for_mixer_called);
  EXPECT_EQ(1, ucm_get_flag_called);
  EXPECT_EQ(0, strcmp(ucm_get_flag_name, "ExtraMainVolume"));
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
  EXPECT_EQ(1, snd_hctl_open_called);
  EXPECT_EQ(1, snd_hctl_nonblock_called);
  EXPECT_EQ(1, snd_hctl_load_called);
  EXPECT_EQ(1, cras_alsa_mixer_create_called);
  ASSERT_EQ(1, cras_system_add_select_fd_called);
  EXPECT_EQ(3, cras_system_add_select_fd_values[0]);
  cras_alsa_card_destroy(c);
  EXPECT_EQ(ARRAY_SIZE(poll_fds), cras_system_rm_select_fd_called);
}

TEST(AlsaCard, CreateFailCtlCardInfo) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_ctl_card_info_ret = -1;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(1, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_close_called);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateNoDevices) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 1;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(0, cras_alsa_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(1, cras_alsa_card_get_index(c));
  EXPECT_EQ(ALSA_CARD_TYPE_INTERNAL, cras_alsa_card_get_type(c));
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(0, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, USBCardBasic) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {0, -1};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_USB;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(1, cras_alsa_usb_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_iodev_create_called);
  EXPECT_EQ(1, cras_alsa_usb_iodev_legacy_complete_init_called);
  EXPECT_EQ(0, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(1, cras_alsa_mixer_add_controls_by_name_matching_usb_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, cras_alsa_usb_iodev_destroy_called);
  EXPECT_EQ(0, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_usb_iodev_create_return[0],
            cras_alsa_usb_iodev_destroy_arg);
}

TEST(AlsaCard, CrOSLateBootCrasSplitAlsaUSBInternalOpen) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {0, -1};
  cras_alsa_card_info card_info;

  ResetStubData();
  cras_features_set_override(CrOSLateBootCrasSplitAlsaUSBInternal, true);
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_USB;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(1, cras_alsa_usb_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_iodev_create_called);
  EXPECT_EQ(1, cras_alsa_usb_iodev_legacy_complete_init_called);
  EXPECT_EQ(0, cras_alsa_iodev_legacy_complete_init_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, cras_alsa_usb_iodev_destroy_called);
  EXPECT_EQ(0, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_usb_iodev_create_return[0],
            cras_alsa_usb_iodev_destroy_arg);
}

TEST(AlsaCard, CrOSLateBootCrasSplitAlsaUSBInternalClose) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {0, -1};
  cras_alsa_card_info card_info;
  ResetStubData();
  cras_features_set_override(CrOSLateBootCrasSplitAlsaUSBInternal, false);
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_USB;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(0, cras_alsa_usb_iodev_create_called);
  EXPECT_EQ(1, cras_alsa_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_usb_iodev_legacy_complete_init_called);
  EXPECT_EQ(1, cras_alsa_iodev_legacy_complete_init_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(0, cras_alsa_usb_iodev_destroy_called);
  EXPECT_EQ(1, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[0], cras_alsa_iodev_destroy_arg);
}

TEST(AlsaCard, CreateOneOutputNextDevError) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_return_error = true;
  card_info.card_type = ALSA_CARD_TYPE_USB;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(snd_ctl_open_called, snd_ctl_close_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateOneOutput) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {0, -1};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_USB;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(2, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(1, cras_alsa_usb_iodev_create_called);
  EXPECT_EQ(1, cras_alsa_usb_iodev_legacy_complete_init_called);
  EXPECT_EQ(0, cras_alsa_usb_iodev_index_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, ucm_create_called);
  EXPECT_EQ(1, ucm_get_dev_for_mixer_called);
  EXPECT_EQ(1, ucm_get_flag_called);
  EXPECT_EQ(0, strcmp(ucm_get_flag_name, "ExtraMainVolume"));
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, ucm_destroy_called);
  EXPECT_EQ(1, cras_alsa_usb_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_usb_iodev_create_return[0],
            cras_alsa_usb_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateOneOutputBlocklisted) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {0, -1};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  alsa_iodev_has_hctl_jacks_return = 0;
  cras_device_blocklist_check_retval = 1;
  card_info.card_type = ALSA_CARD_TYPE_USB;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(2, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(0, cras_alsa_usb_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_usb_iodev_legacy_complete_init_called);
  EXPECT_EQ(cras_card_config_dir, device_config_dir);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(0, cras_alsa_usb_iodev_destroy_called);
  EXPECT_EQ(NULL, cras_alsa_usb_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateTwoOutputs) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0, 3};
  int info_rets[] = {0, -1, 0};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(3, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(2, cras_alsa_iodev_create_called);
  EXPECT_EQ(2, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(1, cras_alsa_iodev_index_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(2, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[1], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateTwoDuplicateDeviceIndex) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0, 0};
  int info_rets[] = {0, -1, 0};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(3, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(1, cras_alsa_iodev_create_called);
  EXPECT_EQ(2, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(1, cras_alsa_iodev_index_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[0], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateOneInput) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {-1, 0};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(2, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(1, cras_alsa_iodev_create_called);
  EXPECT_EQ(1, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(0, cras_alsa_iodev_index_called);
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);
  EXPECT_EQ(1, cras_alsa_mixer_add_controls_by_name_matching_internal_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[0], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateOneInputAndOneOutput) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {0, 0};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(2, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(2, cras_alsa_iodev_create_called);
  EXPECT_EQ(2, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(0, cras_alsa_iodev_index_called);
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(2, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[1], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateOneInputAndOneOutputTwoDevices) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0, 3};
  int info_rets[] = {0, -1, -1, 0};
  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(3, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(2, cras_alsa_iodev_create_called);
  EXPECT_EQ(2, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(0, cras_alsa_iodev_index_called);
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(2, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[1], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateOneOutputWithCoupledMixers) {
  struct cras_alsa_card* c;
  int dev_nums[] = {0};
  int info_rets[] = {0, -1};
  struct mixer_name *mixer_name_1, *mixer_name_2;
  // Use strdup because cras_alsa_card_create will delete it.
  const char *name1 = strdup("MixerName1"), *name2 = strdup("MixerName2");

  cras_alsa_card_info card_info;

  ResetStubData();
  snd_ctl_pcm_next_device_set_devs_size = ARRAY_SIZE(dev_nums);
  snd_ctl_pcm_next_device_set_devs = dev_nums;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;

  /* Creates a list of mixer names as return value of
   * ucm_get_coupled_mixer_names. */
  mixer_name_1 = (struct mixer_name*)malloc(sizeof(*mixer_name_1));
  mixer_name_2 = (struct mixer_name*)malloc(sizeof(*mixer_name_2));
  mixer_name_1->name = name1;
  mixer_name_2->name = name2;
  mixer_name_1->dir = CRAS_STREAM_OUTPUT;
  mixer_name_2->dir = CRAS_STREAM_OUTPUT;
  mixer_name_1->type = MIXER_NAME_VOLUME;
  mixer_name_2->type = MIXER_NAME_VOLUME;

  DL_APPEND(ucm_get_coupled_mixer_names_return_value, mixer_name_1);
  DL_APPEND(ucm_get_coupled_mixer_names_return_value, mixer_name_2);

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);

  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(2, snd_ctl_pcm_next_device_called);
  EXPECT_EQ(1, cras_alsa_iodev_create_called);
  EXPECT_EQ(1, cras_alsa_iodev_legacy_complete_init_called);
  EXPECT_EQ(0, cras_alsa_iodev_index_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, ucm_create_called);
  EXPECT_EQ(1, ucm_get_dev_for_mixer_called);
  EXPECT_EQ(1, ucm_get_flag_called);
  EXPECT_EQ(0, strcmp(ucm_get_flag_name, "ExtraMainVolume"));
  EXPECT_EQ(cras_card_config_dir, device_config_dir);
  EXPECT_EQ(0, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  /* Checks cras_alsa_card_create can handle the list and pass the names to
   * cras_alsa_mixer_create. */
  struct mixer_name* m_name = coupled_output_names_value;
  EXPECT_EQ(0, m_name ? strcmp(m_name->name, "MixerName1") : 1);
  if (m_name) {
    m_name = m_name->next;
  }
  EXPECT_EQ(0, m_name ? strcmp(m_name->name, "MixerName2") : 1);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, ucm_destroy_called);
  EXPECT_EQ(1, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[0], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);

  mixer_name_free(coupled_output_names_value);
  coupled_output_names_value = NULL;
}

TEST(AlsaCard, CreateFullyUCMNoSections) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  ucm_has_fully_specified_ucm_flag_return_value = 1;
  ucm_get_sections_return_value = NULL;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);
  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(0, cras_alsa_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_iodev_ucm_complete_init_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, ucm_destroy_called);
  EXPECT_EQ(0, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(NULL, cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateFullyUCMTwoMainVolume) {
  struct cras_alsa_card* c;
  struct mixer_name *mixer_name_1, *mixer_name_2;
  const char *name1 = strdup("MixerName1"), *name2 = strdup("MixerName2");
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  ucm_has_fully_specified_ucm_flag_return_value = 1;

  /* Creates a list of mixer names as return value of
   * ucm_get_main_volume_names_return_value. */
  mixer_name_1 = (struct mixer_name*)malloc(sizeof(*mixer_name_1));
  mixer_name_2 = (struct mixer_name*)malloc(sizeof(*mixer_name_2));
  mixer_name_1->name = name1;
  mixer_name_2->name = name2;
  mixer_name_1->dir = CRAS_STREAM_OUTPUT;
  mixer_name_2->dir = CRAS_STREAM_OUTPUT;
  mixer_name_1->type = MIXER_NAME_MAIN_VOLUME;
  mixer_name_2->type = MIXER_NAME_MAIN_VOLUME;

  DL_APPEND(ucm_get_main_volume_names_return_value, mixer_name_1);
  DL_APPEND(ucm_get_main_volume_names_return_value, mixer_name_2);

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);

  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(0, cras_alsa_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_iodev_ucm_complete_init_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, cras_alsa_mixer_add_main_volume_control_by_name_called);
  EXPECT_EQ(1, ucm_get_sections_called);
  EXPECT_EQ(0, cras_alsa_mixer_add_controls_in_section_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, ucm_destroy_called);
  EXPECT_EQ(0, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(NULL, cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, TwoUCMSecionsDependentPCM) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;
  struct ucm_section* sections = NULL;
  struct ucm_section* section;

  // Create UCM so that MIC1 and MIC2 will be two nodes on the same iodev.
  section = ucm_section_create("MIC1", "hw:0,3", 0, -1, CRAS_STREAM_INPUT,
                               "my-sound-card Headset Jack", "gpio");
  DL_APPEND(sections, section);
  section = ucm_section_create("MIC2", "hw:0,5", 0, 3, CRAS_STREAM_INPUT,
                               "my-sound-card Headset Jack", "gpio");
  DL_APPEND(sections, section);

  ResetStubData();
  int info_rets[] = {0, 0};
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  ucm_has_fully_specified_ucm_flag_return_value = 1;
  ucm_get_sections_return_value = sections;
  ASSERT_NE(ucm_get_sections_return_value, (struct ucm_section*)NULL);

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);

  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, ucm_get_sections_called);
  EXPECT_EQ(1, snd_ctl_pcm_info_called);
  EXPECT_EQ(2, cras_alsa_mixer_add_controls_in_section_called);
  EXPECT_EQ(1, cras_alsa_iodev_create_called);
  EXPECT_EQ(2, cras_alsa_iodev_ucm_add_nodes_and_jacks_called);
  EXPECT_EQ(1, cras_alsa_iodev_ucm_complete_init_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, ucm_destroy_called);
  EXPECT_EQ(1, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[0], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

struct ucm_section* GenerateUcmSections(void) {
  struct ucm_section* sections = NULL;
  struct ucm_section* section;

  section = ucm_section_create("Headphone", "hw:0,1", 0, -1, CRAS_STREAM_OUTPUT,
                               "my-sound-card Headset Jack", "gpio");
  ucm_section_add_coupled(section, "HP-L", MIXER_NAME_VOLUME);
  ucm_section_add_coupled(section, "HP-R", MIXER_NAME_VOLUME);
  DL_APPEND(sections, section);

  section = ucm_section_create("Speaker", "hw:0,1", 0, -1, CRAS_STREAM_OUTPUT,
                               NULL, NULL);
  ucm_section_add_coupled(section, "SPK-L", MIXER_NAME_VOLUME);
  ucm_section_add_coupled(section, "SPK-R", MIXER_NAME_VOLUME);
  DL_APPEND(sections, section);

  section = ucm_section_create("Internal Mic", "hw:0,1", 0, -1,
                               CRAS_STREAM_INPUT, NULL, NULL);
  ucm_section_add_coupled(section, "INT-MIC-L", MIXER_NAME_VOLUME);
  ucm_section_add_coupled(section, "INT-MIC-R", MIXER_NAME_VOLUME);
  DL_APPEND(sections, section);

  section = ucm_section_create("Mic", "hw:0,1", 1, -1, CRAS_STREAM_INPUT,
                               "my-sound-card Headset Jack", "gpio");
  ucm_section_add_coupled(section, "MIC-L", MIXER_NAME_VOLUME);
  ucm_section_add_coupled(section, "MIC-R", MIXER_NAME_VOLUME);
  DL_APPEND(sections, section);

  section = ucm_section_create("HDMI", "hw:0,1", 2, -1, CRAS_STREAM_OUTPUT,
                               NULL, NULL);
  ucm_section_set_mixer_name(section, "HDMI");
  DL_APPEND(sections, section);

  return sections;
}

TEST(AlsaCard, CreateFullyUCMFailureOnControls) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  ucm_has_fully_specified_ucm_flag_return_value = 1;
  ucm_get_sections_return_value = GenerateUcmSections();
  ASSERT_NE(ucm_get_sections_return_value, (struct ucm_section*)NULL);

  cras_alsa_mixer_add_controls_in_section_return_value = -EINVAL;

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);

  EXPECT_EQ(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, ucm_get_sections_called);
  EXPECT_EQ(1, cras_alsa_mixer_add_controls_in_section_called);
  EXPECT_EQ(0, cras_alsa_iodev_create_called);
  EXPECT_EQ(0, cras_alsa_iodev_ucm_complete_init_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, ucm_destroy_called);
  EXPECT_EQ(0, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(NULL, cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, CreateFullyUCMFourDevicesFiveSections) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;
  int info_rets[] = {0, 0, 0, 0, 0, -1};

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  ucm_has_fully_specified_ucm_flag_return_value = 1;
  ucm_get_sections_return_value = GenerateUcmSections();
  cras_alsa_iodev_index_return[cras_alsa_iodev_create_return[0]] = 0;
  cras_alsa_iodev_index_return[cras_alsa_iodev_create_return[1]] = 0;
  cras_alsa_iodev_index_return[cras_alsa_iodev_create_return[2]] = 1;
  cras_alsa_iodev_index_return[cras_alsa_iodev_create_return[3]] = 2;
  ASSERT_NE(ucm_get_sections_return_value, (struct ucm_section*)NULL);

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);

  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(snd_ctl_close_called, snd_ctl_open_called);
  EXPECT_EQ(1, snd_ctl_card_info_called);
  EXPECT_EQ(1, ucm_get_sections_called);
  EXPECT_EQ(5, snd_ctl_pcm_info_called);
  EXPECT_EQ(5, cras_alsa_mixer_add_controls_in_section_called);
  EXPECT_EQ(4, cras_alsa_iodev_create_called);
  EXPECT_EQ(5, cras_alsa_iodev_ucm_add_nodes_and_jacks_called);
  EXPECT_EQ(4, cras_alsa_iodev_ucm_complete_init_called);

  cras_alsa_card_destroy(c);
  EXPECT_EQ(1, ucm_destroy_called);
  EXPECT_EQ(4, cras_alsa_iodev_destroy_called);
  EXPECT_EQ(cras_alsa_iodev_create_return[3], cras_alsa_iodev_destroy_arg);
  EXPECT_EQ(cras_alsa_mixer_create_called, cras_alsa_mixer_destroy_called);
  EXPECT_EQ(iniparser_load_called, iniparser_freedict_called);
}

TEST(AlsaCard, GG) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;
  int info_rets[] = {0, 0, 0, 0, 0, -1};
  struct cras_ionode nodes[4];
  const char* echo_ref = "echo ref";

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  ucm_has_fully_specified_ucm_flag_return_value = 1;
  ucm_get_sections_return_value = GenerateUcmSections();

  fake_dev1.nodes = nodes;
  fake_dev2.nodes = nodes + 1;
  fake_dev3.nodes = nodes + 2;
  fake_dev4.nodes = nodes + 3;
  snprintf(nodes[0].name, CRAS_NODE_NAME_BUFFER_SIZE, "dev1");
  snprintf(nodes[1].name, CRAS_NODE_NAME_BUFFER_SIZE, "dev2");
  snprintf(nodes[2].name, CRAS_NODE_NAME_BUFFER_SIZE, "dev3");
  snprintf(nodes[3].name, CRAS_NODE_NAME_BUFFER_SIZE, "echo ref");

  ucm_get_echo_reference_dev_name_for_dev_return_value[0] = strdup(echo_ref);

  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            NULL);

  EXPECT_NE(static_cast<struct cras_alsa_card*>(NULL), c);
  EXPECT_EQ(fake_dev1.echo_reference_dev, &fake_dev4);
  cras_alsa_card_destroy(c);
}

TEST(AlsaCard, UCMSuffix) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;
  int info_rets[] = {0, 0, 0, 0, 0, -1};

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  ucm_has_fully_specified_ucm_flag_return_value = 1;
  ucm_get_sections_return_value = GenerateUcmSections();
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            "1mic");
  EXPECT_EQ(0, strcmp(ucm_create_name, "TestName.1mic"));
  EXPECT_EQ(1, cras_system_check_ignore_ucm_suffix_called);
  cras_alsa_card_destroy(c);
}

TEST(AlsaCard, UCMIgnoreSuffix) {
  struct cras_alsa_card* c;
  cras_alsa_card_info card_info;
  int info_rets[] = {0, 0, 0, 0, 0, -1};

  ResetStubData();
  card_info.card_type = ALSA_CARD_TYPE_INTERNAL;
  card_info.card_index = 0;
  snd_ctl_pcm_info_rets_size = ARRAY_SIZE(info_rets);
  snd_ctl_pcm_info_rets = info_rets;
  ucm_has_fully_specified_ucm_flag_return_value = 1;
  ucm_get_sections_return_value = GenerateUcmSections();
  cras_system_check_ignore_ucm_suffix_value = true;
  c = cras_alsa_card_create(&card_info, device_config_dir, fake_blocklist,
                            "1mic");
  EXPECT_EQ(0, strcmp(ucm_create_name, "TestName"));
  EXPECT_EQ(1, cras_system_check_ignore_ucm_suffix_called);
  cras_alsa_card_destroy(c);
}

// Stubs

extern "C" {
struct cras_alsa_mixer* cras_alsa_mixer_create(const char* card_name) {
  cras_alsa_mixer_create_called++;
  return cras_alsa_mixer_create_return;
}

int cras_alsa_mixer_add_controls_by_name_matching_usb(
    struct cras_alsa_mixer* cmix) {
  cras_alsa_mixer_add_controls_by_name_matching_usb_called++;
  return 0;
}

int cras_alsa_mixer_add_controls_by_name_matching_internal(
    struct cras_alsa_mixer* cmix,
    struct mixer_name* extra_controls,
    struct mixer_name* coupled_controls) {
  /* Duplicate coupled_output_names to verify in the end of unittest
   * because names will get freed later in cras_alsa_card_create. */
  struct mixer_name* control;
  cras_alsa_mixer_add_controls_by_name_matching_internal_called++;
  DL_FOREACH (coupled_controls, control) {
    coupled_output_names_value =
        mixer_name_add(coupled_output_names_value, control->name,
                       CRAS_STREAM_OUTPUT, control->type);
  }
  return 0;
}

void cras_alsa_mixer_destroy(struct cras_alsa_mixer* cras_mixer) {
  cras_alsa_mixer_destroy_called++;
}

struct cras_iodev* alsa_iodev_create(size_t card_index,
                                     const char* card_name,
                                     size_t device_index,
                                     const char* pcm_name,
                                     const char* dev_name,
                                     const char* dev_id,
                                     enum CRAS_ALSA_CARD_TYPE card_type,
                                     int is_first,
                                     struct cras_alsa_mixer* mixer,
                                     const struct cras_card_config* config,
                                     struct cras_use_case_mgr* ucm,
                                     snd_hctl_t* hctl,
                                     enum CRAS_STREAM_DIRECTION direction,
                                     size_t usb_vid,
                                     size_t usb_pid,
                                     char* usb_serial_number) {
  struct cras_iodev* result = NULL;
  if (cras_alsa_iodev_create_called < cras_alsa_iodev_create_return_size) {
    result = cras_alsa_iodev_create_return[cras_alsa_iodev_create_called];
  }
  cras_alsa_iodev_create_called++;
  return result;
}
void alsa_iodev_destroy(struct cras_iodev* iodev) {
  cras_alsa_iodev_destroy_called++;
  cras_alsa_iodev_destroy_arg = iodev;
}
int alsa_iodev_legacy_complete_init(struct cras_iodev* iodev) {
  cras_alsa_iodev_legacy_complete_init_called++;
  return 0;
}
int alsa_iodev_ucm_add_nodes_and_jacks(struct cras_iodev* iodev,
                                       struct ucm_section* section) {
  cras_alsa_iodev_ucm_add_nodes_and_jacks_called++;
  return 0;
}
void alsa_iodev_ucm_complete_init(struct cras_iodev* iodev) {
  cras_alsa_iodev_ucm_complete_init_called++;
}
unsigned alsa_iodev_index(struct cras_iodev* iodev) {
  std::map<struct cras_iodev*, unsigned int>::iterator i;
  cras_alsa_iodev_index_called++;
  i = cras_alsa_iodev_index_return.find(iodev);
  if (i != cras_alsa_iodev_index_return.end()) {
    return i->second;
  }
  return 0;
}
int alsa_iodev_has_hctl_jacks(struct cras_iodev* iodev) {
  return alsa_iodev_has_hctl_jacks_return;
}

struct cras_iodev* cras_alsa_usb_iodev_create(
    size_t card_index,
    const char* card_name,
    size_t device_index,
    const char* pcm_name,
    const char* dev_name,
    const char* dev_id,
    enum CRAS_ALSA_CARD_TYPE card_type,
    int is_first,
    struct cras_alsa_mixer* mixer,
    const struct cras_card_config* config,
    struct cras_use_case_mgr* ucm,
    snd_hctl_t* hctl,
    enum CRAS_STREAM_DIRECTION direction,
    size_t usb_vid,
    size_t usb_pid,
    char* usb_serial_number) {
  struct cras_iodev* result = NULL;
  if (cras_alsa_usb_iodev_create_called <
      cras_alsa_usb_iodev_create_return_size) {
    result =
        cras_alsa_usb_iodev_create_return[cras_alsa_usb_iodev_create_called];
  }
  cras_alsa_usb_iodev_create_called++;
  return result;
}

int cras_alsa_usb_iodev_legacy_complete_init(struct cras_iodev* iodev) {
  cras_alsa_usb_iodev_legacy_complete_init_called++;
  return 0;
}

int cras_alsa_usb_iodev_ucm_add_nodes_and_jacks(struct cras_iodev* iodev,
                                                struct ucm_section* section) {
  cras_alsa_usb_iodev_ucm_add_nodes_and_jacks_called++;
  return 0;
}

void cras_alsa_usb_iodev_ucm_complete_init(struct cras_iodev* iodev) {
  cras_alsa_usb_iodev_ucm_complete_init_called++;
}

void cras_alsa_usb_iodev_destroy(struct cras_iodev* iodev) {
  cras_alsa_usb_iodev_destroy_called++;
  cras_alsa_usb_iodev_destroy_arg = iodev;
}

unsigned cras_alsa_usb_iodev_index(struct cras_iodev* iodev) {
  std::map<struct cras_iodev*, unsigned int>::iterator i;
  cras_alsa_usb_iodev_index_called++;
  i = cras_alsa_usb_iodev_index_return.find(iodev);
  if (i != cras_alsa_usb_iodev_index_return.end()) {
    return i->second;
  }
  return 0;
}

int cras_alsa_usb_iodev_has_hctl_jacks(struct cras_iodev* iodev) {
  return cras_alsa_usb_iodev_has_hctl_jacks_return;
}

size_t snd_pcm_info_sizeof() {
  return 10;
}
size_t snd_ctl_card_info_sizeof() {
  return 10;
}
int snd_ctl_open(snd_ctl_t** handle, const char* name, int card) {
  snd_ctl_open_called++;
  if (snd_ctl_open_return == 0) {
    *handle = reinterpret_cast<snd_ctl_t*>(0xff);
  } else {
    *handle = NULL;
  }
  return snd_ctl_open_return;
}
int snd_ctl_close(snd_ctl_t* handle) {
  snd_ctl_close_called++;
  return snd_ctl_close_return;
}
int snd_ctl_pcm_next_device(snd_ctl_t* ctl, int* device) {
  if (snd_ctl_pcm_next_device_return_error) {
    *device = 10;
    return -1;
  }
  snd_ctl_pcm_next_device_called++;
  if (snd_ctl_pcm_next_device_set_devs_index >=
      snd_ctl_pcm_next_device_set_devs_size) {
    *device = -1;
    return 0;
  }
  *device =
      snd_ctl_pcm_next_device_set_devs[snd_ctl_pcm_next_device_set_devs_index];
  snd_ctl_pcm_next_device_set_devs_index++;
  return 0;
}
void snd_pcm_info_set_device(snd_pcm_info_t* obj, unsigned int val) {}
void snd_pcm_info_set_subdevice(snd_pcm_info_t* obj, unsigned int val) {}
void snd_pcm_info_set_stream(snd_pcm_info_t* obj, snd_pcm_stream_t val) {}
const char* snd_pcm_info_get_name(const snd_pcm_info_t* obj) {
  return "Fake device name";
}
const char* snd_pcm_info_get_id(const snd_pcm_info_t* obj) {
  return "Fake device id";
}
int snd_ctl_pcm_info(snd_ctl_t* ctl, snd_pcm_info_t* info) {
  int ret;
  snd_ctl_pcm_info_called++;
  if (snd_ctl_pcm_info_rets_index >= snd_ctl_pcm_info_rets_size) {
    return -1;
  }
  ret = snd_ctl_pcm_info_rets[snd_ctl_pcm_info_rets_index];
  snd_ctl_pcm_info_rets_index++;
  return ret;
}
int snd_ctl_card_info(snd_ctl_t* ctl, snd_ctl_card_info_t* info) {
  snd_ctl_card_info_called++;
  return snd_ctl_card_info_ret;
}
const char* snd_ctl_card_info_get_name(const snd_ctl_card_info_t* obj) {
  return "TestName";
}
const char* snd_ctl_card_info_get_id(const snd_ctl_card_info_t* obj) {
  return "TestId";
}
int snd_hctl_open(snd_hctl_t** hctlp, const char* name, int mode) {
  *hctlp = snd_hctl_open_pointer_val;
  snd_hctl_open_called++;
  return snd_hctl_open_return_value;
}
int snd_hctl_nonblock(snd_hctl_t* hctl, int nonblock) {
  snd_hctl_nonblock_called++;
  return 0;
}
int snd_hctl_load(snd_hctl_t* hctl) {
  snd_hctl_load_called++;
  return snd_hctl_load_return_value;
}
int snd_hctl_close(snd_hctl_t* hctl) {
  snd_hctl_close_called++;
  return 0;
}
int snd_hctl_poll_descriptors_count(snd_hctl_t* hctl) {
  return snd_hctl_poll_descriptors_num_fds;
}
int snd_hctl_poll_descriptors(snd_hctl_t* hctl,
                              struct pollfd* pfds,
                              unsigned int space) {
  unsigned int num = MIN(space, snd_hctl_poll_descriptors_num_fds);
  memcpy(pfds, snd_hctl_poll_descriptors_fds, num * sizeof(*pfds));
  snd_hctl_poll_descriptors_called++;
  return num;
}
int snd_hctl_handle_events(snd_hctl_t* hctl) {
  snd_hctl_handle_events_called++;
  return 0;
}

int cras_system_add_select_fd(int fd,
                              void (*callback)(void* data),
                              void* callback_data) {
  cras_system_add_select_fd_called++;
  cras_system_add_select_fd_values.push_back(fd);
  return 0;
}
void cras_system_rm_select_fd(int fd) {
  cras_system_rm_select_fd_called++;
  cras_system_rm_select_fd_values.push_back(fd);
}

struct cras_card_config* cras_card_config_create(const char* config_path,
                                                 const char* card_name) {
  cras_card_config_dir = config_path;
  return NULL;
}

void cras_card_config_destroy(struct cras_card_config* card_config) {}

struct cras_volume_curve* cras_card_config_get_volume_curve_for_control(
    const struct cras_card_config* card_config,
    const char* control_name) {
  return NULL;
}

int cras_device_blocklist_check(struct cras_device_blocklist* blocklist,
                                unsigned vendor_id,
                                unsigned product_id,
                                unsigned device_index) {
  EXPECT_EQ(fake_blocklist, blocklist);

  return cras_device_blocklist_check_retval;
}

struct cras_use_case_mgr* ucm_create(const char* name) {
  ucm_create_called++;
  strncpy(ucm_create_name, name, sizeof(ucm_create_name) - 1);
  return reinterpret_cast<struct cras_use_case_mgr*>(0x44);
}

void ucm_destroy(struct cras_use_case_mgr* mgr) {
  ucm_destroy_called++;
}

char* ucm_get_dev_for_mixer(struct cras_use_case_mgr* mgr,
                            const char* mixer,
                            enum CRAS_STREAM_DIRECTION dir) {
  ucm_get_dev_for_mixer_called++;
  return strdup("device");
}

char* ucm_get_flag(struct cras_use_case_mgr* mgr, const char* flag_name) {
  ucm_get_flag_called++;
  strncpy(ucm_get_flag_name, flag_name, sizeof(ucm_get_flag_name) - 1);
  return NULL;
}

struct mixer_name* ucm_get_coupled_mixer_names(struct cras_use_case_mgr* mgr,
                                               const char* dev) {
  return ucm_get_coupled_mixer_names_return_value;
}

int ucm_has_fully_specified_ucm_flag(struct cras_use_case_mgr* mgr) {
  return ucm_has_fully_specified_ucm_flag_return_value;
}

struct mixer_name* ucm_get_main_volume_names(struct cras_use_case_mgr* mgr) {
  return ucm_get_main_volume_names_return_value;
}

struct ucm_section* ucm_get_sections(struct cras_use_case_mgr* mgr) {
  ucm_get_sections_called++;
  return ucm_get_sections_return_value;
}
const char* ucm_get_echo_reference_dev_name_for_dev(
    struct cras_use_case_mgr* mgr,
    const char* dev) {
  int idx = ucm_get_echo_reference_dev_name_for_dev_called++;
  return ucm_get_echo_reference_dev_name_for_dev_return_value[idx];
}

int cras_alsa_mixer_add_main_volume_control_by_name(
    struct cras_alsa_mixer* cmix,
    struct mixer_name* mixer_names) {
  cras_alsa_mixer_add_main_volume_control_by_name_called++;
  return cras_alsa_mixer_add_main_volume_control_by_name_return_value;
}

int cras_alsa_mixer_add_controls_in_section(struct cras_alsa_mixer* cmix,
                                            struct ucm_section* section) {
  cras_alsa_mixer_add_controls_in_section_called++;
  return cras_alsa_mixer_add_controls_in_section_return_value;
}

bool cras_system_check_ignore_ucm_suffix(const char* card_name) {
  cras_system_check_ignore_ucm_suffix_called++;
  return cras_system_check_ignore_ucm_suffix_value;
}

void ucm_free_mixer_names(struct mixer_name* names) {
  struct mixer_name* m;
  DL_FOREACH (names, m) {
    DL_DELETE(names, m);
    free((void*)m->name);
    free(m);
  }
}

}  // extern "C"

}  //  namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  openlog(NULL, LOG_PERROR, LOG_USER);
  return RUN_ALL_TESTS();
}
