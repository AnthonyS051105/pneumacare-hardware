#include "noise_filter.h"

#include <math.h>
#include <config.h>

// Biquad Direct Form II Transposed — stabil secara numerik di float 32-bit
// (FPU hardware ESP32), cukup presisi untuk input 24-bit dengan pole HPF 50 Hz.
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;
  bool enabled;
};

static Biquad g_hpf[NUM_AUDIO_CHANNELS];
static Biquad g_lpf[NUM_AUDIO_CHANNELS];

static const float kButterworthQ = 0.70710678f;
static const float kPi = 3.14159265f;

// Koefisien RBJ Audio EQ Cookbook, dinormalisasi terhadap a0.
static Biquad design_biquad(float fs, float fc, bool high_pass) {
  Biquad bq = {};
  if (fc <= 0.0f || fc >= fs * 0.5f) {
    bq.enabled = false;
    return bq;
  }

  float w0 = 2.0f * kPi * fc / fs;
  float cos_w0 = cosf(w0);
  float alpha = sinf(w0) / (2.0f * kButterworthQ);
  float a0 = 1.0f + alpha;

  if (high_pass) {
    bq.b0 = (1.0f + cos_w0) / 2.0f / a0;
    bq.b1 = -(1.0f + cos_w0) / a0;
    bq.b2 = (1.0f + cos_w0) / 2.0f / a0;
  } else {
    bq.b0 = (1.0f - cos_w0) / 2.0f / a0;
    bq.b1 = (1.0f - cos_w0) / a0;
    bq.b2 = (1.0f - cos_w0) / 2.0f / a0;
  }
  bq.a1 = -2.0f * cos_w0 / a0;
  bq.a2 = (1.0f - alpha) / a0;
  bq.enabled = true;
  return bq;
}

static inline float biquad_process(Biquad &bq, float x) {
  if (!bq.enabled) return x;
  float y = bq.b0 * x + bq.z1;
  bq.z1 = bq.b1 * x - bq.a1 * y + bq.z2;
  bq.z2 = bq.b2 * x - bq.a2 * y;
  return y;
}

void noise_filter_init(float sample_rate_hz, float hpf_cutoff_hz, float lpf_cutoff_hz) {
  Biquad hpf = design_biquad(sample_rate_hz, hpf_cutoff_hz, true);
  Biquad lpf = design_biquad(sample_rate_hz, lpf_cutoff_hz, false);
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    g_hpf[ch] = hpf;
    g_lpf[ch] = lpf;
  }
}

void noise_filter_reset() {
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    g_hpf[ch].z1 = g_hpf[ch].z2 = 0.0f;
    g_lpf[ch].z1 = g_lpf[ch].z2 = 0.0f;
  }
}

float noise_filter_apply(uint8_t channel_index, float sample) {
  if (channel_index >= NUM_AUDIO_CHANNELS) return sample;
  return biquad_process(g_lpf[channel_index], biquad_process(g_hpf[channel_index], sample));
}
