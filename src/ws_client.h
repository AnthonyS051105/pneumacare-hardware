#pragma once

#include <stdbool.h>

// Websocket client audio — FR-HW-021, INTEGRATION_CONTRACT.md §2.
//
// Format pesan: JSON + field "pcm_base64" (✅ FINAL, dikunci 12 Agt 2026 di
// kontrak — BUKAN binary frame, opsi itu tidak diimplementasikan backend).
// PCM WAJIB dikonversi ke int16 little-endian di firmware sebelum di-encode
// base64, terlepas dari format akuisisi mentah (INMP441 dibaca ESP32 dalam
// slot 32-bit) — backend TIDAK memvalidasi bit_depth, hardcode asumsi int16,
// jadi kesalahan konversi ini tidak akan terlihat sebagai error, hanya audio
// rusak. Lihat INTEGRATION_CONTRACT.md §2.3 untuk detail lengkap.
//
// Channel-agnostic: ws_client_send_pending_chunks() mengirim SEMUA channel
// yang punya data siap di buffer_manager, di-loop berdasarkan
// NUM_AUDIO_CHANNELS (config.h) — bukan hardcode channel_id=1 tetap. Saat
// NUM_AUDIO_CHANNELS=1 (checkpoint 50%) hanya channel 0 yang punya data;
// menaikkan ke 4 tidak butuh perubahan logic di modul ini.

void ws_client_init();

bool ws_client_is_connected();

// Non-blocking: dipanggil berkala dari loop utama. Menangani event
// websocket library (wajib, meski tidak ada yang dikirim) DAN, bila
// terhubung, memeriksa & mengirim chunk yang siap dari SEMUA channel aktif.
void ws_client_tick();

// Dipanggil berkala dari loop utama untuk reconnect otomatis dengan
// exponential backoff (FR-HW-024, INTEGRATION_CONTRACT.md §2.4) saat
// websocket terputus.
void ws_client_reconnect_tick();
