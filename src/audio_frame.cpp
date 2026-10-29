#include "audio_frame.h"

#include <string.h>

// Ruang untuk semua field JSON selain isi base64 (nama field, device_id, angka).
static const size_t kJsonOverheadBytes = 512;

static const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t audio_frame_base64_len(size_t n_bytes) {
  return ((n_bytes + 2) / 3) * 4;
}

size_t audio_frame_max_size(size_t n_samples) {
  return kJsonOverheadBytes + audio_frame_base64_len(n_samples * 2) + 1;
}

// Penulis string berbatas: semua append berhenti diam-diam bila buffer penuh,
// lalu `overflow` dicek sekali di akhir.
struct Writer {
  char *buf;
  size_t cap;
  size_t len;
  bool overflow;
};

static void put_char(Writer &w, char c) {
  if (w.len + 1 >= w.cap) {
    w.overflow = true;
    return;
  }
  w.buf[w.len++] = c;
}

static void put_str(Writer &w, const char *s) {
  while (*s) put_char(w, *s++);
}

static void put_u64(Writer &w, uint64_t v) {
  char digits[21];
  size_t n = 0;
  do {
    digits[n++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  while (n > 0) put_char(w, digits[--n]);
}

// Base64 atas byte PCM int16 little-endian, langsung dari array sample.
static void put_pcm_base64(Writer &w, const int16_t *pcm, size_t n_samples) {
  size_t n_bytes = n_samples * 2;
  if (w.len + audio_frame_base64_len(n_bytes) + 1 >= w.cap) {
    w.overflow = true;
    return;
  }

  uint8_t triple[3];
  size_t filled = 0;
  for (size_t i = 0; i < n_bytes; i++) {
    uint16_t sample = (uint16_t)pcm[i / 2];
    triple[filled++] = (i % 2 == 0) ? (uint8_t)(sample & 0xFF) : (uint8_t)(sample >> 8);
    if (filled == 3) {
      w.buf[w.len++] = kBase64Alphabet[triple[0] >> 2];
      w.buf[w.len++] = kBase64Alphabet[((triple[0] & 0x03) << 4) | (triple[1] >> 4)];
      w.buf[w.len++] = kBase64Alphabet[((triple[1] & 0x0F) << 2) | (triple[2] >> 6)];
      w.buf[w.len++] = kBase64Alphabet[triple[2] & 0x3F];
      filled = 0;
    }
  }

  if (filled > 0) {
    if (filled == 1) triple[1] = 0;
    w.buf[w.len++] = kBase64Alphabet[triple[0] >> 2];
    w.buf[w.len++] = kBase64Alphabet[((triple[0] & 0x03) << 4) | (triple[1] >> 4)];
    w.buf[w.len++] = (filled == 2) ? kBase64Alphabet[(triple[1] & 0x0F) << 2] : '=';
    w.buf[w.len++] = '=';
  }
}

size_t audio_frame_build(char *out, size_t out_cap, const AudioFrameHeader &header,
                         const int16_t *pcm, size_t n_samples) {
  if (out == NULL || out_cap == 0) return 0;

  Writer w = {out, out_cap, 0, false};

  // device_id dari config (pneumacare-xxxx) tidak mengandung karakter yang
  // perlu di-escape JSON.
  put_str(w, "{\"device_id\":\"");
  put_str(w, header.device_id);
  put_str(w, "\",\"channel_id\":");
  put_u64(w, header.channel_id);
  put_str(w, ",\"sample_rate\":");
  put_u64(w, header.sample_rate);
  // PCM selalu int16 LE; backend menolak bit_depth selain 16.
  put_str(w, ",\"bit_depth\":16,\"timestamp_ms\":");
  put_u64(w, header.timestamp_ms);
  put_str(w, ",\"seq_no\":");
  put_u64(w, header.seq_no);
  put_str(w, ",\"chunk_duration_ms\":");
  put_u64(w, header.chunk_duration_ms);
  if (header.stream_id != NULL) {
    put_str(w, ",\"stream_id\":\"");
    put_str(w, header.stream_id); // generated hex16, no JSON escaping required
    put_str(w, "\",\"sample_start\":");
    put_u64(w, header.sample_start);
  }
  if (header.capture_id) {
    put_str(w, ",\"capture_id\":");put_u64(w, header.capture_id);
    put_str(w, ",\"capture_duration_ms\":");put_u64(w, header.capture_duration_ms);
    put_str(w, ",\"capture_offset_samples\":");put_u64(w, header.capture_offset_samples);
    put_str(w, ",\"capture_start_ms\":");put_u64(w, header.capture_start_ms);
  }
  if (header.simulated_source_channel_id) {
    put_str(w, ",\"data_origin\":\"simulated\",\"source_channel_id\":");
    put_u64(w, header.simulated_source_channel_id);
  }
  put_str(w, ",\"pcm_base64\":\"");
  put_pcm_base64(w, pcm, n_samples);
  put_str(w, "\"}");

  if (w.overflow) {
    out[0] = '\0';
    return 0;
  }
  out[w.len] = '\0';
  return w.len;
}

size_t audio_frame_binary_max_size(size_t n_samples) {
  return 6 + audio_frame_max_size(0) + n_samples * 2;
}

size_t audio_frame_build_binary(uint8_t *out, size_t out_cap, const AudioFrameHeader &header,
                               const int16_t *pcm, size_t n_samples) {
  if (out == NULL || (pcm == NULL && n_samples) || out_cap < 6 ||
      n_samples > (out_cap - 6) / 2) return 0;
  // Reuse the canonical JSON metadata writer, without encoding any samples.
  size_t json_len = audio_frame_build((char *)out + 6, out_cap - 6, header, NULL, 0);
  const char suffix[] = ",\"pcm_base64\":\"\"}";
  size_t suffix_len = sizeof(suffix) - 1;
  if (json_len < suffix_len ||
      memcmp(out + 6 + json_len - suffix_len, suffix, suffix_len) != 0) return 0;
  size_t header_len = json_len - suffix_len + 1;
  if (header_len > 1024 || 6 + header_len + n_samples * 2 > out_cap) return 0;
  out[6 + header_len - 1] = '}';
  memcpy(out, "PCA1", 4);
  out[4] = (uint8_t)header_len;
  out[5] = (uint8_t)(header_len >> 8);
  for (size_t i = 0; i < n_samples; i++) {
    uint16_t v = (uint16_t)pcm[i];
    out[6 + header_len + i * 2] = (uint8_t)v;
    out[6 + header_len + i * 2 + 1] = (uint8_t)(v >> 8);
  }
  return 6 + header_len + n_samples * 2;
}
