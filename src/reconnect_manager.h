#pragma once

#include <stdint.h>

// Exponential backoff generik — FR-HW-024, INTEGRATION_CONTRACT.md §2.4
// (1s, 2s, 4s, ... cap 30s). Dipakai independen oleh network_manager (WiFi),
// ws_client, dan mqtt_client — masing-masing punya state backoff sendiri
// (mis. WiFi putus tidak harus mereset backoff MQTT), jadi tiap pemanggil
// membuat instance ReconnectBackoff sendiri.

struct ReconnectBackoff {
  uint32_t current_delay_ms;
  uint32_t last_attempt_ms;
};

// Reset ke delay awal (dipanggil setelah koneksi berhasil).
void reconnect_backoff_reset(ReconnectBackoff *backoff);

// True bila sudah waktunya mencoba reconnect lagi (delay sejak last_attempt_ms
// terlampaui). Tidak menaikkan/reset apapun sendiri — caller yang menentukan
// kapan attempt terjadi dan memanggil reconnect_backoff_notify_attempt setelahnya.
bool reconnect_backoff_should_attempt(const ReconnectBackoff *backoff, uint32_t now_ms);

// Dipanggil setelah setiap percobaan reconnect (berhasil atau gagal) untuk
// mencatat waktu attempt & menaikkan delay berikutnya (dipanggil sebelum tahu
// hasil; reset terpisah lewat reconnect_backoff_reset bila berhasil).
void reconnect_backoff_notify_attempt(ReconnectBackoff *backoff, uint32_t now_ms);
