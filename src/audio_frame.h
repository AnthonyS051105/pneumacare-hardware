#pragma once

#include <stddef.h>
#include <stdint.h>

// Penyusun pesan websocket audio — INTEGRATION_CONTRACT.md §2.3 (✅ FINAL:
// JSON teks + pcm_base64 legacy, atau PCA1 binary PCM yang backward-compatible).
//
// Pesan ditulis langsung ke satu buffer milik pemanggil (tanpa ArduinoJson /
// String perantara) — versi lama menyalin ~11 KB data hingga 4x per chunk
// (String base64 -> JsonDocument -> String hasil serialize), yang tidak muat
// bila dikalikan 4 channel. Byte PCM disusun eksplisit little-endian (tidak
// bergantung endianness CPU) supaya cocok dengan _decode_pcm_base64 backend.

struct AudioFrameHeader {
  const char *device_id;
  uint8_t channel_id;
  uint32_t sample_rate;
  uint64_t timestamp_ms;
  uint32_t seq_no;
  uint32_t chunk_duration_ms;
  const char *stream_id; // boot identifier; NULL for legacy diagnostic senders
  uint64_t sample_start;
  uint32_t capture_id;
  uint32_t capture_duration_ms;
  uint32_t capture_offset_samples;
  uint64_t capture_start_ms;
};

// Ukuran base64 (tanpa terminator) untuk n byte input.
size_t audio_frame_base64_len(size_t n_bytes);

// Kapasitas buffer minimal (termasuk terminator '\0') untuk n sample PCM.
size_t audio_frame_max_size(size_t n_samples);

// Menulis JSON lengkap ke out (diakhiri '\0'). Mengembalikan panjang pesan
// (tanpa terminator), atau 0 bila out_cap tidak cukup.
size_t audio_frame_build(char *out, size_t out_cap, const AudioFrameHeader &header,
                         const int16_t *pcm, size_t n_samples);

// PCA1 + uint16 LE JSON header length + JSON metadata (no pcm_base64) + PCM LE.
size_t audio_frame_binary_max_size(size_t n_samples);
size_t audio_frame_build_binary(uint8_t *out, size_t out_cap, const AudioFrameHeader &header,
                               const int16_t *pcm, size_t n_samples);
