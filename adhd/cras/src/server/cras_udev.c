/* Copyright 2012 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <assert.h>
#include <errno.h>
#include <libudev.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "cras/src/common/cras_checksum.h"
#include "cras/src/common/cras_string.h"
#include "cras/src/server/cras_alsa_card.h"
#include "cras/src/server/cras_system_state.h"
#include "cras_types.h"
#include "cras_util.h"

struct udev_callback_data {
  struct udev_monitor* mon;
  struct udev* udev;
  int fd;
};

static unsigned is_action(const char* desired, const char* actual)
    __attribute__((nonnull(1)));

/* Matches Alsa sound device entries generated by udev.  For
 * example:
 *
 *   /devices/pci0000:00/0000:00:1b.0/sound/card1/pcmC1D0p
 *
 * We want to be able to extract:
 *
 *   o The card number
 *   o The device number
 *   o If it's 'playback' (p) or 'capture' (c). (It may not be both.)
 *
 * Given the example above, the following matches should occur:
 *
 *
 *   |                        A                           |
 *                                                   BBCCCD
 *   /devices/pci0000:00/0000:00:1b.0/sound/card1/pcmC1D10p
 *
 * A: The whole regex will be matched.
 * B: The card.
 * C: The device.
 * D: 'p' (playback) or 'c' (capture)
 *
 * The order of the offsets in the 'pmatch' buffer does not appear
 * to match with the documentation:
 *
 *     Each rm_so element that is not -1 indicates the start
 *     offset of the next largest substring match within the
 *     string.
 *
 * But are, instead, filled in the same order presented in the
 * string.  To alleviate possible issudes, the 'C' (card) and 'D'
 * (device) identifying characters are included in the result.
 */
static const char pcm_regex_string[] = "^.*pcm(C[0-9]+)(D[0-9]+)([pc])";
static regex_t pcm_regex;

/* Card regex is similar to above, but only has one field -- the card. The
 * format is the same with the exception of the leaf node being of the form:
 *
 *  /devices/...../card0
 *
 * Where 0 is the card number and the only thing we care about in
 * this case.
 */

static const char card_regex_string[] = "^.*/card([0-9]+)";
static regex_t card_regex;

static char const* const subsystem = "sound";
static const unsigned int MAX_DESC_NAME_LEN = 256;

static unsigned is_action(const char* desired, const char* actual) {
  return actual != NULL && strcmp(desired, actual) == 0;
}

static unsigned is_action_change(const char* action) {
  return is_action("change", action);
}

static unsigned is_action_remove(const char* action) {
  return is_action("remove", action);
}

// If the internal card supports headset, speaker or dmic,
// there should be platform subsystem.
static unsigned is_internal_bus(const char* bus) {
  return (bus != NULL && strncmp(bus, "platform", 8) == 0);
}

static unsigned is_external_bus(const char* bus) {
  return (bus != NULL && (strncmp(bus, "usb", 3) == 0));
}

static bool is_dummy_device(struct udev_device* dev) {
  return strstr(udev_device_get_devpath(dev), "snd_dummy") != NULL;
}

static enum CRAS_ALSA_CARD_TYPE check_device_type(struct udev_device* dev) {
  // treat snd_dummy as external USB device
  if (is_dummy_device(dev)) {
    return ALSA_CARD_TYPE_USB;
  }
  struct udev_device* parent = udev_device_get_parent(dev);

  while (parent != NULL) {
    const char* name = udev_device_get_subsystem(parent);

    if (name != NULL) {
      if (is_external_bus(name)) {
        return ALSA_CARD_TYPE_USB;
      } else if (is_internal_bus(name)) {
        return ALSA_CARD_TYPE_INTERNAL;
      } else {
        return ALSA_CARD_TYPE_HDMI;
      }
    }
    parent = udev_device_get_parent(parent);
  }
  return ALSA_CARD_TYPE_USB;
}

static unsigned is_card_device(struct udev_device* dev,
                               enum CRAS_ALSA_CARD_TYPE* card_type,
                               unsigned* card_number,
                               const char** sysname) {
  regmatch_t m[2];
  const char* devpath = udev_device_get_devpath(dev);

  if (devpath != NULL &&
      regexec(&card_regex, devpath, ARRAY_SIZE(m), m, 0) == 0) {
    *sysname = udev_device_get_sysname(dev);
    *card_type = check_device_type(dev);
    *card_number = (unsigned)atoi(&devpath[m[1].rm_so]);
    return 1;
  }

  return 0;
}

static void set_factory_default(unsigned card_number) {
  static const char alsactl[] = "/usr/sbin/alsactl";
  static const char asound_state[] = "/etc/asound.state";
  char cmd_buf[128];
  struct stat stat_buf;
  int r;

  if (stat(asound_state, &stat_buf) == 0) {
    syslog(LOG_INFO, "%s: init card '%u' to factory default", __FUNCTION__,
           card_number);
    r = snprintf(cmd_buf, ARRAY_SIZE(cmd_buf), "%s --file %s restore %u",
                 alsactl, asound_state, card_number);
    cmd_buf[ARRAY_SIZE(cmd_buf) - 1] = '\0';
    r = system(cmd_buf);
    if (r != 0) {
      syslog(LOG_WARNING,
             "%s: failed to init card '%d' "
             "to factory default.  Failure: %d.  Command: %s",
             __FUNCTION__, card_number, r, cmd_buf);
    }
  }
}

static inline void udev_delay_for_alsa() {
  /* Provide a small delay so that the udev message can
   * propogate throughout the whole system, and Alsa can set up
   * the new device.  Without a small delay, an error of the
   * form:
   *
   *    Fail opening control hw:?
   *
   * will be produced by cras_alsa_card_create().
   */
  usleep(125000);  // 0.125 second
}

/* Reads the "descriptors" file of the usb device and returns the
 * checksum of the contents. Returns 0 if the file can not be read */
static uint32_t calculate_desc_checksum(struct udev_device* dev) {
  char path[MAX_DESC_NAME_LEN];
  struct stat stat_buf;
  unsigned char* buf = NULL;
  int buf_size = 0;
  int read_size;
  ssize_t n;
  uint32_t result;

  if (snprintf(path, sizeof(path), "%s/descriptors",
               udev_device_get_syspath(dev)) >= sizeof(path)) {
    syslog(LOG_ERR, "failed to build path");
    return 0;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    syslog(LOG_WARNING, "failed to open file %s: %s", path,
           cras_strerror(errno));
    return 0;
  }

  if (fstat(fd, &stat_buf) < 0) {
    syslog(LOG_WARNING, "failed to stat file %s: %s", path,
           cras_strerror(errno));
    goto bail;
    return 0;
  }

  read_size = 0;
  while (read_size < stat_buf.st_size) {
    if (read_size == buf_size) {
      if (buf_size == 0) {
        buf_size = 256;
      } else {
        buf_size *= 2;
      }
      uint8_t* new_buf = realloc(buf, buf_size);
      if (new_buf == NULL) {
        syslog(LOG_ERR, "no memory to read file %s", path);
        goto bail;
      }
      buf = new_buf;
    }
    n = read(fd, buf + read_size, buf_size - read_size);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      syslog(LOG_WARNING, "failed to read file %s", path);
      goto bail;
    }
    read_size += n;
  }

  close(fd);
  result = crc32_checksum(buf, read_size);
  free(buf);
  return result;
bail:
  close(fd);
  free(buf);
  return 0;
}

static void fill_usb_card_info(struct cras_alsa_card_info* card_info,
                               struct udev_device* dev) {
  const char* sysattr;
  struct udev_device* parent_dev =
      udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
  if (!parent_dev) {
    return;
  }

  sysattr = udev_device_get_sysattr_value(parent_dev, "idVendor");
  if (sysattr) {
    card_info->usb_vendor_id = strtol(sysattr, NULL, 16);
  }
  sysattr = udev_device_get_sysattr_value(parent_dev, "idProduct");
  if (sysattr) {
    card_info->usb_product_id = strtol(sysattr, NULL, 16);
  }
  sysattr = udev_device_get_sysattr_value(parent_dev, "serial");
  if (sysattr) {
    strncpy(card_info->usb_serial_number, sysattr,
            USB_SERIAL_NUMBER_BUFFER_SIZE - 1);
    card_info->usb_serial_number[USB_SERIAL_NUMBER_BUFFER_SIZE - 1] = '\0';
  }

  card_info->usb_desc_checksum = calculate_desc_checksum(parent_dev);

  syslog(LOG_INFO,
         "USB card: vendor:%04x, product:%04x, serial num:%s, "
         "checksum:%08x",
         card_info->usb_vendor_id, card_info->usb_product_id,
         card_info->usb_serial_number, card_info->usb_desc_checksum);
}

static void device_add_alsa(struct udev_device* dev,
                            const char* sysname,
                            unsigned card,
                            enum CRAS_ALSA_CARD_TYPE card_type) {
  struct cras_alsa_card_info card_info;
  memset(&card_info, 0, sizeof(card_info));

  udev_delay_for_alsa();
  card_info.card_index = card;
  card_info.card_type = card_type;
  if (card_type == ALSA_CARD_TYPE_USB) {
    fill_usb_card_info(&card_info, dev);
  }

  cras_system_add_alsa_card(&card_info);
}

void device_remove_alsa(const char* sysname, unsigned card) {
  udev_delay_for_alsa();
  cras_system_remove_alsa_card(card);
}

static int udev_sound_initialized(struct udev_device* dev) {
  /* udev will set SOUND_INITALIZED=1 for the main card node when the
   * system has already been initialized, i.e. when cras is restarted
   * on an already running system.
   */
  const char* s;

  s = udev_device_get_property_value(dev, "SOUND_INITIALIZED");
  if (s) {
    return 1;
  }

  return 0;
}

static void change_udev_device_if_alsa_device(struct udev_device* dev) {
  /* If the device, 'dev' is an alsa device, add it to the set of
   * devices available for I/O.  Mark it as the active device.
   */
  enum CRAS_ALSA_CARD_TYPE card_type;
  unsigned card_number;
  const char* sysname;

  if (is_card_device(dev, &card_type, &card_number, &sysname) &&
      udev_sound_initialized(dev) &&
      !cras_system_alsa_card_exists(card_number)) {
    if (card_type == ALSA_CARD_TYPE_USB) {
      set_factory_default(card_number);
    }
    device_add_alsa(dev, sysname, card_number, card_type);
  }
}

static void remove_device_if_card(struct udev_device* dev) {
  enum CRAS_ALSA_CARD_TYPE card_type;
  unsigned card_number;
  const char* sysname;

  if (is_card_device(dev, &card_type, &card_number, &sysname)) {
    device_remove_alsa(sysname, card_number);
  }
}

static void enumerate_devices(struct udev_callback_data* data) {
  struct udev_enumerate* enumerate = udev_enumerate_new(data->udev);
  struct udev_list_entry* dl;
  struct udev_list_entry* dev_list_entry;

  udev_enumerate_add_match_subsystem(enumerate, subsystem);
  udev_enumerate_scan_devices(enumerate);
  dl = udev_enumerate_get_list_entry(enumerate);

  udev_list_entry_foreach(dev_list_entry, dl) {
    const char* path = udev_list_entry_get_name(dev_list_entry);
    struct udev_device* dev = udev_device_new_from_syspath(data->udev, path);

    change_udev_device_if_alsa_device(dev);
    udev_device_unref(dev);
  }
  udev_enumerate_unref(enumerate);
}

static void udev_sound_subsystem_callback(void* arg, int revents) {
  struct udev_callback_data* data = (struct udev_callback_data*)arg;
  struct udev_device* dev;

  dev = udev_monitor_receive_device(data->mon);
  if (dev) {
    const char* action = udev_device_get_action(dev);

    if (is_action_change(action)) {
      change_udev_device_if_alsa_device(dev);
    } else if (is_action_remove(action)) {
      remove_device_if_card(dev);
    }
    udev_device_unref(dev);
  } else {
    syslog(LOG_WARNING,
           "%s (internal error): "
           "No device obtained",
           __FUNCTION__);
  }
}

static void compile_regex(regex_t* regex, const char* str) {
  __attribute__((__unused__)) int r = regcomp(regex, str, REG_EXTENDED);
  assert(r == 0);
}

static struct udev_callback_data udev_data;
void cras_udev_start_sound_subsystem_monitor() {
  udev_data.udev = udev_new();
  assert(udev_data.udev != NULL);
  udev_data.mon = udev_monitor_new_from_netlink(udev_data.udev, "udev");

  udev_monitor_filter_add_match_subsystem_devtype(udev_data.mon, subsystem,
                                                  NULL);
  udev_monitor_enable_receiving(udev_data.mon);
  udev_data.fd = udev_monitor_get_fd(udev_data.mon);

  __attribute__((__unused__)) int r = cras_system_add_select_fd(
      udev_data.fd, udev_sound_subsystem_callback, &udev_data, POLLIN);
  assert(r == 0);
  compile_regex(&pcm_regex, pcm_regex_string);
  compile_regex(&card_regex, card_regex_string);

  enumerate_devices(&udev_data);
}

void cras_udev_stop_sound_subsystem_monitor() {
  udev_unref(udev_data.udev);
  regfree(&pcm_regex);
  regfree(&card_regex);
}
