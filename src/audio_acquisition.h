#pragma once

#include <stddef.h>
#include <stdint.h>

// Driver I2S — FR-HW-001/003. Saat ini: PoC 1-channel dari mikrofon INMP441 #1
// (lihat SDD_HARDWARE.md §5, ARCHITECTURE_HARDWARE.md §4). Perluasan ke 4-channel
// menyusul setelah PoC 1-channel tervalidasi bersih.
//
// Pin dikonfirmasi manual oleh Tony (2026-08-12) untuk mic #1 — BUKAN nilai final
// PIN_MAPPING_BOM.md §1.1 (masih sebagian TODO di sana). Update PIN_MAPPING_BOM.md
// begitu Alfito konfirmasi skematik final.
//
// audio_acquisition_read() mengembalikan SAMPLE MENTAH individual (bukan
// agregat/statistik) — penting untuk Model A backend (mel-spectrogram butuh
// bentuk gelombang asli, bukan ringkasan volume per batch).

#define AUDIO_ACQUISITION_BATCH_LEN 512 // jumlah sample per pembacaan i2s_read()

struct AudioReadResult {
  size_t samples_read;
  int32_t samples[AUDIO_ACQUISITION_BATCH_LEN]; // sample mentah, sudah di-shift dari 24-bit
};

void audio_acquisition_init();

// Blocking: membaca satu batch sample mentah dari I2S. Konversi 24-bit ->
// wide int32 (shift >>8) sudah dilakukan di sini; konversi int32 -> int16 LE
// untuk transmisi dilakukan di ws_client (lihat catatan di ws_client.h).
AudioReadResult audio_acquisition_read();
