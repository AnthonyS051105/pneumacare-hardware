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

// Menambah satu pasang sample PPG (red + infrared, MAX30102 dual-wavelength)
// ke buffer batch internal (non-blocking, hanya menambah ke array — publish
// sesungguhnya terjadi di mqtt_client_tick() saat batch penuh).
//
// INTEGRATION_CONTRACT.md §3.3 (revisi 17 Agt 2026): red WAJIB dikirim
// berpasangan dengan ir pada index yang sama supaya backend bisa menghitung
// SpO2 (rasio-of-ratios red/infrared) — bukan hanya HR.
//
// PENTING (Fase 4, dual-core): fungsi ini HARUS hanya dipanggil dari task
// network (Core 1) — sample PPG yang dibaca task acquisition (Core 0)
// dikirim lewat FreeRTOS queue (lihat main.cpp, g_ppg_sample_queue) dan
// di-drain di task network sebelum dipanggilkan ke fungsi ini. Ini menjaga
// g_ppg_batch/g_ppg_batch_count di mqtt_client.cpp tetap hanya diakses dari
// satu core, tanpa perlu lock manual tambahan di modul ini.
void mqtt_client_push_ppg_sample(uint32_t red_sample, uint32_t ir_sample);

// Publish heartbeat/status (§3.4) — dipanggil berkala sesuai
// HEARTBEAT_INTERVAL_MS dari loop utama, BUKAN otomatis di dalam tick().
void mqtt_client_publish_heartbeat();

// Dipanggil berkala dari loop utama untuk reconnect otomatis dengan
// exponential backoff (FR-HW-024) saat koneksi MQTT terputus.
void mqtt_client_reconnect_tick();
