#include "audio_pcm.h"

int16_t audio_pcm_from_sample24(float sample24, uint8_t gain_shift, bool *clipped) {
  float scaled = sample24 / (float)(1UL << gain_shift);
  // Pembulatan ke integer terdekat (simetris untuk nilai negatif).
  float rounded = (scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f);

  if (rounded >= 32767.0f) {
    if (clipped) *clipped = true;
    return 32767;
  }
  if (rounded <= -32768.0f) {
    if (clipped) *clipped = true;
    return -32768;
  }
  if (clipped) *clipped = false;
  return (int16_t)(int32_t)rounded;
}

size_t audio_pcm_convert_block(const float *in, int16_t *out, size_t n, uint8_t gain_shift) {
  size_t clip_count = 0;
  for (size_t i = 0; i < n; i++) {
    bool clipped = false;
    out[i] = audio_pcm_from_sample24(in[i], gain_shift, &clipped);
    if (clipped) clip_count++;
  }
  return clip_count;
}
