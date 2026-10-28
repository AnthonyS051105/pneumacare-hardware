#pragma once

// Indikator status lokal (PRD_HARDWARE.md §3, opsional) — pasien/perawat bisa
// tahu perangkat menyala & terhubung tanpa membuka dashboard.
//
// Pola kedip:
//   BOOT               : menyala terus
//   WIFI_CONNECTING    : kedip cepat 2 Hz
//   BACKEND_CONNECTING : kedip 1 Hz (WiFi OK, websocket/MQTT belum)
//   STREAMING          : kedip singkat tiap 2 detik (hemat daya)
//   SENSOR_FAULT       : kedip sangat cepat 8 Hz (mic/PPG bermasalah)

enum StatusLedMode {
  LED_MODE_BOOT,
  LED_MODE_WIFI_CONNECTING,
  LED_MODE_BACKEND_CONNECTING,
  LED_MODE_STREAMING,
  LED_MODE_SENSOR_FAULT,
};

void status_led_init();
void status_led_set_mode(StatusLedMode mode);
// Dipanggil sesering mungkin dari loop non-blocking (berbasis millis()).
void status_led_tick();
