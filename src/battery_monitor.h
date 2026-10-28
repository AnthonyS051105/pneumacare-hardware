#pragma once

#include <stdint.h>

// Level baterai untuk payload status — FR-HW-030 (opsional), PIN_MAPPING_BOM.md §1.3.
// Membaca tegangan baterai lewat pembagi tegangan ke ADC1 (PIN_BATTERY_SENSE).
// Bila BATTERY_SENSE_ENABLED = 0, selalu mengembalikan -1 ("tidak tersedia",
// dinormalisasi backend menjadi None).

void battery_monitor_init();

// Tegangan baterai (mV) hasil rata-rata BATTERY_ADC_SAMPLES pembacaan. 0 bila nonaktif.
uint32_t battery_monitor_read_mv();

// Persen 0-100 (dihaluskan), atau -1 bila nonaktif.
int battery_monitor_read_percent();
