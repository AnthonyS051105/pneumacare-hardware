#pragma once

#include <stddef.h>
#include <stdint.h>

// Ring buffer per-channel & chunking — FR-HW-011, lihat SDD_HARDWARE.md §4.
//
// Menyimpan sample PCM int16 yang SUDAH difilter & dikonversi (bukan int32
// mentah) — separuh memori dibanding versi lama, sehingga 4 channel x 500 ms
// muat nyaman di heap ESP32. Buffer dialokasikan di heap saat init (bukan array
// statis) supaya tidak membebani segmen DRAM statis yang terbatas.
//
// Producer (task acquisition, Core 0) menulis per blok; consumer (task network,
// Core 1) membaca per chunk. Kebijakan saat penuh: drop sample TERLAMA (SDD §4),
// jumlah sample yang di-drop dicatat per channel untuk diagnostik.

// Mengalokasikan num_channels ring buffer berkapasitas capacity sample.
// Mengembalikan false bila alokasi gagal. Aman dipanggil ulang (realokasi).
bool buffer_manager_init(uint8_t num_channels, size_t capacity);

// Membebaskan seluruh buffer (dipakai unit test / re-init).
void buffer_manager_deinit();

size_t buffer_manager_capacity();

// Menulis n sample ke channel tsb. Non-blocking. Bila tidak muat, sample
// terlama di-drop; bila n > kapasitas, hanya `kapasitas` sample terbaru disimpan.
void buffer_manager_write_block(uint8_t channel_index, const int16_t *samples, size_t n);

size_t buffer_manager_available(uint8_t channel_index);

// Menyalin & meng-consume hingga max_samples sample tertua ke out_buf.
// Mengembalikan jumlah sample yang benar-benar disalin.
// sample_start (opsional) diambil DALAM lock yang sama dengan PCM: indeks
// sample pertama sejak boot, termasuk sample terbuang/disupresi. Jangan baca
// drop_count terpisah sebagai metadata chunk (race dengan producer).
size_t buffer_manager_read_chunk(uint8_t channel_index, int16_t *out_buf, size_t max_samples,
                                 uint64_t *sample_start = NULL);

// Tandai frame akuisisi yang sengaja disupresi (mic faulty); batalkan isi lama
// supaya audio sebelum/sesudah celah tidak disambung sebagai kontinu.
void buffer_manager_skip_samples(uint8_t channel_index, size_t n);

// Mengosongkan isi channel tsb (mis. saat koneksi belum siap streaming).
void buffer_manager_clear(uint8_t channel_index);

// Total sample yang di-drop sejak boot (indikasi network lebih lambat dari akuisisi).
uint32_t buffer_manager_get_drop_count(uint8_t channel_index);
