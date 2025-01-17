/* Copyright 2012 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cras/src/server/cras_alsa_ucm.h"

#include <alsa/asoundlib.h>
#include <alsa/use-case.h>
#include <ctype.h>
#include <string.h>
#include <syslog.h>

#include "cras_util.h"
#include "third_party/strlcpy/strlcpy.h"
#include "third_party/utlist/utlist.h"

#define INVALID_JACK_SWITCH -1

static const char jack_control_var[] = "JackControl";
static const char jack_dev_var[] = "JackDev";
static const char jack_switch_var[] = "JackSwitch";
static const char edid_var[] = "EDIDFile";
static const char cap_var[] = "CaptureControl";
static const char override_type_name_var[] = "OverrideNodeType";
static const char dsp_name_var[] = "DspName";
static const char playback_mixer_elem_var[] = "PlaybackMixerElem";
static const char capture_mixer_elem_var[] = "CaptureMixerElem";
static const char min_buffer_level_var[] = "MinBufferLevel";
static const char dma_period_var[] = "DmaPeriodMicrosecs";
static const char disable_software_volume[] = "DisableSoftwareVolume";
static const char playback_device_name_var[] = "PlaybackPCM";
static const char playback_device_rate_var[] = "PlaybackRate";
static const char playback_channels_var[] = "PlaybackChannels";
static const char capture_device_name_var[] = "CapturePCM";
static const char capture_device_rate_var[] = "CaptureRate";
static const char capture_channel_map_var[] = "CaptureChannelMap";
static const char capture_channels_var[] = "CaptureChannels";
static const char coupled_mixers[] = "CoupledMixers";
static const char dependent_device_name_var[] = "DependentPCM";
static const char preempt_hotword_var[] = "PreemptHotword";
static const char echo_reference_dev_name_var[] = "EchoReferenceDev";
static const char rtc_proc_echo_cancellation_modifier[] =
    "RTC Proc Echo Cancellation";
static const char rtc_proc_noise_suppression_modifier[] =
    "RTC Proc Noise Suppression";
static const char rtc_proc_gain_control_modifier[] = "RTC Proc Gain Control";

// SectionModifier prefixes and suffixes.
static const char hotword_model_prefix[] = "Hotword Model";
static const char swap_mode_suffix[] = "Swap Mode";
static const char noise_cancellation_suffix[] = "Noise Cancellation";

/*
 * Set this value in a SectionDevice to specify the intrinsic sensitivity in
 * 0.01 dBFS/Pa. It currently only supports input devices. You should get the
 * value by recording samples without either hardware or software gain. We are
 * still working on building a standard process for measuring it. The value you
 * see now in our UCM is just estimated value. If it is set, CRAS will enable
 * software gain and use the value as a reference for calculating the
 * appropriate software gain to apply to the device to meet our target volume.
 */
static const char intrinsic_sensitivity_var[] = "IntrinsicSensitivity";

/*
 * Set this value in a SectionDevice to specify the default node gain in
 * 0.01 dB.
 */
static const char default_node_gain[] = "DefaultNodeGain";
static const char fully_specified_ucm_var[] = "FullySpecifiedUCM";
static const char main_volume_names[] = "MainVolumeNames";

/*
 * Names of lists for snd_use_case_geti to use when looking up values
 */
static const char enabled_devices_list[] = "_devstatus";
static const char enabled_modifiers_list[] = "_modstatus";

// Use case verbs corresponding to CRAS_STREAM_TYPE.
static const char* use_case_verbs[] = {
    "HiFi", "Multimedia", "Voice Call", "Speech", "Pro Audio", "Accessibility",
};

static const size_t max_section_name_len = 100;

// Represents a list of section names found in UCM.
struct section_name {
  const char* name;
  struct section_name *prev, *next;
};

struct cras_use_case_mgr {
  snd_use_case_mgr_t* mgr;
  char* name;
  unsigned int avail_use_cases;
  enum CRAS_STREAM_TYPE use_case;
  char* hotword_modifier;
};

static inline const char* uc_verb(struct cras_use_case_mgr* mgr) {
  return use_case_verbs[mgr->use_case];
}

static int get_status(struct cras_use_case_mgr* mgr,
                      const char* list,
                      const char* target,
                      long* value) {
  char identifier[max_section_name_len];

  snprintf(identifier, sizeof(identifier), "%s/%s", list, target);
  return snd_use_case_geti(mgr->mgr, identifier, value);
}

static inline int modifier_enabled(struct cras_use_case_mgr* mgr,
                                   const char* modifier,
                                   long* value) {
  return get_status(mgr, enabled_modifiers_list, modifier, value);
}

static inline int device_enabled(struct cras_use_case_mgr* mgr,
                                 const char* dev,
                                 long* value) {
  return get_status(mgr, enabled_devices_list, dev, value);
}

static int get_var(struct cras_use_case_mgr* mgr,
                   const char* var,
                   const char* dev,
                   const char* verb,
                   const char** value) {
  char* id;
  int rc;
  size_t len = strlen(var) + strlen(dev) + strlen(verb) + 4;

  id = (char*)malloc(len);
  if (!id) {
    return -ENOMEM;
  }
  snprintf(id, len, "=%s/%s/%s", var, dev, verb);
  rc = snd_use_case_get(mgr->mgr, id, value);

  free((void*)id);
  return rc;
}

static int get_int(struct cras_use_case_mgr* mgr,
                   const char* var,
                   const char* dev,
                   const char* verb,
                   int* value) {
  const char* str_value;
  int rc;

  if (!value) {
    return -EINVAL;
  }
  rc = get_var(mgr, var, dev, verb, &str_value);
  if (rc != 0) {
    return rc;
  }
  *value = atoi(str_value);
  free((void*)str_value);
  return 0;
}

static int ucm_set_modifier_enabled(struct cras_use_case_mgr* mgr,
                                    const char* mod,
                                    int enable) {
  int rc;

  rc = snd_use_case_set(mgr->mgr, enable ? "_enamod" : "_dismod", mod);
  if (rc) {
    syslog(LOG_WARNING, "Can not %s UCM modifier %s, rc = %d",
           enable ? "enable" : "disable", mod, rc);
  }
  return rc;
}

static int ucm_str_ends_with_suffix(const char* str, const char* suffix) {
  if (!str || !suffix) {
    return 0;
  }
  size_t len_str = strlen(str);
  size_t len_suffix = strlen(suffix);
  if (len_suffix > len_str) {
    return 0;
  }
  return strncmp(str + len_str - len_suffix, suffix, len_suffix) == 0;
}

static int ucm_section_exists_with_suffix(struct cras_use_case_mgr* mgr,
                                          const char* suffix,
                                          const char* identifier) {
  const char** list;
  unsigned int i;
  int num_entries;
  int exist = 0;

  num_entries = snd_use_case_get_list(mgr->mgr, identifier, &list);
  if (num_entries <= 0) {
    return num_entries;
  }

  for (i = 0; i < (unsigned int)num_entries; i += 2) {
    if (!list[i]) {
      continue;
    }

    if (ucm_str_ends_with_suffix(list[i], suffix)) {
      exist = 1;
      break;
    }
  }
  snd_use_case_free_list(list, num_entries);
  return exist;
}

static int ucm_mod_exists_with_suffix(struct cras_use_case_mgr* mgr,
                                      const char* suffix) {
  char* identifier;
  int rc;

  identifier = snd_use_case_identifier("_modifiers/%s", uc_verb(mgr));
  rc = ucm_section_exists_with_suffix(mgr, suffix, identifier);
  free(identifier);
  return rc;
}

static int ucm_mod_exists_with_name(struct cras_use_case_mgr* mgr,
                                    const char* name) {
  long value;
  int ret;
  ret = modifier_enabled(mgr, name, &value);
  if (ret == -ENOENT) {
    return 0;
  }
  return ret < 0 ? ret : 1;
}

// Get a list of section names whose variable is the matched value.
static struct section_name* ucm_get_sections_for_var(
    struct cras_use_case_mgr* mgr,
    const char* var,
    const char* value,
    const char* identifier,
    enum CRAS_STREAM_DIRECTION direction) {
  const char** list;
  struct section_name *section_names = NULL, *s_name;
  unsigned int i;
  int num_entries;
  int rc;

  num_entries = snd_use_case_get_list(mgr->mgr, identifier, &list);
  if (num_entries < 0) {
    syslog(LOG_WARNING, "Failed to get section UCM list for %s, error: %d",
           identifier, num_entries);
  }
  if (num_entries <= 0) {
    return NULL;
  }

  /* snd_use_case_get_list fills list with pairs of device name and
   * comment, so device names are in even-indexed elements. */
  for (i = 0; i < (unsigned int)num_entries; i += 2) {
    const char* this_value;

    if (!list[i]) {
      continue;
    }

    rc = get_var(mgr, var, list[i], uc_verb(mgr), &this_value);
    if (rc) {
      continue;
    }

    if (!strcmp(value, this_value)) {
      s_name = (struct section_name*)malloc(sizeof(struct section_name));

      if (!s_name) {
        syslog(LOG_ERR, "Failed to allocate memory");
        free((void*)this_value);
        break;
      }

      s_name->name = strdup(list[i]);
      DL_APPEND(section_names, s_name);
    }
    free((void*)this_value);
  }

  snd_use_case_free_list(list, num_entries);
  return section_names;
}

static struct section_name* ucm_get_devices_for_var(
    struct cras_use_case_mgr* mgr,
    const char* var,
    const char* value,
    enum CRAS_STREAM_DIRECTION dir) {
  char* identifier;
  struct section_name* section_names;

  identifier = snd_use_case_identifier("_devices/%s", uc_verb(mgr));
  section_names = ucm_get_sections_for_var(mgr, var, value, identifier, dir);
  free(identifier);
  return section_names;
}

static const char* ucm_get_value_for_dev(struct cras_use_case_mgr* mgr,
                                         const char* value_var,
                                         const char* dev) {
  const char* name = NULL;
  int rc;

  rc = get_var(mgr, value_var, dev, uc_verb(mgr), &name);
  if (rc) {
    return NULL;
  }

  return name;
}

static inline const char* ucm_get_playback_device_name_for_dev(
    struct cras_use_case_mgr* mgr,
    const char* dev) {
  return ucm_get_value_for_dev(mgr, playback_device_name_var, dev);
}

static inline const char* ucm_get_capture_device_name_for_dev(
    struct cras_use_case_mgr* mgr,
    const char* dev) {
  return ucm_get_value_for_dev(mgr, capture_device_name_var, dev);
}

/* Gets the value of DependentPCM property. This is used to structure two
 * SectionDevices under one cras iodev to avoid two PCMs be open at the
 * same time because of restriction in lower layer driver or hardware.
 */
static inline const char* ucm_get_dependent_device_name_for_dev(
    struct cras_use_case_mgr* mgr,
    const char* dev) {
  return ucm_get_value_for_dev(mgr, dependent_device_name_var, dev);
}

/* Get a list of mixer names specified in a UCM variable separated by ",".
 * E.g. "Left Playback,Right Playback".
 */
static struct mixer_name* ucm_get_mixer_names(struct cras_use_case_mgr* mgr,
                                              const char* dev,
                                              const char* var,
                                              enum CRAS_STREAM_DIRECTION dir,
                                              mixer_name_type type) {
  const char* names_in_string = NULL;
  int rc;
  char *tokens, *name;
  char* laststr = NULL;
  struct mixer_name* names = NULL;

  rc = get_var(mgr, var, dev, uc_verb(mgr), &names_in_string);
  if (rc) {
    return NULL;
  }

  tokens = strdup(names_in_string);
  name = strtok_r(tokens, ",", &laststr);
  while (name != NULL) {
    names = mixer_name_add(names, name, dir, type);
    name = strtok_r(NULL, ",", &laststr);
  }
  free((void*)names_in_string);
  free(tokens);
  return names;
}

// Gets the modifier name of Noise Cancellation for the given node_name.
static void ucm_get_node_noise_cancellation_name(const char* node_name,
                                                 char* mod_name) {
  size_t len = strlen(node_name) + 1 + strlen(noise_cancellation_suffix) + 1;
  if (len > max_section_name_len) {
    syslog(LOG_ERR, "Length of the given section name is %zu > %zu(max)", len,
           max_section_name_len);
    len = max_section_name_len;
  }
  snprintf(mod_name, len, "%s %s", node_name, noise_cancellation_suffix);
}

// Exported Interface

struct cras_use_case_mgr* ucm_create(const char* name) {
  struct cras_use_case_mgr* mgr;
  int rc;
  const char** list;
  int num_verbs, i, j;

  assert_on_compile(ARRAY_SIZE(use_case_verbs) == CRAS_STREAM_NUM_TYPES);

  if (!name) {
    return NULL;
  }

  mgr = (struct cras_use_case_mgr*)malloc(sizeof(*mgr));
  if (!mgr) {
    return NULL;
  }

  mgr->name = strdup(name);
  if (!mgr->name) {
    goto cleanup;
  }

  rc = snd_use_case_mgr_open(&mgr->mgr, name);
  if (rc) {
    syslog(LOG_WARNING, "Can not open ucm for card %s, rc = %d", name, rc);
    goto cleanup;
  }

  mgr->avail_use_cases = 0;
  mgr->hotword_modifier = NULL;
  num_verbs = snd_use_case_get_list(mgr->mgr, "_verbs", &list);
  for (i = 0; i < num_verbs; i += 2) {
    for (j = 0; j < CRAS_STREAM_NUM_TYPES; ++j) {
      if (strcmp(list[i], use_case_verbs[j]) == 0) {
        break;
      }
    }
    if (j < CRAS_STREAM_NUM_TYPES) {
      mgr->avail_use_cases |= (1 << j);
    }
  }
  if (num_verbs > 0) {
    snd_use_case_free_list(list, num_verbs);
  }

  rc = ucm_set_use_case(mgr, CRAS_STREAM_TYPE_DEFAULT);
  if (rc) {
    goto cleanup_mgr;
  }

  return mgr;

cleanup_mgr:
  snd_use_case_mgr_close(mgr->mgr);
cleanup:
  free(mgr->name);
  free(mgr);
  return NULL;
}

void ucm_destroy(struct cras_use_case_mgr* mgr) {
  snd_use_case_mgr_close(mgr->mgr);
  free(mgr->hotword_modifier);
  free(mgr->name);
  free(mgr);
}

int ucm_set_use_case(struct cras_use_case_mgr* mgr,
                     enum CRAS_STREAM_TYPE use_case) {
  int rc;

  if (mgr->avail_use_cases & (1 << use_case)) {
    mgr->use_case = use_case;
  } else {
    syslog(LOG_ERR, "Unavailable use case %d for card %s", use_case, mgr->name);
    return -EINVAL;
  }

  rc = snd_use_case_set(mgr->mgr, "_verb", uc_verb(mgr));
  if (rc) {
    syslog(LOG_ERR, "Can not set verb %s for card %s, rc = %d", uc_verb(mgr),
           mgr->name, rc);
    return rc;
  }

  return 0;
}

int ucm_swap_mode_exists(struct cras_use_case_mgr* mgr) {
  return ucm_mod_exists_with_suffix(mgr, swap_mode_suffix);
}

static int ucm_modifier_try_enable(struct cras_use_case_mgr* mgr,
                                   int enable,
                                   const char* name) {
  long value;
  int ret;

  if (!ucm_mod_exists_with_name(mgr, name)) {
    syslog(LOG_WARNING, "Can not find modifier %s.", name);
    return -ENOTSUP;
  }

  ret = modifier_enabled(mgr, name, &value);
  if (ret < 0) {
    syslog(LOG_WARNING, "Failed to check modifier: %d", ret);
    return ret;
  }

  if (value == !!enable) {
    syslog(LOG_DEBUG, "Modifier %s is already %s.", name,
           enable ? "enabled" : "disabled");
    return 0;
  }

  syslog(LOG_DEBUG, "UCM %s Modifier %s", enable ? "enable" : "disable", name);
  return ucm_set_modifier_enabled(mgr, name, enable);
}

int ucm_enable_swap_mode(struct cras_use_case_mgr* mgr,
                         const char* node_name,
                         int enable) {
  char swap_mod[max_section_name_len];

  snprintf(swap_mod, sizeof(swap_mod), "%s %s", node_name, swap_mode_suffix);
  return ucm_set_modifier_enabled(mgr, swap_mod, enable);
}

int inline ucm_node_echo_cancellation_exists(struct cras_use_case_mgr* mgr) {
  return ucm_mod_exists_with_name(mgr, rtc_proc_echo_cancellation_modifier);
}

int inline ucm_enable_node_echo_cancellation(struct cras_use_case_mgr* mgr,
                                             int enable) {
  return ucm_modifier_try_enable(mgr, enable,
                                 rtc_proc_echo_cancellation_modifier);
}

int inline ucm_node_noise_suppression_exists(struct cras_use_case_mgr* mgr) {
  return ucm_mod_exists_with_name(mgr, rtc_proc_noise_suppression_modifier);
}

int inline ucm_enable_node_noise_suppression(struct cras_use_case_mgr* mgr,
                                             int enable) {
  return ucm_modifier_try_enable(mgr, enable,
                                 rtc_proc_noise_suppression_modifier);
}

int inline ucm_node_gain_control_exists(struct cras_use_case_mgr* mgr) {
  return ucm_mod_exists_with_name(mgr, rtc_proc_gain_control_modifier);
}

int inline ucm_enable_node_gain_control(struct cras_use_case_mgr* mgr,
                                        int enable) {
  return ucm_modifier_try_enable(mgr, enable, rtc_proc_gain_control_modifier);
}

int ucm_node_noise_cancellation_exists(struct cras_use_case_mgr* mgr,
                                       const char* node_name) {
  char node_modifier_name[max_section_name_len];

  ucm_get_node_noise_cancellation_name(node_name, node_modifier_name);
  return ucm_mod_exists_with_name(mgr, node_modifier_name);
}

int ucm_enable_node_noise_cancellation(struct cras_use_case_mgr* mgr,
                                       const char* node_name,
                                       int enable) {
  char node_modifier_name[max_section_name_len];

  ucm_get_node_noise_cancellation_name(node_name, node_modifier_name);
  return ucm_modifier_try_enable(mgr, enable, node_modifier_name);
}

int ucm_set_enabled(struct cras_use_case_mgr* mgr,
                    const char* dev,
                    int enable) {
  int rc;
  long value;

  rc = device_enabled(mgr, dev, &value);
  if (rc < 0) {
    syslog(LOG_WARNING, "Failed to check device status in enable");
    return rc;
  }
  if (value == !!enable) {
    return 0;
  }
  syslog(LOG_DEBUG, "UCM %s %s", enable ? "enable" : "disable", dev);
  rc = snd_use_case_set(mgr->mgr, enable ? "_enadev" : "_disdev", dev);
  if (rc && (rc != -ENOENT || ucm_has_fully_specified_ucm_flag(mgr))) {
    syslog(LOG_WARNING, "Can not %s UCM for device %s, rc = %d",
           enable ? "enable" : "disable", dev, rc);
  }
  return rc;
}

char* ucm_get_flag(struct cras_use_case_mgr* mgr, const char* flag_name) {
  char* flag_value = NULL;
  const char* value;
  int rc;

  // Set device to empty string since flag is specified in verb section
  rc = get_var(mgr, flag_name, "", uc_verb(mgr), &value);
  if (!rc) {
    flag_value = strdup(value);
    free((void*)value);
  }

  return flag_value;
}

char* ucm_get_cap_control(struct cras_use_case_mgr* mgr, const char* ucm_dev) {
  char* control_name = NULL;
  const char* value;
  int rc;

  rc = get_var(mgr, cap_var, ucm_dev, uc_verb(mgr), &value);
  if (!rc) {
    control_name = strdup(value);
    free((void*)value);
  }

  return control_name;
}

inline const char* ucm_get_override_type_name(struct cras_use_case_mgr* mgr,
                                              const char* dev) {
  return ucm_get_value_for_dev(mgr, override_type_name_var, dev);
}

char* ucm_get_dev_for_jack(struct cras_use_case_mgr* mgr,
                           const char* jack,
                           enum CRAS_STREAM_DIRECTION direction) {
  struct section_name *section_names, *c;
  char* ret = NULL;

  section_names = ucm_get_devices_for_var(mgr, jack_dev_var, jack, direction);

  DL_FOREACH (section_names, c) {
    if (!strcmp(c->name, "Mic")) {
      // Skip mic section for output
      if (direction == CRAS_STREAM_OUTPUT) {
        continue;
      }
    } else {
      // Only check mic for input.
      if (direction == CRAS_STREAM_INPUT) {
        continue;
      }
    }
    ret = strdup(c->name);
    break;
  }

  DL_FOREACH (section_names, c) {
    DL_DELETE(section_names, c);
    free((void*)c->name);
    free(c);
  }

  return ret;
}

char* ucm_get_dev_for_mixer(struct cras_use_case_mgr* mgr,
                            const char* mixer,
                            enum CRAS_STREAM_DIRECTION dir) {
  struct section_name *section_names = NULL, *c;
  char* ret = NULL;

  if (dir == CRAS_STREAM_OUTPUT) {
    section_names =
        ucm_get_devices_for_var(mgr, playback_mixer_elem_var, mixer, dir);
  } else if (dir == CRAS_STREAM_INPUT) {
    section_names =
        ucm_get_devices_for_var(mgr, capture_mixer_elem_var, mixer, dir);
  }

  if (section_names) {
    ret = strdup(section_names->name);
  }

  DL_FOREACH (section_names, c) {
    DL_DELETE(section_names, c);
    free((void*)c->name);
    free(c);
  }

  return ret;
}

inline const char* ucm_get_edid_file_for_dev(struct cras_use_case_mgr* mgr,
                                             const char* dev) {
  return ucm_get_value_for_dev(mgr, edid_var, dev);
}

inline const char* ucm_get_dsp_name_for_dev(struct cras_use_case_mgr* mgr,
                                            const char* dev) {
  return ucm_get_value_for_dev(mgr, dsp_name_var, dev);
}

int ucm_get_min_buffer_level(struct cras_use_case_mgr* mgr,
                             unsigned int* level) {
  int value;
  int rc;

  rc = get_int(mgr, min_buffer_level_var, "", uc_verb(mgr), &value);
  if (rc) {
    return -ENOENT;
  }
  *level = value;

  return 0;
}

unsigned int ucm_get_disable_software_volume(struct cras_use_case_mgr* mgr) {
  int value;
  int rc;

  rc = get_int(mgr, disable_software_volume, "", uc_verb(mgr), &value);
  if (rc) {
    return 0;
  }

  return value;
}

int ucm_get_default_node_gain(struct cras_use_case_mgr* mgr,
                              const char* dev,
                              long* gain) {
  int value;
  int rc;

  rc = get_int(mgr, default_node_gain, dev, uc_verb(mgr), &value);
  if (rc) {
    return rc;
  }
  *gain = value;
  return 0;
}

int ucm_get_intrinsic_sensitivity(struct cras_use_case_mgr* mgr,
                                  const char* dev,
                                  long* sensitivity) {
  int value;
  int rc;

  rc = get_int(mgr, intrinsic_sensitivity_var, dev, uc_verb(mgr), &value);
  if (rc) {
    return rc;
  }
  *sensitivity = value;
  return 0;
}

int ucm_get_preempt_hotword(struct cras_use_case_mgr* mgr, const char* dev) {
  int value;
  int rc;

  rc = get_int(mgr, preempt_hotword_var, dev, uc_verb(mgr), &value);
  if (rc) {
    return 0;
  }
  return value;
}

static int get_device_index_from_target(const char* target_device_name);

int ucm_get_alsa_dev_idx_for_dev(struct cras_use_case_mgr* mgr,
                                 const char* dev,
                                 enum CRAS_STREAM_DIRECTION direction) {
  const char* pcm_name = NULL;
  int dev_idx = -1;

  if (direction == CRAS_STREAM_OUTPUT) {
    pcm_name = ucm_get_playback_device_name_for_dev(mgr, dev);
  } else if (direction == CRAS_STREAM_INPUT) {
    pcm_name = ucm_get_capture_device_name_for_dev(mgr, dev);
  }

  if (pcm_name) {
    dev_idx = get_device_index_from_target(pcm_name);
    free((void*)pcm_name);
  }
  return dev_idx;
}

inline const char* ucm_get_echo_reference_dev_name_for_dev(
    struct cras_use_case_mgr* mgr,
    const char* dev) {
  return ucm_get_value_for_dev(mgr, echo_reference_dev_name_var, dev);
}

int ucm_get_sample_rate_for_dev(struct cras_use_case_mgr* mgr,
                                const char* dev,
                                enum CRAS_STREAM_DIRECTION direction) {
  int value;
  int rc;
  const char* var_name;

  if (direction == CRAS_STREAM_OUTPUT) {
    var_name = playback_device_rate_var;
  } else if (direction == CRAS_STREAM_INPUT) {
    var_name = capture_device_rate_var;
  } else {
    return -EINVAL;
  }

  rc = get_int(mgr, var_name, dev, uc_verb(mgr), &value);
  if (rc) {
    return rc;
  }

  return value;
}

int ucm_get_channels_for_dev(struct cras_use_case_mgr* mgr,
                             const char* dev,
                             enum CRAS_STREAM_DIRECTION direction,
                             size_t* channels) {
  int value;
  int rc;
  const char* var_name;

  if (direction == CRAS_STREAM_OUTPUT) {
    var_name = playback_channels_var;
  } else if (direction == CRAS_STREAM_INPUT) {
    var_name = capture_channels_var;
  } else {
    return -EINVAL;
  }

  rc = get_int(mgr, var_name, dev, uc_verb(mgr), &value);
  if (rc) {
    return rc;
  }
  if (value < 0) {
    return -EINVAL;
  }

  *channels = (size_t)value;
  return 0;
}

int ucm_get_capture_chmap_for_dev(struct cras_use_case_mgr* mgr,
                                  const char* dev,
                                  int8_t* channel_layout) {
  const char* var_str;
  char *tokens, *token;
  int i, rc;

  rc = get_var(mgr, capture_channel_map_var, dev, uc_verb(mgr), &var_str);
  if (rc) {
    return rc;
  }

  tokens = strdup(var_str);
  token = strtok(tokens, " ");
  for (i = 0; token && (i < CRAS_CH_MAX); i++) {
    channel_layout[i] = atoi(token);
    token = strtok(NULL, " ");
  }

  free((void*)tokens);
  free((void*)var_str);
  return (i == CRAS_CH_MAX) ? 0 : -EINVAL;
}

struct mixer_name* ucm_get_coupled_mixer_names(struct cras_use_case_mgr* mgr,
                                               const char* dev) {
  return ucm_get_mixer_names(mgr, dev, coupled_mixers, CRAS_STREAM_OUTPUT,
                             MIXER_NAME_VOLUME);
}

static int get_device_index_from_target(const char* target_device_name) {
  // Expects a string in the form: hw:card-name,<num>
  const char* pos = target_device_name;
  if (!pos) {
    return -EINVAL;
  }
  while (*pos && *pos != ',') {
    ++pos;
  }
  if (*pos == ',') {
    ++pos;
    return atoi(pos);
  }
  return -EINVAL;
}

static const char* ucm_get_dir_for_device(struct cras_use_case_mgr* mgr,
                                          const char* dev_name,
                                          enum CRAS_STREAM_DIRECTION* dir) {
  const char* pcm_name;

  pcm_name = ucm_get_playback_device_name_for_dev(mgr, dev_name);

  if (pcm_name) {
    *dir = CRAS_STREAM_OUTPUT;
    return pcm_name;
  }

  pcm_name = ucm_get_capture_device_name_for_dev(mgr, dev_name);
  if (pcm_name) {
    *dir = CRAS_STREAM_INPUT;
    return pcm_name;
  }

  *dir = CRAS_STREAM_UNDEFINED;
  return NULL;
}

static int ucm_parse_device_section(struct cras_use_case_mgr* mgr,
                                    const char* dev,
                                    struct ucm_section** sections) {
  enum CRAS_STREAM_DIRECTION dir;
  int dev_idx = -1;
  int dependent_dev_idx = -1;
  const char* jack_name = NULL;
  const char* jack_type = NULL;
  const char* jack_dev = NULL;
  const char* jack_control = NULL;
  const char* mixer_name = NULL;
  struct mixer_name* m_name;
  int rc = 0;
  const char* pcm_name;
  const char* dependent_dev_name = NULL;
  struct ucm_section* dev_sec;
  const char* dev_name;

  dev_name = strdup(dev);
  if (!dev_name) {
    return 0;
  }

  pcm_name = ucm_get_dir_for_device(mgr, dev_name, &dir);

  if (pcm_name) {
    dev_idx = get_device_index_from_target(pcm_name);
  }

  if (dir == CRAS_STREAM_UNDEFINED) {
    syslog(LOG_ERR,
           "UCM configuration for device '%s' missing"
           " PlaybackPCM or CapturePCM definition.",
           dev_name);
    rc = -EINVAL;
    goto error_cleanup;
  }

  dependent_dev_name = ucm_get_dependent_device_name_for_dev(mgr, dev_name);
  if (dependent_dev_name) {
    dependent_dev_idx = get_device_index_from_target(dependent_dev_name);
  }

  jack_dev = ucm_get_jack_dev_for_dev(mgr, dev_name);
  jack_control = ucm_get_jack_control_for_dev(mgr, dev_name);
  if (dir == CRAS_STREAM_OUTPUT) {
    mixer_name = ucm_get_playback_mixer_elem_for_dev(mgr, dev_name);
  } else if (dir == CRAS_STREAM_INPUT) {
    mixer_name = ucm_get_capture_mixer_elem_for_dev(mgr, dev_name);
  }

  if (jack_dev) {
    jack_name = jack_dev;
    jack_type = "gpio";
  } else if (jack_control) {
    jack_name = jack_control;
    jack_type = "hctl";
  }

  dev_sec = ucm_section_create(dev_name, pcm_name, dev_idx, dependent_dev_idx,
                               dir, jack_name, jack_type);

  if (!dev_sec) {
    syslog(LOG_ERR, "Failed to allocate memory.");
    rc = -ENOMEM;
    goto error_cleanup;
  }

  dev_sec->jack_switch = ucm_get_jack_switch_for_dev(mgr, dev_name);

  if (mixer_name) {
    rc = ucm_section_set_mixer_name(dev_sec, mixer_name);
    if (rc) {
      goto error_cleanup;
    }
  }

  m_name = ucm_get_mixer_names(mgr, dev_name, coupled_mixers, dir,
                               MIXER_NAME_VOLUME);
  ucm_section_concat_coupled(dev_sec, m_name);

  DL_APPEND(*sections, dev_sec);
  ucm_section_dump(dev_sec);
error_cleanup:
  free((void*)dev_name);
  free((void*)dependent_dev_name);
  free((void*)jack_dev);
  free((void*)jack_control);
  free((void*)mixer_name);
  free((void*)pcm_name);
  return rc;
}

struct ucm_section* ucm_get_sections(struct cras_use_case_mgr* mgr) {
  struct ucm_section* sections = NULL;
  const char** list;
  int num_devs;
  int i;
  char* identifier;

  /* Find the list of all mixers using the control names defined in
   * the header definintion for this function.  */
  identifier = snd_use_case_identifier("_devices/%s", uc_verb(mgr));
  num_devs = snd_use_case_get_list(mgr->mgr, identifier, &list);
  free(identifier);

  if (num_devs < 0) {
    syslog(LOG_ERR, "Failed to get ucm sections: %d", num_devs);
    return NULL;
  }

  /* snd_use_case_get_list fills list with pairs of device name and
   * comment, so device names are in even-indexed elements. */
  for (i = 0; i < num_devs; i += 2) {
    if (ucm_parse_device_section(mgr, list[i], &sections) < 0) {
      ucm_section_free_list(sections);
      sections = NULL;
      break;
    }
  }

  if (num_devs > 0) {
    snd_use_case_free_list(list, num_devs);
  }
  return sections;
}

char* ucm_get_hotword_models(struct cras_use_case_mgr* mgr) {
  const char** list;
  int i, num_entries;
  int models_len = 0;
  int ret = -ENOMEM;
  char* models = NULL;
  const char* model_name;
  char* identifier;
  size_t buf_size;

  identifier = snd_use_case_identifier("_modifiers/%s", uc_verb(mgr));
  if (!identifier) {
    goto err;
  }

  num_entries = snd_use_case_get_list(mgr->mgr, identifier, &list);
  free(identifier);
  if (num_entries < 0) {
    ret = num_entries;
    goto err;
  }

  if (num_entries == 0) {
    return NULL;
  }

  buf_size = num_entries * (CRAS_MAX_HOTWORD_MODEL_NAME_SIZE + 1);
  models = (char*)malloc(buf_size);
  if (!models) {
    snd_use_case_free_list(list, num_entries);
    goto err;
  }

  for (i = 0; i < num_entries; i += 2) {
    if (!list[i]) {
      continue;
    }

    if (strncmp(list[i], hotword_model_prefix, strlen(hotword_model_prefix))) {
      continue;
    }

    model_name = list[i] + strlen(hotword_model_prefix);
    while (isspace(*model_name)) {
      model_name++;
    }

    if (strlen(model_name) > CRAS_MAX_HOTWORD_MODEL_NAME_SIZE) {
      syslog(LOG_ERR,
             "Ignore hotword model %s because the it is"
             "too long.",
             list[i]);
      continue;
    }

    if (models_len != 0) {
      models[models_len++] = ',';
    }

    strlcpy(models + models_len, model_name, buf_size - models_len);
    models_len += strlen(model_name);
  }
  models[models_len++] = 0;
  snd_use_case_free_list(list, num_entries);

  return models;
err:
  syslog(LOG_WARNING, "Failed to get hotword due to error: %d", ret);
  return NULL;
}

void ucm_disable_all_hotword_models(struct cras_use_case_mgr* mgr) {
  const char** list;
  int num_enmods, mod_idx;

  if (!mgr) {
    return;
  }

  // Disable all currently enabled hotword model modifiers.
  num_enmods = snd_use_case_get_list(mgr->mgr, "_enamods", &list);
  if (num_enmods < 0) {
    syslog(LOG_WARNING, "Failed to get hotword model list: %d", num_enmods);
  }
  if (num_enmods <= 0) {
    return;
  }

  for (mod_idx = 0; mod_idx < num_enmods; mod_idx++) {
    if (!strncmp(list[mod_idx], hotword_model_prefix,
                 strlen(hotword_model_prefix))) {
      ucm_set_modifier_enabled(mgr, list[mod_idx], 0);
    }
  }
  snd_use_case_free_list(list, num_enmods);
}

int ucm_enable_hotword_model(struct cras_use_case_mgr* mgr) {
  long mod_status;
  int ret;

  if (!mgr->hotword_modifier) {
    return -EINVAL;
  }

  ret = modifier_enabled(mgr, mgr->hotword_modifier, &mod_status);
  if (ret < 0) {
    return ret;
  }

  if (!mod_status) {
    return ucm_set_modifier_enabled(mgr, mgr->hotword_modifier, 1);
  }

  return -EINVAL;
}

int ucm_set_hotword_model(struct cras_use_case_mgr* mgr, const char* model) {
  char* model_mod;
  long mod_status = 0;
  size_t model_mod_size = strlen(model) + 1 + strlen(hotword_model_prefix) + 1;

  model_mod = (char*)malloc(model_mod_size);

  if (!model_mod) {
    return -ENOMEM;
  }
  snprintf(model_mod, model_mod_size, "%s %s", hotword_model_prefix, model);
  if (!ucm_mod_exists_with_name(mgr, model_mod)) {
    free((void*)model_mod);
    return -EINVAL;
  }

  if (mgr->hotword_modifier &&
      !strncmp(mgr->hotword_modifier, model_mod, model_mod_size)) {
    free((void*)model_mod);
    return 0;
  }

  // If check failed, just move on, dont fail incoming model
  if (mgr->hotword_modifier) {
    modifier_enabled(mgr, mgr->hotword_modifier, &mod_status);
  }

  ucm_disable_all_hotword_models(mgr);
  free(mgr->hotword_modifier);
  mgr->hotword_modifier = model_mod;
  if (mod_status) {
    return ucm_set_modifier_enabled(mgr, mgr->hotword_modifier, 1);
  }
  return 0;
}

int ucm_has_fully_specified_ucm_flag(struct cras_use_case_mgr* mgr) {
  char* flag;
  int ret = 0;
  flag = ucm_get_flag(mgr, fully_specified_ucm_var);
  if (!flag) {
    return 0;
  }
  ret = !strcmp(flag, "1");
  free(flag);
  return ret;
}

inline const char* ucm_get_playback_mixer_elem_for_dev(
    struct cras_use_case_mgr* mgr,
    const char* dev) {
  return ucm_get_value_for_dev(mgr, playback_mixer_elem_var, dev);
}

inline const char* ucm_get_capture_mixer_elem_for_dev(
    struct cras_use_case_mgr* mgr,
    const char* dev) {
  return ucm_get_value_for_dev(mgr, capture_mixer_elem_var, dev);
}

struct mixer_name* ucm_get_main_volume_names(struct cras_use_case_mgr* mgr) {
  return ucm_get_mixer_names(mgr, "", main_volume_names, CRAS_STREAM_OUTPUT,
                             MIXER_NAME_MAIN_VOLUME);
}

int ucm_list_section_devices_by_device_name(
    struct cras_use_case_mgr* mgr,
    enum CRAS_STREAM_DIRECTION direction,
    const char* device_name,
    ucm_list_section_devices_callback cb,
    void* cb_arg) {
  int listed = 0;
  struct section_name *section_names, *c;
  const char* var;
  char* identifier;

  if (direction == CRAS_STREAM_OUTPUT) {
    var = playback_device_name_var;
  } else if (direction == CRAS_STREAM_INPUT) {
    var = capture_device_name_var;
  } else {
    return 0;
  }

  identifier = snd_use_case_identifier("_devices/%s", uc_verb(mgr));
  section_names =
      ucm_get_sections_for_var(mgr, var, device_name, identifier, direction);
  free(identifier);
  if (!section_names) {
    return 0;
  }

  DL_FOREACH (section_names, c) {
    cb(c->name, cb_arg);
    listed++;
  }

  DL_FOREACH (section_names, c) {
    DL_DELETE(section_names, c);
    free((void*)c->name);
    free(c);
  }
  return listed;
}

inline const char* ucm_get_jack_control_for_dev(struct cras_use_case_mgr* mgr,
                                                const char* dev) {
  return ucm_get_value_for_dev(mgr, jack_control_var, dev);
}

inline const char* ucm_get_jack_dev_for_dev(struct cras_use_case_mgr* mgr,
                                            const char* dev) {
  return ucm_get_value_for_dev(mgr, jack_dev_var, dev);
}

int ucm_get_jack_switch_for_dev(struct cras_use_case_mgr* mgr,
                                const char* dev) {
  int value;

  int rc = get_int(mgr, jack_switch_var, dev, uc_verb(mgr), &value);
  if (rc || value < 0) {
    return JACK_SWITCH_AUTO_DETECT;
  }
  return value;
}

unsigned int ucm_get_dma_period_for_dev(struct cras_use_case_mgr* mgr,
                                        const char* dev) {
  int value;

  int rc = get_int(mgr, dma_period_var, dev, uc_verb(mgr), &value);
  if (rc || value < 0) {
    return 0;
  }
  return value;
}
