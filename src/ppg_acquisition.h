#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Driver I2C sensor PPG MAX30102 — FR-HW-002, SDD_HARDWARE.md §6.
// Part number dikonfirmasi Tony (2026-08-13, PIN_MAPPING_BOM.md §1.2):
// dual-wavelength Red + Infrared, I2C 0x57, PART_ID 0x15.
//
// Driver register-level (bukan library SparkFun lagi): library lama hanya
// menampung 4 sample di buffer internalnya dan firmware lama mengambil 1 sample
// per iterasi, sehingga sebagian besar sample hilang dan laju aktual jauh di
// bawah sample_rate_hz yang diklaim di payload — estimasi HR backend jadi salah.
// Driver ini menguras SELURUH FIFO sensor (32 sample) tiap kali dipanggil.
//
// Firmware hanya membaca raw sample; filtering & estimasi HR/SpO2 di backend.

struct PpgSample {
  uint32_t red;
  uint32_t ir;
};

#define PPG_FIFO_DEPTH 32

// Probe (PART_ID) + reset + konfigurasi sesuai config.h. Mengembalikan false
// bila sensor tidak merespons/ID salah.
bool ppg_acquisition_init();

bool ppg_acquisition_is_ready();

// Non-blocking: menyalin semua sample baru di FIFO (maks max_samples) ke out.
// Bila sensor belum siap/hilang, otomatis mencoba init ulang tiap
// PPG_REINIT_INTERVAL_MS tanpa memblokir pemanggil lama-lama.
size_t ppg_acquisition_read(PpgSample *out, size_t max_samples);

// Diagnostik.
uint32_t ppg_acquisition_get_samples_read();
uint32_t ppg_acquisition_get_fifo_overflows(); // sample hilang di FIFO sensor
uint32_t ppg_acquisition_get_i2c_errors();

// Utilitas untuk program diagnostik (src/diag/diag_ppg.cpp).
bool ppg_read_register(uint8_t reg, uint8_t *value);
bool ppg_write_register(uint8_t reg, uint8_t value);
bool ppg_read_part_info(uint8_t *part_id, uint8_t *rev_id);
// Suhu die sensor (°C), NAN bila gagal. Blocking ~30 ms.
float ppg_read_temperature_c();
bool ppg_set_led_currents(uint8_t red, uint8_t ir);
