#include "audio_frame.h"

#include <string.h>

// Ruang untuk semua field JSON selain isi base64 (nama field, device_id, angka).
static const size_t kJsonOverheadBytes = 384;

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
