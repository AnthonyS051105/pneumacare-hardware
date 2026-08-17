#pragma once

#include <stdbool.h>
#include <stdint.h>

// Wi-Fi connect + NTP sync — FR-HW-020/025, SDD_HARDWARE.md §3 (state machine
// WIFI_CONNECTING -> NTP_SYNC -> SENSOR_INIT -> STREAMING).
//
// NTP bersifat best-effort (FR-HW-025 "SEBAIKNYA"): kegagalan NTP TIDAK
// menghentikan boot, firmware tetap lanjut ke streaming, hanya dicatat log.

// Blocking: menunggu koneksi WiFi awal (dipanggil sekali saat boot, sebelum
// STREAMING). Untuk reconnect setelah putus di tengah jalan, panggil
// network_manager_reconnect_tick() secara berkala dari loop non-blocking.
void network_manager_init();

bool network_manager_is_connected();

// Dipanggil berkala dari loop utama (Core 1 nanti). Non-blocking: memakai
// reconnect_manager (exponential backoff) internal, hanya mencoba connect
// ulang saat WiFi terputus DAN backoff delay sudah lewat.
void network_manager_reconnect_tick();

// Unix epoch ms saat ini. Sumber: NTP bila sinkron berhasil (lihat
// network_manager_is_time_synced()), fallback ke millis() sejak boot (tidak
// akurat secara absolut) bila NTP belum/tidak pernah berhasil — FR-HW-025.
uint64_t network_manager_get_epoch_ms();

bool network_manager_is_time_synced();
