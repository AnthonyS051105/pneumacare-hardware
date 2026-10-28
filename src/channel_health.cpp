#include "channel_health.h"

static void reset_window(ChannelHealth *h) {
  h->window_min = INT32_MAX;
  h->window_max = INT32_MIN;
  h->window_samples = 0;
}

void channel_health_init(ChannelHealth *h, uint32_t window_length_samples, int32_t p2p_threshold) {
  h->window_length = window_length_samples;
  h->p2p_threshold = p2p_threshold;
  h->last_p2p = 0;
  h->faulty = false;
  h->evaluated = false;
  h->fault_transitions = 0;
  reset_window(h);
}

bool channel_health_update(ChannelHealth *h, const int32_t *samples, size_t n) {
  bool changed = false;

  for (size_t i = 0; i < n; i++) {
    int32_t s = samples[i];
    if (s < h->window_min) h->window_min = s;
    if (s > h->window_max) h->window_max = s;
    h->window_samples++;

    if (h->window_samples >= h->window_length) {
      h->last_p2p = h->window_max - h->window_min;
      bool now_faulty = h->last_p2p < h->p2p_threshold;
      if (now_faulty != h->faulty) {
        h->faulty = now_faulty;
        h->fault_transitions++;
        changed = true;
      }
      h->evaluated = true;
      reset_window(h);
    }
  }

  return changed;
}
