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

struct AudioReadResult {
  size_t samples_read;
  int32_t min_val;
  int32_t max_val;
  int32_t avg_abs;
};

void audio_acquisition_init();

// Blocking: membaca satu batch sample dari I2S dan menghitung statistik dasar
// (min/max/avg_abs) untuk keperluan PoC. Belum ada filtering/buffering — itu
// modul noise_filter/buffer_manager di Fase 2.
AudioReadResult audio_acquisition_read();
