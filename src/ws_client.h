#pragma once

#include <stdbool.h>
#include <stdint.h>

// Websocket client audio — FR-HW-021, INTEGRATION_CONTRACT.md §2.
//
// Format pesan: JSON + "pcm_base64" (✅ FINAL §2.3, bukan binary frame), disusun
// oleh audio_frame.cpp. PCM selalu int16 little-endian (backend memvalidasi
// bit_depth). Mengirim chunk dari SEMUA channel yang punya data
// di buffer_manager (loop NUM_AUDIO_CHANNELS); channel yang mic-nya terdeteksi
// mati tidak punya data sehingga otomatis tidak terkirim.
//
// timestamp_ms tiap chunk = perkiraan waktu tangkap sample PERTAMA chunk itu
// (bukan waktu kirim), dihitung dari jumlah sample yang masih antre di buffer.

struct WsClientStats {
  uint32_t chunks_sent;
  uint32_t send_failures;
  uint32_t connects;
  uint32_t max_build_us;
  uint32_t max_send_us;
};

// device_id disuntikkan supaya program test fase 3 bisa memakai DIAG_DEVICE_ID
// tanpa menyentuh data pasien. Pointer harus valid selamanya (literal/static).
// Mengembalikan false bila alokasi buffer pesan gagal.
bool ws_client_init(const char *device_id);

bool ws_client_is_connected();

// Non-blocking (kecuali saat connect/kirim TCP): menjalankan event websocket,
// reconnect dengan exponential backoff (FR-HW-024, §2.4), dan bila terhubung &
// streaming_enabled, mengirim chunk yang siap. Saat tidak bisa mengirim, isi
// ring buffer dibuang (data lama tidak relevan & drop_count tetap bermakna
// sebagai indikator "network lebih lambat dari akuisisi").
void ws_client_tick(bool streaming_enabled);

void ws_client_get_stats(WsClientStats *out);
