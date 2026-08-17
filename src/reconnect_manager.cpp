#include "reconnect_manager.h"

#include <config.h>

void reconnect_backoff_reset(ReconnectBackoff *backoff) {
  backoff->current_delay_ms = RECONNECT_BACKOFF_INITIAL_MS;
  backoff->last_attempt_ms = 0;
}

bool reconnect_backoff_should_attempt(const ReconnectBackoff *backoff, uint32_t now_ms) {
  return (now_ms - backoff->last_attempt_ms) >= backoff->current_delay_ms;
}

void reconnect_backoff_notify_attempt(ReconnectBackoff *backoff, uint32_t now_ms) {
  backoff->last_attempt_ms = now_ms;

  uint32_t next_delay = backoff->current_delay_ms * RECONNECT_BACKOFF_MULTIPLIER;
  backoff->current_delay_ms =
      (next_delay > RECONNECT_BACKOFF_MAX_MS) ? RECONNECT_BACKOFF_MAX_MS : next_delay;
}
