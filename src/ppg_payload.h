#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Penyusun payload MQTT — INTEGRATION_CONTRACT.md §3.3 (PPG) & §3.4 (status).
// Dipisah dari mqtt_client supaya skema bisa diuji unit test tanpa broker.

// §3.3: "samples" = infrared, "samples_red" = red, panjang & urutan identik.
// Mengembalikan panjang JSON, atau 0 bila out_cap tidak cukup.
size_t ppg_payload_build(char *out, size_t out_cap, const char *device_id, uint64_t timestamp_ms,
                         uint32_t sample_rate_hz, const uint32_t *samples_ir,
                         const uint32_t *samples_red, size_t n_samples);

// §3.4: status "online"/"offline". include_battery=false menghilangkan field
// battery_pct (dipakai Last Will "offline", supaya level baterai terakhir di
// backend tidak tertimpa nilai kosong). battery_pct -1 = sensor tidak tersedia.
size_t status_payload_build(char *out, size_t out_cap, const char *device_id, const char *status,
                            uint64_t timestamp_ms, bool include_battery, int battery_pct);
