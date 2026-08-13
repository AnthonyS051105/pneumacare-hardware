#pragma once

#include <stddef.h>
#include <stdint.h>

// Ring buffer per-channel & chunking — FR-HW-011, lihat SDD_HARDWARE.md §4.
//
// Channel-agnostic by design: semua fungsi menerima channel_index (0-based,
// harus < NUM_AUDIO_CHANNELS dari config.h). Saat ini hanya index 0 dipakai
// (1 mikrofon fisik tersedia untuk checkpoint 50%), tapi implementasi internal
// sudah di-loop untuk NUM_AUDIO_CHANNELS channel sejak awal — menaikkan
// NUM_AUDIO_CHANNELS ke 4 tidak butuh perubahan di modul ini.
//
// Kebijakan saat buffer penuh: drop data TERLAMA (bukan menahan penuh sampai
// crash), sesuai SDD §4. Setiap event drop dicatat lewat drop_count per channel.

void buffer_manager_init();

// Menulis satu sample ke ring buffer channel tsb. Non-blocking, aman dipanggil
// dari task acquisition (Core 0). Bila buffer penuh, sample TERLAMA di-drop
// otomatis untuk memberi ruang bagi sample baru (drop_count bertambah).
void buffer_manager_write(uint8_t channel_index, int32_t sample);

// Jumlah sample yang saat ini tersedia untuk dibaca di channel tsb.
size_t buffer_manager_available(uint8_t channel_index);

// Menyalin hingga max_samples sample tertua dari ring buffer ke out_buf, lalu
// menghapusnya dari buffer (consume). Mengembalikan jumlah sample yang benar-benar
// disalin. Dipanggil dari task network (Core 1) saat siap kirim chunk.
size_t buffer_manager_read_chunk(uint8_t channel_index, int32_t *out_buf,
                                  size_t max_samples);

// Total event drop-oldest sejak boot untuk channel tsb — indikasi network
// lebih lambat dari akuisisi, dipakai untuk diagnostik/status payload (SDD §4).
uint32_t buffer_manager_get_drop_count(uint8_t channel_index);
