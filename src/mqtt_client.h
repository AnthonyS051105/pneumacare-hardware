#pragma once

#include <stdbool.h>
#include <stdint.h>

// MQTT publish PPG + status — FR-HW-022/023, INTEGRATION_CONTRACT.md §3.
//
// - PPG: batch PPG_BATCH_SIZE sample (red+IR berpasangan, §3.3) -> .../ppg/raw.
// - Status: "online" retained tiap HEARTBEAT_INTERVAL_MS (§3.4), plus Last Will
//   "offline" retained yang dipublish BROKER saat device hilang mendadak (baterai
//   habis, WiFi mati) — dashboard langsung tahu tanpa menunggu timeout backend.
//
// Semua fungsi HANYA boleh dipanggil dari task network (Core 1). Sample PPG
// dari task acquisition (Core 0) dikirim lewat FreeRTOS queue (main.cpp).

struct MqttClientStats {
  uint32_t batches_published;
  uint32_t publish_failures;
  uint32_t samples_dropped; // dibuang karena broker tidak terhubung
  uint32_t connects;
};

// device_id disuntikkan (program test fase 3 memakai DIAG_DEVICE_ID). Pointer
// harus valid selamanya.
void mqtt_client_init(const char *device_id);

bool mqtt_client_is_connected();

// Menambah satu pasang sample. Bila batch penuh, batch langsung dipublish (atau
// dibuang bila broker tidak terhubung) sebelum sample baru ditambahkan.
void mqtt_client_push_ppg_sample(uint32_t red_sample, uint32_t ir_sample);

// Dipanggil berkala: keepalive MQTT, reconnect dengan exponential backoff
// (FR-HW-024), dan heartbeat status berkala (FR-HW-023). battery_pct -1 =
// sensor baterai tidak tersedia.
void mqtt_client_tick(int battery_pct);

void mqtt_client_get_stats(MqttClientStats *out);
