// Noise filter (FR-HW-010), konversi PCM int16 (INTEGRATION_CONTRACT.md §2.3),
// dan deteksi channel mati.

#include <math.h>
#include <unity.h>

#include <config.h>

#include "audio_pcm.h"
#include "channel_health.h"
#include "noise_filter.h"

static const float kFs = 16000.0f;
static const float kPi = 3.14159265f;

void setUp() {}
void tearDown() {}

// Amplitudo puncak keluaran filter untuk sinus frekuensi f (setelah transien).
static float filtered_sine_peak(float freq_hz, float hpf_hz, float lpf_hz) {
  noise_filter_init(kFs, hpf_hz, lpf_hz);
  const int total = 16000; // 1 detik
  float peak = 0.0f;
  for (int n = 0; n < total; n++) {
    float x = 10000.0f * sinf(2.0f * kPi * freq_hz * n / kFs);
    float y = noise_filter_apply(0, x);
    if (n > total / 2 && fabsf(y) > peak) peak = fabsf(y);
  }
  return peak / 10000.0f;
}

void test_hpf_removes_dc_offset() {
  noise_filter_init(kFs, 50.0f, 0.0f);
  float y = 0.0f;
  for (int n = 0; n < 16000; n++) y = noise_filter_apply(0, 50000.0f);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, y);
}

void test_hpf_passes_breath_band_and_attenuates_rumble() {
  // Pita suara napas (~100-2000 Hz) lolos hampir utuh.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, filtered_sine_peak(1000.0f, 50.0f, 0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.10f, 1.0f, filtered_sine_peak(200.0f, 50.0f, 0.0f));
  // Cutoff Butterworth = -3 dB (0.707).
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.707f, filtered_sine_peak(50.0f, 50.0f, 0.0f));
  // Rumble 10 Hz teredam kuat (orde 2: ~-28 dB).
  TEST_ASSERT_LESS_THAN_FLOAT(0.06f, filtered_sine_peak(10.0f, 50.0f, 0.0f));
}

void test_optional_lpf() {
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, filtered_sine_peak(500.0f, 0.0f, 4000.0f));
  TEST_ASSERT_LESS_THAN_FLOAT(0.3f, filtered_sine_peak(7500.0f, 0.0f, 4000.0f));
}

void test_disabled_filter_is_passthrough() {
  noise_filter_init(kFs, 0.0f, 0.0f);
  TEST_ASSERT_EQUAL_FLOAT(1234.0f, noise_filter_apply(0, 1234.0f));
}

void test_filter_state_is_per_channel() {
  noise_filter_init(kFs, 50.0f, 0.0f);
  for (int n = 0; n < 1000; n++) noise_filter_apply(0, 30000.0f);
  // Channel 1 belum pernah menerima sample -> respons step pertama penuh.
  TEST_ASSERT_FLOAT_WITHIN(1500.0f, 30000.0f, noise_filter_apply(1, 30000.0f));
}

void test_pcm_gain_shift_and_rounding() {
  bool clipped = true;
  TEST_ASSERT_EQUAL_INT16(250, audio_pcm_from_sample24(1000.0f, 2, &clipped));
  TEST_ASSERT_FALSE(clipped);
  TEST_ASSERT_EQUAL_INT16(-250, audio_pcm_from_sample24(-1000.0f, 2, &clipped));
  TEST_ASSERT_EQUAL_INT16(3, audio_pcm_from_sample24(10.0f, 2, &clipped)); // 2.5 -> 3
  TEST_ASSERT_EQUAL_INT16(-3, audio_pcm_from_sample24(-10.0f, 2, &clipped));
  // shift 8 = 16 bit teratas dari 24 bit.
  TEST_ASSERT_EQUAL_INT16(32767, audio_pcm_from_sample24(8388352.0f, 8, &clipped));
}

void test_pcm_saturates_instead_of_wrapping() {
  bool clipped = false;
  TEST_ASSERT_EQUAL_INT16(32767, audio_pcm_from_sample24(8388607.0f, 0, &clipped));
  TEST_ASSERT_TRUE(clipped);
  TEST_ASSERT_EQUAL_INT16(-32768, audio_pcm_from_sample24(-8388608.0f, 0, &clipped));
  TEST_ASSERT_TRUE(clipped);
}

void test_pcm_block_counts_clipping() {
  float in[4] = {100.0f, 200000.0f, -200000.0f, -100.0f};
  int16_t out[4];
  TEST_ASSERT_EQUAL_UINT32(2, audio_pcm_convert_block(in, out, 4, 2));
  TEST_ASSERT_EQUAL_INT16(25, out[0]);
  TEST_ASSERT_EQUAL_INT16(32767, out[1]);
  TEST_ASSERT_EQUAL_INT16(-32768, out[2]);
  TEST_ASSERT_EQUAL_INT16(-25, out[3]);
}

void test_channel_health_detects_silent_mic_and_recovers() {
  ChannelHealth h;
  channel_health_init(&h, 100, AUDIO_DEAD_CHANNEL_P2P_THRESHOLD);
  TEST_ASSERT_FALSE(h.evaluated);

  int32_t zeros[100] = {0};
  TEST_ASSERT_TRUE(channel_health_update(&h, zeros, 100)); // mic lepas -> faulty
  TEST_ASSERT_TRUE(h.faulty);
  TEST_ASSERT_TRUE(h.evaluated);
  TEST_ASSERT_FALSE(channel_health_update(&h, zeros, 100)); // tetap faulty, tidak ada perubahan

  int32_t noise[100];
  for (int i = 0; i < 100; i++) noise[i] = (i % 2) ? 300 : -300; // noise floor mic asli
  TEST_ASSERT_TRUE(channel_health_update(&h, noise, 100));
  TEST_ASSERT_FALSE(h.faulty);
  TEST_ASSERT_EQUAL_INT32(600, h.last_p2p);
}

void test_channel_health_detects_stuck_value() {
  ChannelHealth h;
  channel_health_init(&h, 50, AUDIO_DEAD_CHANNEL_P2P_THRESHOLD);
  int32_t stuck[50];
  for (int i = 0; i < 50; i++) stuck[i] = 8388607; // terkunci full-scale
  channel_health_update(&h, stuck, 50);
  TEST_ASSERT_TRUE(h.faulty);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_hpf_removes_dc_offset);
  RUN_TEST(test_hpf_passes_breath_band_and_attenuates_rumble);
  RUN_TEST(test_optional_lpf);
  RUN_TEST(test_disabled_filter_is_passthrough);
  RUN_TEST(test_filter_state_is_per_channel);
  RUN_TEST(test_pcm_gain_shift_and_rounding);
  RUN_TEST(test_pcm_saturates_instead_of_wrapping);
  RUN_TEST(test_pcm_block_counts_clipping);
  RUN_TEST(test_channel_health_detects_silent_mic_and_recovers);
  RUN_TEST(test_channel_health_detects_stuck_value);
  return UNITY_END();
}
