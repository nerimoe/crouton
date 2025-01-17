// Copyright 2012 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <math.h>
#include <sys/param.h>

extern "C" {
#include "cras/src/server/cras_fmt_conv.h"
#include "cras_types.h"
}

static int mono_channel_layout[CRAS_CH_MAX] = {-1, -1, -1, -1, 0, -1,
                                               -1, -1, -1, -1, -1};
static int stereo_channel_layout[CRAS_CH_MAX] = {0,  1,  -1, -1, -1, -1,
                                                 -1, -1, -1, -1, -1};
static int surround_channel_center_layout[CRAS_CH_MAX] = {0,  1,  2,  3,  4, 5,
                                                          -1, -1, -1, -1, -1};
static int common_5_1_channel_center_layout[CRAS_CH_MAX] = {
    0, 1, 4, 5, 2, 3, -1, -1, -1, -1, -1};

static int surround_channel_left_right_layout[CRAS_CH_MAX] = {
    0, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int surround_channel_unknown_layout[CRAS_CH_MAX] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int quad_channel_layout[CRAS_CH_MAX] = {0,  1,  2,  3,  -1, -1,
                                               -1, -1, -1, -1, -1};
static int linear_resampler_needed_val;
static double linear_resampler_ratio = 1.0;
static unsigned int linear_resampler_num_channels;
static unsigned int linear_resampler_format_bytes;
static int linear_resampler_src_rate;
static int linear_resampler_dst_rate;

void ResetStub() {
  linear_resampler_needed_val = 0;
  linear_resampler_ratio = 1.0;
}

// Like malloc or calloc, but fill the memory with random bytes.
static void* ralloc(size_t size) {
  unsigned char* buf = (unsigned char*)malloc(size);
  while (size--) {
    buf[size] = rand() & 0xff;
  }
  return buf;
}

static void swap_channel_layout(int8_t* layout,
                                CRAS_CHANNEL a,
                                CRAS_CHANNEL b) {
  int8_t tmp = layout[a];
  layout[a] = layout[b];
  layout[b] = tmp;
}

TEST(FormatConverterTest, SmallFramesSRCWithLinearResampler) {
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;
  struct cras_fmt_conv* c;
  int16_t* in_buf;
  int16_t* out_buf;
  unsigned int in_frames = 1;
  unsigned int out_frames = 2;

  ResetStub();
  in_fmt.format = out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = out_fmt.num_channels = 1;
  in_fmt.frame_rate = 16000;
  out_fmt.frame_rate = 48000;
  linear_resampler_needed_val = 1;

  in_buf = (int16_t*)malloc(10 * 2 * 2);
  out_buf = (int16_t*)malloc(10 * 2 * 2);

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, 10, 1, CRAS_NODE_TYPE_LINEOUT);
  EXPECT_NE((void*)NULL, c);
  EXPECT_EQ(out_fmt.frame_rate, linear_resampler_src_rate);
  EXPECT_EQ(out_fmt.frame_rate, linear_resampler_dst_rate);

  /* When process on small buffers doing SRC 16KHz -> 48KHz,
   * speex does the work in two steps:
   *
   * (1) 0 -> 2 frames in output
   * (2) 1 -> 1 frame in output
   *
   * Total result is 1 frame consumed in input and generated
   * 3 frames in output.
   */
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buf, (uint8_t*)out_buf, &in_frames, out_frames);
  EXPECT_EQ(2, out_frames);
  EXPECT_EQ(0, in_frames);

  in_frames = 1;
  out_frames = 2;
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buf, (uint8_t*)out_buf, &in_frames, out_frames);
  EXPECT_EQ(1, out_frames);
  EXPECT_EQ(1, in_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buf);
  free(out_buf);
}

// Only support LE, BE should fail.
TEST(FormatConverterTest, InvalidParamsOnlyLE) {
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;
  struct cras_fmt_conv* c;

  ResetStub();
  in_fmt.format = out_fmt.format = SND_PCM_FORMAT_S32_BE;
  in_fmt.num_channels = out_fmt.num_channels = 2;
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, 4096, 0, CRAS_NODE_TYPE_LINEOUT);
  EXPECT_EQ(NULL, c);
}

// Test Mono to Stereo mix.
TEST(FormatConverterTest, MonoToStereo) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 1;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (size_t i = 0; i < buf_size; i++) {
    if (in_buff[i] != out_buff[i * 2] || in_buff[i] != out_buff[i * 2 + 1]) {
      EXPECT_TRUE(false);
      break;
    }
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test Stereo to Mono mix.
TEST(FormatConverterTest, StereoToMono) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  unsigned int i;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 1;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  for (i = 0; i < buf_size; i++) {
    in_buff[i * 2] = 13450;
    in_buff[i * 2 + 1] = -13449;
  }
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_EQ(1, out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test Stereo to Mono mix.  Overflow.
TEST(FormatConverterTest, StereoToMonoOverflow) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  unsigned int i;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 1;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  for (i = 0; i < buf_size; i++) {
    in_buff[i * 2] = 0x7fff;
    in_buff[i * 2 + 1] = 1;
  }
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_EQ(0x7fff, out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test Stereo to Mono mix.  Underflow.
TEST(FormatConverterTest, StereoToMonoUnderflow) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  unsigned int i;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 1;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  for (i = 0; i < buf_size; i++) {
    in_buff[i * 2] = -0x8000;
    in_buff[i * 2 + 1] = -1;
  }
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_EQ(-0x8000, out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test Stereo to Mono mix 24 and 32 bit.
TEST(FormatConverterTest, StereoToMono24bit) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int32_t* out_buff;
  unsigned int i;
  const size_t buf_size = 100;
  unsigned int in_buf_size = 100;
  unsigned int test;

  for (test = 0; test < 2; test++) {
    ResetStub();
    if (test == 0) {
      in_fmt.format = SND_PCM_FORMAT_S24_LE;
      out_fmt.format = SND_PCM_FORMAT_S24_LE;
    } else {
      in_fmt.format = SND_PCM_FORMAT_S32_LE;
      out_fmt.format = SND_PCM_FORMAT_S32_LE;
    }
    in_fmt.num_channels = 2;
    out_fmt.num_channels = 1;
    in_fmt.frame_rate = 48000;
    out_fmt.frame_rate = 48000;

    c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                             CRAS_NODE_TYPE_LINEOUT);
    ASSERT_NE(c, (void*)NULL);

    out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
    EXPECT_EQ(buf_size, out_frames);

    out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
    EXPECT_EQ(buf_size, out_frames);

    in_buff = (int32_t*)malloc(buf_size * cras_get_format_bytes(&in_fmt));
    out_buff = (int32_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
    // TODO(dgreid) - s/0x10000/1/ once it stays full bits the whole way.
    for (i = 0; i < buf_size; i++) {
      in_buff[i * 2] = 13450 << 16;
      in_buff[i * 2 + 1] = -in_buff[i * 2] + 0x10000;
    }
    out_frames = cras_fmt_conv_convert_frames(
        c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
    EXPECT_EQ(buf_size, out_frames);
    for (i = 0; i < buf_size; i++) {
      EXPECT_EQ(0x10000, out_buff[i]);
    }

    cras_fmt_conv_destroy(&c);
    free(in_buff);
    free(out_buff);
  }
}

// Test 5.1 to Stereo mix.
TEST(FormatConverterTest, SurroundToStereo) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  unsigned int i;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));

  /* Swap channel to FL = 13450, RL = -100.
   * Assert right channel is silent.
   */
  for (i = 0; i < buf_size; i++) {
    in_buff[i * 6] = 13450;
    in_buff[i * 6 + 1] = 0;
    in_buff[i * 6 + 2] = -100;
    in_buff[i * 6 + 3] = 0;
    in_buff[i * 6 + 4] = 0;
    in_buff[i * 6 + 5] = 0;
  }
  out_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_LT(0, out_buff[i * 2]);
  }
  cras_fmt_conv_destroy(&c);

  /* Swap channel to FR = 13450, RR = -100.
   * Assert left channel is silent.
   */
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_FL, CRAS_CH_FR);
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_RL, CRAS_CH_RR);
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_LT(0, out_buff[i * 2 + 1]);
  }
  cras_fmt_conv_destroy(&c);

  /* Swap channel to FC = 13450, LFE = -100.
   * Assert output left and right has equal magnitude.
   */
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_FR, CRAS_CH_FC);
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_RR, CRAS_CH_LFE);
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_NE(0, out_buff[i * 2]);
    EXPECT_EQ(out_buff[i * 2], out_buff[i * 2 + 1]);
  }
  cras_fmt_conv_destroy(&c);

  /* Swap channel to FR = 13450, FL = -100.
   * Assert output left is positive and right is negative. */
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_LFE, CRAS_CH_FR);
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_FC, CRAS_CH_FL);
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_LT(0, out_buff[i * 2]);
    EXPECT_GT(0, out_buff[i * 2 + 1]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 5.1 to Quad mix.
TEST(FormatConverterTest, SurroundToQuad) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  unsigned int i;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 4;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));

  const int16_t in_fl = 100;
  const int16_t in_fr = 200;
  const int16_t in_rl = 200;
  const int16_t in_rr = 300;
  const int16_t in_fc = 60;
  const int16_t in_lfe = 90;

  for (i = 0; i < buf_size; i++) {
    in_buff[i * 6 + CRAS_CH_FL] = in_fl;
    in_buff[i * 6 + CRAS_CH_FR] = in_fr;
    in_buff[i * 6 + CRAS_CH_RL] = in_rl;
    in_buff[i * 6 + CRAS_CH_RR] = in_rr;
    in_buff[i * 6 + CRAS_CH_FC] = in_fc;
    in_buff[i * 6 + CRAS_CH_LFE] = in_lfe;
  }
  out_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  // This is the sum of mtx[CRAS_CH_FL] coefficients.
  const float normalize_factor = 1.0 / (1 + 0.707 + 0.5);

  for (i = 0; i < buf_size; i++) {
    int16_t lfe = 0.5 * normalize_factor * in_lfe;
    int16_t center = 0.707 * normalize_factor * in_fc;
    int16_t fl = normalize_factor * in_fl + center + lfe;
    int16_t fr = normalize_factor * in_fr + center + lfe;
    int16_t rl = normalize_factor * in_rl + lfe;
    int16_t rr = normalize_factor * in_rr + lfe;

    EXPECT_EQ(fl, out_buff[i * 4 + CRAS_CH_FL]);
    EXPECT_EQ(fr, out_buff[i * 4 + CRAS_CH_FR]);
    EXPECT_EQ(rl, out_buff[i * 4 + CRAS_CH_RL]);
    EXPECT_EQ(rr, out_buff[i * 4 + CRAS_CH_RR]);
  }
  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test Quad to Stereo mix.
TEST(FormatConverterTest, QuadToStereo) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  unsigned int i;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 4;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = quad_channel_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));

  /*
   * Set left channel positive, right channel negative, assert values are
   * copied and scaled as expected.
   */
  for (i = 0; i < buf_size; i++) {
    in_buff[i * 4] = 800;
    in_buff[i * 4 + 1] = -800;
    in_buff[i * 4 + 2] = 80;
    in_buff[i * 4 + 3] = -80;
  }
  out_buff = (int16_t*)malloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));

  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_EQ(820, out_buff[i * 2]);
    EXPECT_EQ(-820, out_buff[i * 2 + 1]);
  }
  cras_fmt_conv_destroy(&c);

  /*
   * Swap left and right channels, check channel map is respected.
   */
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_FL, CRAS_CH_FR);
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_RL, CRAS_CH_RR);
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_EQ(-820, out_buff[i * 2]);
    EXPECT_EQ(820, out_buff[i * 2 + 1]);
  }
  cras_fmt_conv_destroy(&c);

  /*
   * Swap front and rear, check channel map is respected.
   */
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_FR, CRAS_CH_RR);
  swap_channel_layout(in_fmt.channel_layout, CRAS_CH_FL, CRAS_CH_RL);
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_EQ(-280, out_buff[i * 2]);
    EXPECT_EQ(280, out_buff[i * 2 + 1]);
  }
  cras_fmt_conv_destroy(&c);

  /*
   * Empty channel map, check default behavior is applied.
   */
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = -1;
  }
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (i = 0; i < buf_size; i++) {
    EXPECT_EQ(820, out_buff[i * 2]);
    EXPECT_EQ(-820, out_buff[i * 2 + 1]);
  }
  cras_fmt_conv_destroy(&c);

  free(in_buff);
  free(out_buff);
}

// Test 2 to 1 SRC.
TEST(FormatConverterTest, Convert2To1) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = out_fmt.num_channels = 2;
  in_fmt.frame_rate = 96000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size / 2, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size / 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size / 2);
  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 1 to 2 SRC.
TEST(FormatConverterTest, Convert1To2) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;
  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = out_fmt.num_channels = 2;
  in_fmt.frame_rate = 22050;
  out_fmt.frame_rate = 44100;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size * 2, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size * 2);
  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 1 to 2 SRC with mono to stereo conversion.
TEST(FormatConverterTest, Convert1To2MonoToStereo) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;
  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 1;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 22050;
  out_fmt.frame_rate = 44100;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_out_frames_to_in(c, buf_size);
  EXPECT_EQ(buf_size / 2, out_frames);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size * 2, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size * 2);
  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 32 to 16 bit conversion.
TEST(FormatConverterTest, ConvertS32LEToS16LE) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S32_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ((int16_t)(in_buff[i] >> 16), out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 24 to 16 bit conversion.
TEST(FormatConverterTest, ConvertS24LEToS16LE) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S24_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ((int16_t)(in_buff[i] >> 8), out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 8 to 16 bit conversion.
TEST(FormatConverterTest, ConvertU8LEToS16LE) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  uint8_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_U8;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (uint8_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ((int16_t)((uint16_t)((int16_t)(in_buff[i]) - 128) << 8),
              out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 to 32 bit conversion.
TEST(FormatConverterTest, ConvertS16LEToS32LE) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int32_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S32_LE;
  in_fmt.num_channels = out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ((int32_t)((uint32_t)(int32_t)in_buff[i] << 16), out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 to 24 bit conversion.
TEST(FormatConverterTest, ConvertS16LEToS24LE) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int32_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S24_LE;
  in_fmt.num_channels = out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int32_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ((int32_t)((uint32_t)(int32_t)in_buff[i] << 8), out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 to 8 bit conversion.
TEST(FormatConverterTest, ConvertS16LEToU8) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  uint8_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_U8;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (uint8_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ((in_buff[i] >> 8) + 128, out_buff[i]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 32 bit 5.1 to 16 bit stereo conversion.
TEST(FormatConverterTest, ConvertS32LEToS16LEDownmix51ToStereo) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S32_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 bit stereo to 5.1 conversion.
TEST(FormatConverterTest, ConvertS16LEToS16LEStereoTo51) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 6;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    out_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    // Check mono be converted to CRAS_CH_FL and CRAS_CH_FR
    EXPECT_EQ(in_buff[2 * i], out_buff[6 * i]);
    EXPECT_EQ(in_buff[2 * i + 1], out_buff[6 * i + 1]);
    EXPECT_EQ(0, out_buff[6 * i + 2]);
    EXPECT_EQ(0, out_buff[6 * i + 3]);
    EXPECT_EQ(0, out_buff[6 * i + 4]);
    EXPECT_EQ(0, out_buff[6 * i + 5]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 bit mono to 5.1 conversion.  Center.
TEST(FormatConverterTest, ConvertS16LEToS16LEMonoTo51Center) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 1;
  out_fmt.num_channels = 6;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    out_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    // Check mono be converted to CRAS_CH_FC
    EXPECT_EQ(in_buff[i], out_buff[6 * i + 4]);
    EXPECT_EQ(0, out_buff[6 * i + 0]);
    EXPECT_EQ(0, out_buff[6 * i + 1]);
    EXPECT_EQ(0, out_buff[6 * i + 2]);
    EXPECT_EQ(0, out_buff[6 * i + 3]);
    EXPECT_EQ(0, out_buff[6 * i + 5]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 bit mono to 5.1 conversion.  Left Right.
TEST(FormatConverterTest, ConvertS16LEToS16LEMonoTo51LeftRight) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  unsigned int i, left, right;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 1;
  out_fmt.num_channels = 6;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    out_fmt.channel_layout[i] = surround_channel_left_right_layout[i];
  }
  left = surround_channel_left_right_layout[CRAS_CH_FL];
  right = surround_channel_left_right_layout[CRAS_CH_FR];

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    // Check mono be converted to CRAS_CH_FL and CRAS_CH_FR
    for (unsigned int k = 0; k < 6; ++k) {
      if (k == left) {
        EXPECT_EQ(in_buff[i] / 2, out_buff[6 * i + left]);
      } else if (k == right) {
        EXPECT_EQ(in_buff[i] / 2, out_buff[6 * i + right]);
      } else {
        EXPECT_EQ(0, out_buff[6 * i + k]);
      }
    }
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 bit mono to 5.1 conversion.  Unknown.
TEST(FormatConverterTest, ConvertS16LEToS16LEMonoTo51Unknown) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 1;
  out_fmt.num_channels = 6;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    out_fmt.channel_layout[i] = surround_channel_unknown_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    // Check mono be converted to CRAS_CH_FL
    EXPECT_EQ(in_buff[i], out_buff[6 * i + 0]);
    EXPECT_EQ(0, out_buff[6 * i + 1]);
    EXPECT_EQ(0, out_buff[6 * i + 2]);
    EXPECT_EQ(0, out_buff[6 * i + 3]);
    EXPECT_EQ(0, out_buff[6 * i + 4]);
    EXPECT_EQ(0, out_buff[6 * i + 5]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 bit stereo to quad conversion.
TEST(FormatConverterTest, ConvertS16LEToS16LEStereoToQuad) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 4;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (unsigned int i = 0; i < CRAS_CH_MAX; i++) {
    out_fmt.channel_layout[i] = quad_channel_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&in_fmt));
  for (unsigned int i = 0; i < in_buf_size; i++) {
    in_buff[i * 2] = 40;
    in_buff[i * 2 + 1] = 80;
  }

  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ(40, out_buff[4 * i]);
    EXPECT_EQ(80, out_buff[4 * i + 1]);
    EXPECT_EQ(0, out_buff[4 * i + 2]);
    EXPECT_EQ(0, out_buff[4 * i + 3]);
  }
  cras_fmt_conv_destroy(&c);

  // Swap channels and check channel layout is respected.
  swap_channel_layout(out_fmt.channel_layout, CRAS_CH_FL, CRAS_CH_RR);
  swap_channel_layout(out_fmt.channel_layout, CRAS_CH_RL, CRAS_CH_FR);
  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ(0, out_buff[4 * i]);
    EXPECT_EQ(0, out_buff[4 * i + 1]);
    EXPECT_EQ(80, out_buff[4 * i + 2]);
    EXPECT_EQ(40, out_buff[4 * i + 3]);
  }

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 16 bit stereo to quad conversion.
TEST(FormatConverterTest,
     ConvertS16LEStereoToQuadInternalSpeakerDefaultLayout) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 4;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  for (unsigned int i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_left_right_layout[i];
    out_fmt.channel_layout[i] = quad_channel_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_INTERNAL_SPEAKER);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&in_fmt));
  for (unsigned int i = 0; i < in_buf_size; i++) {
    in_buff[i * 2] = 40;
    in_buff[i * 2 + 1] = 80;
  }

  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ(40, out_buff[4 * i]);
    EXPECT_EQ(80, out_buff[4 * i + 1]);
    EXPECT_EQ(40, out_buff[4 * i + 2]);
    EXPECT_EQ(80, out_buff[4 * i + 3]);
  }
  cras_fmt_conv_destroy(&c);

  free(in_buff);
  free(out_buff);
}

// Test 16 bit stereo to quad conversion.
TEST(FormatConverterTest, ConvertS16LEStereoToQuadInternalSpeaker) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 4;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  for (unsigned int i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_left_right_layout[i];
    out_fmt.channel_layout[i] = quad_channel_layout[i];
  }

  out_fmt.channel_layout[2] = 3;
  out_fmt.channel_layout[3] = 2;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_INTERNAL_SPEAKER);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&in_fmt));
  for (unsigned int i = 0; i < in_buf_size; i++) {
    in_buff[i * 2] = 40;
    in_buff[i * 2 + 1] = 80;
  }

  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ(40, out_buff[4 * i]);
    EXPECT_EQ(80, out_buff[4 * i + 1]);
    EXPECT_EQ(80, out_buff[4 * i + 2]);
    EXPECT_EQ(40, out_buff[4 * i + 3]);
  }
  cras_fmt_conv_destroy(&c);

  free(in_buff);
  free(out_buff);
}

// Test 16 bit surround to quad internal speaker conversion.
TEST(FormatConverterTest, ConvertS16LE5_1ToQuadInternalSpeaker) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 4;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  for (unsigned int i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
    out_fmt.channel_layout[i] = quad_channel_layout[i];
  }

  out_fmt.channel_layout[2] = 3;
  out_fmt.channel_layout[3] = 2;

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_INTERNAL_SPEAKER);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&in_fmt));
  for (unsigned int i = 0; i < in_buf_size; i++) {
    in_buff[i * 6] = 40;
    in_buff[i * 6 + 1] = 80;
    in_buff[i * 6 + 2] = 120;
    in_buff[i * 6 + 3] = 160;
    in_buff[i * 6 + 4] = 200;
    in_buff[i * 6 + 5] = 240;
  }

  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ(130, out_buff[4 * i]);
    EXPECT_EQ(154, out_buff[4 * i + 1]);
    EXPECT_EQ(154, out_buff[4 * i + 2]);
    EXPECT_EQ(130, out_buff[4 * i + 3]);
  }
  cras_fmt_conv_destroy(&c);

  free(in_buff);
  free(out_buff);
}

// Test 16 bit surround to quad internal speaker conversion.
TEST(FormatConverterTest, ConvertS16LE5_1Map2ToQuadInternalSpeaker) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int16_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 4;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;

  for (unsigned int i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = common_5_1_channel_center_layout[i];
    out_fmt.channel_layout[i] = quad_channel_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_INTERNAL_SPEAKER);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size, out_frames);

  in_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&in_fmt));
  for (unsigned int i = 0; i < in_buf_size; i++) {
    in_buff[i * 6] = 40;
    in_buff[i * 6 + 1] = 80;
    in_buff[i * 6 + 2] = 120;
    in_buff[i * 6 + 3] = 160;
    in_buff[i * 6 + 4] = 200;
    in_buff[i * 6 + 5] = 240;
  }

  out_buff = (int16_t*)malloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(buf_size, out_frames);
  for (unsigned int i = 0; i < buf_size; i++) {
    EXPECT_EQ(124, out_buff[4 * i]);
    EXPECT_EQ(148, out_buff[4 * i + 1]);
    EXPECT_EQ(124, out_buff[4 * i + 2]);
    EXPECT_EQ(148, out_buff[4 * i + 3]);
  }
  cras_fmt_conv_destroy(&c);

  free(in_buff);
  free(out_buff);
}

// Test 32 bit 5.1 to 16 bit stereo conversion with SRC 1 to 2.
TEST(FormatConverterTest, ConvertS32LEToS16LEDownmix51ToStereo48To96) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S32_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 96000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size * 2, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size * 2);
  EXPECT_EQ(buf_size * 2, out_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 32 bit 5.1 to 16 bit stereo conversion with SRC 2 to 1.
TEST(FormatConverterTest, ConvertS32LEToS16LEDownmix51ToStereo96To48) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S32_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 96000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size / 2, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size / 2 * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size / 2);
  EXPECT_EQ(buf_size / 2, out_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 32 bit 5.1 to 16 bit stereo conversion with SRC 48 to 44.1.
TEST(FormatConverterTest, ConvertS32LEToS16LEDownmix51ToStereo48To441) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  size_t ret_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S32_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 44100;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_LT(out_frames, buf_size);

  in_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(out_frames * cras_get_format_bytes(&out_fmt));
  ret_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, out_frames);
  EXPECT_EQ(out_frames, ret_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test 32 bit 5.1 to 16 bit stereo conversion with SRC 441 to 48.
TEST(FormatConverterTest, ConvertS32LEToS16LEDownmix51ToStereo441To48) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  size_t ret_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S32_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 44100;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_GT(out_frames, buf_size);

  in_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff =
      (int16_t*)ralloc((out_frames - 1) * cras_get_format_bytes(&out_fmt));
  ret_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, out_frames - 1);
  EXPECT_EQ(out_frames - 1, ret_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test Invalid buffer length just truncates.
TEST(FormatConverterTest, ConvertS32LEToS16LEDownmix51ToStereo96To48Short) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  size_t ret_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S32_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 6;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 96000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);

  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(buf_size / 2, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff =
      (int16_t*)ralloc((out_frames - 2) * cras_get_format_bytes(&out_fmt));
  ret_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, out_frames - 2);
  EXPECT_EQ(out_frames - 2, ret_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test format convert pre linear resample and then follows SRC from 96 to 48.
TEST(FormatConverterTest, Convert96to48PreLinearResample) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  unsigned int expected_fr;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 96000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
    out_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size * 2, 1,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);
  EXPECT_EQ(out_fmt.frame_rate, linear_resampler_src_rate);
  EXPECT_EQ(out_fmt.frame_rate, linear_resampler_dst_rate);

  linear_resampler_needed_val = 1;
  linear_resampler_ratio = 1.01;
  expected_fr = buf_size / 2 * linear_resampler_ratio;
  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(expected_fr, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, out_frames);
  EXPECT_EQ(expected_fr, out_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test format convert SRC from 96 to 48 and then post linear resample.
TEST(FormatConverterTest, Convert96to48PostLinearResample) {
  struct cras_fmt_conv* c;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  size_t out_frames;
  int32_t* in_buff;
  int16_t* out_buff;
  const size_t buf_size = 4096;
  unsigned int in_buf_size = 4096;
  unsigned int expected_fr;
  int i;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 96000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = surround_channel_center_layout[i];
    out_fmt.channel_layout[i] = surround_channel_center_layout[i];
  }

  c = cras_fmt_conv_create(&in_fmt, &out_fmt, buf_size * 2, 0,
                           CRAS_NODE_TYPE_LINEOUT);
  ASSERT_NE(c, (void*)NULL);
  EXPECT_EQ(out_fmt.frame_rate, linear_resampler_src_rate);
  EXPECT_EQ(out_fmt.frame_rate, linear_resampler_dst_rate);

  linear_resampler_needed_val = 1;
  linear_resampler_ratio = 0.99;
  expected_fr = buf_size / 2 * linear_resampler_ratio;
  out_frames = cras_fmt_conv_in_frames_to_out(c, buf_size);
  EXPECT_EQ(expected_fr, out_frames);

  in_buff = (int32_t*)ralloc(buf_size * cras_get_format_bytes(&in_fmt));
  out_buff = (int16_t*)ralloc(buf_size * cras_get_format_bytes(&out_fmt));
  out_frames = cras_fmt_conv_convert_frames(
      c, (uint8_t*)in_buff, (uint8_t*)out_buff, &in_buf_size, buf_size);
  EXPECT_EQ(expected_fr, out_frames);

  cras_fmt_conv_destroy(&c);
  free(in_buff);
  free(out_buff);
}

// Test format converter created in config_format_converter
TEST(FormatConverterTest, ConfigConverter) {
  int i;
  struct cras_fmt_conv* c = NULL;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 1;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 96000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = mono_channel_layout[i];
    out_fmt.channel_layout[i] = stereo_channel_layout[i];
  }

  config_format_converter(&c, CRAS_STREAM_OUTPUT, &in_fmt, &out_fmt,
                          CRAS_NODE_TYPE_HEADPHONE, 4096);
  ASSERT_NE(c, (void*)NULL);

  cras_fmt_conv_destroy(&c);
}

// Test format converter not created when in/out format conversion is not
// needed.
TEST(FormatConverterTest, ConfigConverterNoNeed) {
  int i;
  struct cras_fmt_conv* c = NULL;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 2;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = stereo_channel_layout[i];
    out_fmt.channel_layout[i] = stereo_channel_layout[i];
  }

  config_format_converter(&c, CRAS_STREAM_OUTPUT, &in_fmt, &out_fmt,
                          CRAS_NODE_TYPE_HEADPHONE, 4096);
  EXPECT_NE(c, (void*)NULL);
  EXPECT_EQ(0, cras_fmt_conversion_needed(c));
  cras_fmt_conv_destroy(&c);
}

// Test format converter not created for input when in/out format differs
// at channel count or layout.
TEST(FormatConverterTest, ConfigConverterNoNeedForInput) {
  static int kmic_channel_layout[CRAS_CH_MAX] = {0,  1,  -1, -1, 2, -1,
                                                 -1, -1, -1, -1, -1};
  int i;
  struct cras_fmt_conv* c = NULL;
  struct cras_audio_format in_fmt;
  struct cras_audio_format out_fmt;

  ResetStub();
  in_fmt.format = SND_PCM_FORMAT_S16_LE;
  out_fmt.format = SND_PCM_FORMAT_S16_LE;
  in_fmt.num_channels = 2;
  out_fmt.num_channels = 3;
  in_fmt.frame_rate = 48000;
  out_fmt.frame_rate = 48000;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    in_fmt.channel_layout[i] = stereo_channel_layout[i];
    out_fmt.channel_layout[i] = kmic_channel_layout[i];
  }

  config_format_converter(&c, CRAS_STREAM_INPUT, &in_fmt, &out_fmt,
                          CRAS_NODE_TYPE_HEADPHONE, 4096);
  EXPECT_NE(c, (void*)NULL);
  EXPECT_EQ(0, cras_fmt_conversion_needed(c));
  cras_fmt_conv_destroy(&c);
}

TEST(ChannelRemixTest, ChannelRemixAppliedOrNot) {
  float coeff[4] = {0.5, 0.5, 0.26, 0.73};
  struct cras_fmt_conv* conv;
  struct cras_audio_format fmt;
  int16_t *buf, *res;
  unsigned i;

  fmt.num_channels = 2;
  conv = cras_channel_remix_conv_create(2, coeff);

  buf = (int16_t*)ralloc(50 * 4);
  res = (int16_t*)malloc(50 * 4);

  memcpy(res, buf, 50 * 4);

  // Remix conversion will not apply for non S16_LE format.
  fmt.format = SND_PCM_FORMAT_S24_LE;
  cras_channel_remix_convert(conv, &fmt, (uint8_t*)buf, 50);
  for (i = 0; i < 100; i++) {
    EXPECT_EQ(res[i], buf[i]);
  }

  for (i = 0; i < 100; i += 2) {
    res[i] = coeff[0] * buf[i];
    res[i] += coeff[1] * buf[i + 1];
    res[i + 1] = coeff[2] * buf[i];
    res[i + 1] += coeff[3] * buf[i + 1];
  }

  fmt.format = SND_PCM_FORMAT_S16_LE;
  cras_channel_remix_convert(conv, &fmt, (uint8_t*)buf, 50);
  for (i = 0; i < 100; i++) {
    EXPECT_EQ(res[i], buf[i]);
  }

  // If num_channels not match, remix conversion will not apply.
  fmt.num_channels = 6;
  cras_channel_remix_convert(conv, &fmt, (uint8_t*)buf, 50);
  for (i = 0; i < 100; i++) {
    EXPECT_EQ(res[i], buf[i]);
  }

  cras_fmt_conv_destroy(&conv);
  free(buf);
  free(res);
}

extern "C" {
float** cras_channel_conv_matrix_alloc(size_t in_ch, size_t out_ch) {
  int i;
  float** conv_mtx;
  conv_mtx = (float**)calloc(CRAS_CH_MAX, sizeof(*conv_mtx));
  for (i = 0; i < CRAS_CH_MAX; i++) {
    conv_mtx[i] = (float*)calloc(CRAS_CH_MAX, sizeof(*conv_mtx[i]));
  }
  return conv_mtx;
}
void cras_channel_conv_matrix_destroy(float** mtx, size_t out_ch) {
  int i;
  for (i = 0; i < CRAS_CH_MAX; i++) {
    free(mtx[i]);
  }
  free(mtx);
}
float** cras_channel_conv_matrix_create(const struct cras_audio_format* in,
                                        const struct cras_audio_format* out) {
  return cras_channel_conv_matrix_alloc(in->num_channels, out->num_channels);
}
struct linear_resampler* linear_resampler_create(unsigned int num_channels,
                                                 unsigned int format_bytes,
                                                 float src_rate,
                                                 float dst_rate) {
  linear_resampler_format_bytes = format_bytes;
  linear_resampler_num_channels = num_channels;
  linear_resampler_src_rate = src_rate;
  linear_resampler_dst_rate = dst_rate;
  return reinterpret_cast<struct linear_resampler*>(0x33);
  ;
}

int linear_resampler_needed(struct linear_resampler* lr) {
  return linear_resampler_needed_val;
}

void linear_resampler_set_rates(struct linear_resampler* lr,
                                unsigned int from,
                                unsigned int to) {
  linear_resampler_src_rate = from;
  linear_resampler_dst_rate = to;
}

unsigned int linear_resampler_out_frames_to_in(struct linear_resampler* lr,
                                               unsigned int frames) {
  return (double)frames / linear_resampler_ratio;
}

// Converts the frames count from input rate to output rate.
unsigned int linear_resampler_in_frames_to_out(struct linear_resampler* lr,
                                               unsigned int frames) {
  return (double)frames * linear_resampler_ratio;
}

unsigned int linear_resampler_resample(struct linear_resampler* lr,
                                       uint8_t* src,
                                       unsigned int* src_frames,
                                       uint8_t* dst,
                                       unsigned dst_frames) {
  unsigned int resampled_fr = *src_frames * linear_resampler_ratio;

  if (resampled_fr > dst_frames) {
    resampled_fr = dst_frames;
    *src_frames = dst_frames / linear_resampler_ratio;
  }
  unsigned int resampled_bytes = resampled_fr * linear_resampler_format_bytes *
                                 linear_resampler_num_channels;
  for (size_t i = 0; i < resampled_bytes; i++) {
    dst[i] = (uint8_t)rand() & 0xff;
  }

  return resampled_fr;
}

void linear_resampler_destroy(struct linear_resampler* lr) {}
}  // extern "C"
