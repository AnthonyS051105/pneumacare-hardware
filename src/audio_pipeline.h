#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Rangkaian pemrosesan audio di task acquisition (Core 0) — ARCHITECTURE_HARDWARE.md §1:
//   I2S 4-channel -> cek kesehatan channel -> noise filter -> PCM int16 -> ring buffer
//
// Dipisah dari main.cpp supaya program test fase 2 (src/diag/phase2_filter_buffer.cpp)
// menjalankan pipeline yang PERSIS sama dengan firmware produksi.

struct AudioChannelStats {
  bool faulty;           // mic mati/lepas/macet (lihat channel_health.h)
  bool evaluated;        // minimal 1 jendela kesehatan sudah dievaluasi
  int32_t last_p2p;      // puncak-ke-puncak 24-bit jendela terakhir
  uint32_t clip_count;   // total sample int16 yang tersaturasi sejak boot
  uint32_t sample_count; // total sample diproses sejak boot
  int16_t last_peak;     // |puncak| PCM int16 blok terakhir
};

// Inisialisasi (atau init ulang) I2S, filter, dan health monitor. Mengembalikan
// false bila I2S gagal. Ring buffer TIDAK dialokasikan di sini — panggil
// buffer_manager_init() sekali sebelum task dibuat, supaya init ulang setelah
// kegagalan I2S tidak membebaskan buffer yang sedang dibaca task network.
bool audio_pipeline_init();

// Satu iterasi blocking (dipacu DMA I2S, ~16 ms). Mengembalikan jumlah frame
// yang diproses per channel (0 bila pembacaan I2S gagal).
size_t audio_pipeline_process_block();

void audio_pipeline_get_stats(uint8_t channel_index, AudioChannelStats *out);
