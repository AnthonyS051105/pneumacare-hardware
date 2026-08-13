#pragma once

#include <stdint.h>

// Filtering ringan — FR-HW-010. Baseline: DC-offset removal (single-pole
// high-pass) sesuai catatan SRS_HARDWARE.md FR-HW-010 ("mulai dari high-pass
// filter sederhana... didiskusikan lebih lanjut bila perlu lebih canggih").
//
// Channel-agnostic: state filter disimpan per channel_index (0-based, harus
// < NUM_AUDIO_CHANNELS). Sama seperti buffer_manager, hanya index 0 dipakai
// saat ini tapi implementasi sudah di-loop untuk NUM_AUDIO_CHANNELS sejak awal.

void noise_filter_init();

// Menerapkan filter DC-offset ke satu sample mentah dari channel tsb dan
// mengembalikan hasilnya. State filter (running DC estimate) per channel
// otomatis ter-update di setiap panggilan — panggil berurutan per sample,
// jangan diacak antar channel dalam satu aliran filter.
int32_t noise_filter_apply(uint8_t channel_index, int32_t raw_sample);
