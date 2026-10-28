#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <config.h>

// Driver I2S 4-channel INMP441 — FR-HW-001/003, SDD_HARDWARE.md §5,
// ARCHITECTURE_HARDWARE.md §4 (risiko teknis utama).
//
// Wiring Alfito (2026-10-27): dua master RX stereo dengan clock terpisah.
// I2S0: SCK/WS/SD=26/25/33 (mic 1 kiri, 2 kanan).
// I2S1: SCK/WS/SD=14/27/32 (mic 3 kiri, 4 kanan).
// Keduanya memakai sample rate sama dan start berurutan. Offset fase antarbus
// belum terukur; jangan menganggap empat channel sinkron secara sample-exact.
//
// Bila NUM_AUDIO_CHANNELS hanya memakai unit 0 (mis. 1-2 mic), I2S1 tidak
// diinisialisasi sama sekali.

// Mengembalikan false bila driver gagal dipasang ATAU clock I2S tidak berjalan
// (pembacaan awal timeout). Membuang AUDIO_STARTUP_DISCARD_MS pertama.
bool audio_acquisition_init();

// Melepas driver I2S (aman dipanggil walau belum terpasang) — untuk init ulang
// setelah kegagalan tanpa reboot.
void audio_acquisition_deinit();

// Blocking (dipacu DMA): membaca satu blok untuk semua channel. out[ch][i] =
// sample 24-bit bertanda (sudah di-shift dari slot 32-bit). Mengembalikan
// jumlah frame per channel (sama untuk semua channel), 0 bila timeout/error.
size_t audio_acquisition_read(int32_t out[NUM_AUDIO_CHANNELS][AUDIO_I2S_FRAMES_PER_READ]);

uint8_t audio_acquisition_units_used();

// Diagnostik: jumlah pembacaan gagal/timeout dan event overflow antrean DMA
// (sample hilang karena task acquisition terlambat membaca).
uint32_t audio_acquisition_get_read_errors();
uint32_t audio_acquisition_get_dma_overflows();
