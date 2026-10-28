#include "reconnect_manager.h"

#include <config.h>

void reconnect_backoff_init(ReconnectBackoff *backoff, uint32_t initial_delay_ms,
                            uint32_t max_delay_ms) {
  backoff->initial_delay_ms = initial_delay_ms;
  backoff->max_delay_ms = max_delay_ms;
  reconnect_backoff_reset(backoff);
}

void reconnect_backoff_reset(ReconnectBackoff *backoff) {
  backoff->current_delay_ms = backoff->initial_delay_ms;
  backoff->last_attempt_ms = 0;
}

bool reconnect_backoff_should_attempt(const ReconnectBackoff *backoff, uint32_t now_ms) {
  // Pengurangan unsigned tetap benar saat millis() overflow (~49 hari).
  return (uint32_t)(now_ms - backoff->last_attempt_ms) >= backoff->current_delay_ms;
}

void reconnect_backoff_notify_attempt(ReconnectBackoff *backoff, uint32_t now_ms) {
  backoff->last_attempt_ms = now_ms;

  uint32_t next_delay = backoff->current_delay_ms * RECONNECT_BACKOFF_MULTIPLIER;
  backoff->current_delay_ms =
      (next_delay > backoff->max_delay_ms) ? backoff->max_delay_ms : next_delay;
}
