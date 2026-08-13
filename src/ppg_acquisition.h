#pragma once

#include <stdbool.h>
#include <stdint.h>

// Driver I2C sensor PPG — FR-HW-002. Chip aktual: MAX30102 (dikonfirmasi Tony,
// 2026-08-13) — dual-wavelength (Red + Infrared), secara hardware mendukung
// estimasi SpO2 di backend selain HR (lihat SDD_HARDWARE.md §6, SDD_SOFTWARE.md §7).
// Firmware hanya membaca raw sample, TIDAK melakukan filtering/estimasi HR/SpO2
// (itu ada di backend — lihat PRD_HARDWARE.md §1).
//
// Pin I2C default ESP32 (SDA=21, SCL=22) dipakai untuk PoC ini — BUKAN nilai
// final PIN_MAPPING_BOM.md §1.2 (masih placeholder TODO di sana untuk pin dan
// part number). Update PIN_MAPPING_BOM.md begitu Alfito konfirmasi skematik final.

struct PpgReadResult {
  bool has_sample;
  uint32_t red;
  uint32_t ir;
};

// Mengembalikan false bila sensor tidak terdeteksi di bus I2C (mis. salah wiring
// atau alamat I2C tidak sesuai) — caller harus menangani kegagalan ini.
bool ppg_acquisition_init();

// Non-blocking: mengembalikan has_sample=false bila belum ada sample baru di FIFO.
PpgReadResult ppg_acquisition_read();
