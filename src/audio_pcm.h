#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Konversi sample audio domain 24-bit (hasil filter) -> PCM int16 untuk
// transmisi. INTEGRATION_CONTRACT.md §2.3: backend hardcode asumsi int16 LE
// dan TIDAK memvalidasi isi PCM — konversi yang salah tidak akan terlihat
// sebagai error, hanya audio rusak. Karena itu konversi dipusatkan di sini dan
// diuji unit test (test/test_audio_pcm).

// Membagi dengan 2^gain_shift, membulatkan, lalu saturasi ke rentang int16
// (tidak pernah wrap-around). *clipped di-set true bila saturasi terjadi.
int16_t audio_pcm_from_sample24(float sample24, uint8_t gain_shift, bool *clipped);

// Versi blok. Mengembalikan jumlah sample yang ter-clip.
size_t audio_pcm_convert_block(const float *in, int16_t *out, size_t n, uint8_t gain_shift);
