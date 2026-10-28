#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Deteksi mikrofon mati/lepas/macet per channel. Mic INMP441 asli selalu punya
// noise floor (ratusan LSB 24-bit); mic yang lepas (SD ditarik pull-down) terbaca
// 0 terus, dan mic rusak/short terbaca konstan. Keduanya punya puncak-ke-puncak
// mendekati nol dalam satu jendela waktu.
//
// Penting untuk perangkat kesehatan: tanpa deteksi ini, keheningan dari mic
// yang lepas akan diklasifikasikan backend sebagai "none" (normal) — rasa aman
// palsu. Status dievaluasi per jendela; pulih otomatis saat sinyal kembali.

struct ChannelHealth {
  int32_t window_min;
  int32_t window_max;
  uint32_t window_samples;
  uint32_t window_length;
  int32_t p2p_threshold;
  int32_t last_p2p;
  bool faulty;
  bool evaluated; // minimal satu jendela penuh sudah dievaluasi
  uint32_t fault_transitions;
};

void channel_health_init(ChannelHealth *h, uint32_t window_length_samples, int32_t p2p_threshold);

// Memasukkan blok sample mentah 24-bit. Mengembalikan true bila status faulty
// berubah pada pemanggilan ini (untuk logging).
bool channel_health_update(ChannelHealth *h, const int32_t *samples, size_t n);
