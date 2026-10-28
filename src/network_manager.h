#pragma once

#include <stdbool.h>
#include <stdint.h>

// Wi-Fi connect + NTP sync — FR-HW-020/025, SDD_HARDWARE.md §3.
//
// Non-blocking: init hanya memulai koneksi; status dipantau lewat
// network_manager_tick() dari loop task network. (Versi lama memblokir task
// network tanpa batas sampai WiFi tersambung, sehingga LED status, watchdog,
// dan log tidak bisa berjalan selama itu.)
//
// NTP memakai SNTP bawaan ESP-IDF yang berjalan di latar belakang dan
// sinkron ulang berkala sendiri; best-effort (FR-HW-025 "SEBAIKNYA").

void network_manager_init();

// Dipanggil berkala: reconnect WiFi dengan exponential backoff (FR-HW-024)
// dan mencatat transisi koneksi ke serial log.
void network_manager_tick();

bool network_manager_is_connected();

// Lama WiFi terputus terus-menerus (ms), 0 bila sedang terhubung.
uint32_t network_manager_disconnected_duration_ms();

int network_manager_rssi();

// Unix epoch ms. Bila jam belum tersinkron NTP, fallback ke millis() sejak boot
// (tidak absolut) — backend memakai waktu terima bila timestamp tidak masuk akal.
uint64_t network_manager_get_epoch_ms();

bool network_manager_is_time_synced();
