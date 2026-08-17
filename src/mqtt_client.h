#pragma once

#include <stdbool.h>
#include <stdint.h>

// MQTT publish PPG + status — FR-HW-022/023, INTEGRATION_CONTRACT.md §3.
// Topik & payload sesuai §3.2-§3.4 (tidak ada opsi ganda di bagian ini,
// beda dengan §2.3 sebelumnya).

void mqtt_client_init();

bool mqtt_client_is_connected();

// Non-blocking: dipanggil berkala dari loop utama. Menangani PubSubClient
// loop() (wajib untuk keepalive) dan mem-buffer sample PPG yang datang
// lewat mqtt_client_push_ppg_sample() sampai PPG_BATCH_SIZE tercapai, lalu
// publish batch ke topik .../ppg/raw (§3.3).
void mqtt_client_tick();

// Dipanggil dari task acquisition (nanti) atau harness test setiap ada
// sample PPG baru (mis. dari ppg_acquisition_read()). Non-blocking, hanya
// menambah ke buffer internal — publish sesungguhnya terjadi di
// mqtt_client_tick() saat batch penuh.
void mqtt_client_push_ppg_sample(uint32_t ir_sample);

// Publish heartbeat/status (§3.4) — dipanggil berkala sesuai
// HEARTBEAT_INTERVAL_MS dari loop utama, BUKAN otomatis di dalam tick().
void mqtt_client_publish_heartbeat();

// Dipanggil berkala dari loop utama untuk reconnect otomatis dengan
// exponential backoff (FR-HW-024) saat koneksi MQTT terputus.
void mqtt_client_reconnect_tick();
