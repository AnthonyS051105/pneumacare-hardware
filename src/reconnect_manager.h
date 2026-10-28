#pragma once

#include <stdbool.h>
#include <stdint.h>

// Exponential backoff generik — FR-HW-024, INTEGRATION_CONTRACT.md §2.4
// (1s, 2s, 4s, ... cap 30s). Dipakai independen oleh network_manager (WiFi),
// ws_client, dan mqtt_client — masing-masing punya instance sendiri (mis. WiFi
// putus tidak harus mereset backoff MQTT). Delay awal bisa berbeda per pemakai:
// WiFi butuh beberapa detik untuk asosiasi, jadi tidak boleh diputus tiap 1 detik.

struct ReconnectBackoff {
  uint32_t initial_delay_ms;
  uint32_t max_delay_ms;
  uint32_t current_delay_ms;
  uint32_t last_attempt_ms;
};

// Set parameter lalu reset.
void reconnect_backoff_init(ReconnectBackoff *backoff, uint32_t initial_delay_ms,
                            uint32_t max_delay_ms);

// Reset ke delay awal (dipanggil setelah koneksi berhasil).
void reconnect_backoff_reset(ReconnectBackoff *backoff);

// True bila delay sejak last_attempt_ms sudah terlampaui. Tidak mengubah state.
bool reconnect_backoff_should_attempt(const ReconnectBackoff *backoff, uint32_t now_ms);

// Dipanggil setiap kali percobaan reconnect dilakukan: mencatat waktu attempt
// dan menaikkan delay berikutnya (x RECONNECT_BACKOFF_MULTIPLIER, maks max_delay_ms).
void reconnect_backoff_notify_attempt(ReconnectBackoff *backoff, uint32_t now_ms);
