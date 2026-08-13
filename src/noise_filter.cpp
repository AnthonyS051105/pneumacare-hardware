#include "noise_filter.h"

#include <config.h>

// Koefisien single-pole high-pass (DC blocker): y[n] = x[n] - x[n-1] + alpha*y[n-1].
// alpha dekat 1.0 -> cutoff sangat rendah, cukup untuk buang DC offset/rumble
// tanpa memakan bandwidth sinyal napas/suara yang diinginkan. ⚠️ belum di-tuning
// dengan sinyal nyata — didiskusikan lebih lanjut sesuai catatan FR-HW-010.
#define DC_BLOCKER_ALPHA_NUM 63
#define DC_BLOCKER_ALPHA_DEN 64

struct DcBlockerState {
  int32_t prev_x;
  int32_t prev_y;
};

static DcBlockerState g_filter_state[NUM_AUDIO_CHANNELS];

void noise_filter_init() {
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    g_filter_state[ch].prev_x = 0;
    g_filter_state[ch].prev_y = 0;
  }
}

int32_t noise_filter_apply(uint8_t channel_index, int32_t raw_sample) {
  if (channel_index >= NUM_AUDIO_CHANNELS) return raw_sample;

  DcBlockerState &state = g_filter_state[channel_index];

  int32_t y = raw_sample - state.prev_x +
              (int32_t)(((int64_t)DC_BLOCKER_ALPHA_NUM * state.prev_y) /
                        DC_BLOCKER_ALPHA_DEN);

  state.prev_x = raw_sample;
  state.prev_y = y;

  return y;
}
