// Format pesan ke backend — INTEGRATION_CONTRACT.md §2.3 (audio websocket),
// §3.3 (PPG), §3.4 (status). Backend tidak memvalidasi isi PCM, jadi kesalahan
// format hanya terlihat sebagai audio rusak; test ini mendekode pesan dengan
// cara yang sama seperti backend (_decode_pcm_base64: base64 -> int16 LE).

#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include <config.h>

#include "audio_frame.h"
#include "ppg_payload.h"

void setUp() {}
void tearDown() {}

static int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decoder base64 referensi (independen dari encoder yang diuji).
static size_t base64_decode(const char *in, uint8_t *out) {
  size_t len = strlen(in), n = 0;
  for (size_t i = 0; i + 3 < len; i += 4) {
    int v[4];
    for (int k = 0; k < 4; k++) v[k] = (in[i + k] == '=') ? 0 : base64_value(in[i + k]);
    out[n++] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
    if (in[i + 2] != '=') out[n++] = (uint8_t)(((v[1] & 0x0F) << 4) | (v[2] >> 2));
    if (in[i + 3] != '=') out[n++] = (uint8_t)(((v[2] & 0x03) << 6) | v[3]);
  }
  return n;
}

// Sama dengan backend/ingestion/websocket_server.py::_decode_pcm_base64.
static size_t backend_decode_pcm(const char *b64, int16_t *out) {
  static uint8_t raw[CHUNK_SAMPLE_COUNT * 2 + 4];
  size_t n_bytes = base64_decode(b64, raw);
  for (size_t i = 0; i < n_bytes / 2; i++) {
    int value = raw[2 * i] | (raw[2 * i + 1] << 8);
    if (value >= 32768) value -= 65536;
    out[i] = (int16_t)value;
  }
  return n_bytes / 2;
}

static AudioFrameHeader make_header() {
  AudioFrameHeader h = {"pneumacare-a1b2", 3, 16000, 1723276800123ULL, 4821, CHUNK_DURATION_MS};
  return h;
}

void test_audio_frame_has_all_required_fields() {
  int16_t pcm[4] = {0, 1, -1, 32767};
  static char out[1024];
  size_t len = audio_frame_build(out, sizeof(out), make_header(), pcm, 4);
  TEST_ASSERT_GREATER_THAN(0, len);
  TEST_ASSERT_EQUAL_UINT32(strlen(out), len);

  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, out).code());
  // Field wajib backend (pesan di-skip bila salah satu None).
  TEST_ASSERT_EQUAL_STRING("pneumacare-a1b2", doc["device_id"].as<const char *>());
  TEST_ASSERT_EQUAL_INT(3, doc["channel_id"].as<int>());
  TEST_ASSERT_EQUAL_INT(4821, doc["seq_no"].as<int>());
  TEST_ASSERT_TRUE(doc["timestamp_ms"].as<uint64_t>() == 1723276800123ULL);
  TEST_ASSERT_EQUAL_INT(CHUNK_DURATION_MS, doc["chunk_duration_ms"].as<int>());
  TEST_ASSERT_EQUAL_INT(16000, doc["sample_rate"].as<int>());
  TEST_ASSERT_TRUE(doc["pcm_base64"].is<const char *>());
  // bit_depth harus 16 — backend men-skip pesan dengan bit_depth lain.
  TEST_ASSERT_EQUAL_INT(16, doc["bit_depth"].as<int>());
}

void test_audio_frame_pcm_roundtrip_matches_backend_decoder() {
  static int16_t pcm[CHUNK_SAMPLE_COUNT];
  for (size_t i = 0; i < CHUNK_SAMPLE_COUNT; i++) {
    pcm[i] = (int16_t)((i * 7919) % 65536 - 32768); // mencakup nilai negatif & ekstrem
  }
  pcm[0] = -32768;
  pcm[1] = 32767;
  pcm[2] = -1;

  size_t cap = audio_frame_max_size(CHUNK_SAMPLE_COUNT);
  char *out = new char[cap];
  size_t len = audio_frame_build(out, cap, make_header(), pcm, CHUNK_SAMPLE_COUNT);
  TEST_ASSERT_GREATER_THAN(0, len);

  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, out).code());
  static int16_t decoded[CHUNK_SAMPLE_COUNT];
  size_t n = backend_decode_pcm(doc["pcm_base64"].as<const char *>(), decoded);

  TEST_ASSERT_EQUAL_UINT32(CHUNK_SAMPLE_COUNT, n);
  TEST_ASSERT_EQUAL_INT16_ARRAY(pcm, decoded, CHUNK_SAMPLE_COUNT);
  delete[] out;
}

void test_audio_frame_base64_padding_for_odd_byte_counts() {
  // 1 sample = 2 byte -> 1 karakter '=' padding; 2 sample = 4 byte -> 2 '='.
  int16_t one[1] = {0x1234};
  int16_t two[2] = {0x1234, -2};
  static char out[512];
  int16_t decoded[2];

  audio_frame_build(out, sizeof(out), make_header(), one, 1);
  JsonDocument doc;
  deserializeJson(doc, out);
  TEST_ASSERT_EQUAL_STRING("NBI=", doc["pcm_base64"].as<const char *>());
  TEST_ASSERT_EQUAL_UINT32(1, backend_decode_pcm(doc["pcm_base64"].as<const char *>(), decoded));
  TEST_ASSERT_EQUAL_INT16(0x1234, decoded[0]);

  audio_frame_build(out, sizeof(out), make_header(), two, 2);
  deserializeJson(doc, out);
  TEST_ASSERT_EQUAL_UINT32(2, backend_decode_pcm(doc["pcm_base64"].as<const char *>(), decoded));
  TEST_ASSERT_EQUAL_INT16_ARRAY(two, decoded, 2);
}

void test_audio_frame_rejects_small_buffer() {
  int16_t pcm[100] = {0};
  char out[64];
  TEST_ASSERT_EQUAL_UINT32(0, audio_frame_build(out, sizeof(out), make_header(), pcm, 100));
  TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}

void test_audio_frame_max_size_is_sufficient_for_full_chunk() {
  static int16_t pcm[CHUNK_SAMPLE_COUNT];
  size_t cap = audio_frame_max_size(CHUNK_SAMPLE_COUNT);
  char *out = new char[cap];
  AudioFrameHeader h = make_header();
  h.timestamp_ms = UINT64_MAX; // field angka terpanjang
  h.seq_no = UINT32_MAX;
  TEST_ASSERT_GREATER_THAN(0, audio_frame_build(out, cap, h, pcm, CHUNK_SAMPLE_COUNT));
  delete[] out;
}

void test_ppg_payload_schema() {
  uint32_t ir[3] = {512, 515, 262143};
  uint32_t red[3] = {498, 501, 0};
  char out[512];
  size_t len = ppg_payload_build(out, sizeof(out), "pneumacare-a1b2", 1723276800123ULL, 100, ir,
                                 red, 3);
  TEST_ASSERT_GREATER_THAN(0, len);

  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, out).code());
  TEST_ASSERT_EQUAL_STRING("pneumacare-a1b2", doc["device_id"].as<const char *>());
  TEST_ASSERT_EQUAL_INT(100, doc["sample_rate_hz"].as<int>());
  TEST_ASSERT_TRUE(doc["timestamp_ms"].as<uint64_t>() == 1723276800123ULL);
  // §3.3: samples = IR, samples_red = red, panjang & urutan sama.
  JsonArray s = doc["samples"];
  JsonArray r = doc["samples_red"];
  TEST_ASSERT_EQUAL_UINT32(3, s.size());
  TEST_ASSERT_EQUAL_UINT32(3, r.size());
  TEST_ASSERT_EQUAL_UINT32(262143, s[2].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(498, r[0].as<uint32_t>());
}

void test_full_ppg_batch_fits_mqtt_buffer() {
  // Kasus terburuk: semua sample bernilai maksimum 18-bit (6 digit).
  uint32_t ir[PPG_BATCH_SIZE], red[PPG_BATCH_SIZE];
  for (int i = 0; i < PPG_BATCH_SIZE; i++) ir[i] = red[i] = 262143;
  static char out[MQTT_BUFFER_SIZE];
  size_t len = ppg_payload_build(out, sizeof(out), "pneumacare-a1b2", UINT64_MAX / 2,
                                 PPG_SAMPLE_RATE_HZ, ir, red, PPG_BATCH_SIZE);
  TEST_ASSERT_GREATER_THAN(0, len);
  // PubSubClient butuh ruang header MQTT (topik + ~5 byte) di buffer yang sama.
  TEST_ASSERT_LESS_THAN(MQTT_BUFFER_SIZE - 64, len);
}

void test_ppg_payload_rejects_small_buffer() {
  uint32_t ir[50] = {0}, red[50] = {0};
  char out[32];
  TEST_ASSERT_EQUAL_UINT32(0, ppg_payload_build(out, sizeof(out), "x", 0, 100, ir, red, 50));
}

void test_status_payload_online_and_will() {
  char out[256];
  JsonDocument doc;

  status_payload_build(out, sizeof(out), "pneumacare-a1b2", "online", 42, true, -1);
  deserializeJson(doc, out);
  TEST_ASSERT_EQUAL_STRING("online", doc["status"].as<const char *>());
  TEST_ASSERT_EQUAL_INT(-1, doc["battery_pct"].as<int>()); // sentinel "tidak tersedia"
  TEST_ASSERT_EQUAL_INT(42, doc["timestamp_ms"].as<int>());

  status_payload_build(out, sizeof(out), "pneumacare-a1b2", "offline", 0, false, 0);
  deserializeJson(doc, out);
  TEST_ASSERT_EQUAL_STRING("offline", doc["status"].as<const char *>());
  TEST_ASSERT_FALSE(doc["battery_pct"].is<int>()); // Last Will tanpa baterai
}

void test_audio_continuity_metadata_roundtrip_and_size() {
  static int16_t pcm[CHUNK_SAMPLE_COUNT];
  AudioFrameHeader h = make_header();
  h.stream_id = "0123456789abcdef";
  h.sample_start = UINT64_MAX;
  h.timestamp_ms = UINT64_MAX;
  h.seq_no = UINT32_MAX;
  size_t cap = audio_frame_max_size(CHUNK_SAMPLE_COUNT);
  char *out = new char[cap];
  TEST_ASSERT_GREATER_THAN(0, audio_frame_build(out, cap, h, pcm, CHUNK_SAMPLE_COUNT));
  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, out).code());
  TEST_ASSERT_EQUAL_STRING(h.stream_id, doc["stream_id"].as<const char *>());
  TEST_ASSERT_TRUE(doc["sample_start"].as<uint64_t>() == UINT64_MAX);
  delete[] out;
}

static void test_binary_pcm_header_and_signed_roundtrip() {
  AudioFrameHeader h = {"test-device", 2, 16000, 1700000000000ULL, 7, 250,
                        "0123456789abcdef", 28000};
  int16_t pcm[] = {-32768, 32767, 0, -1};
  uint8_t out[1024];
  size_t len = audio_frame_build_binary(out, sizeof(out), h, pcm, 4);
  TEST_ASSERT_GREATER_THAN(6, len);
  TEST_ASSERT_EQUAL_MEMORY("PCA1", out, 4);
  size_t header_len = out[4] | ((size_t)out[5] << 8);
  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok,
                    deserializeJson(doc, out + 6, header_len).code());
  TEST_ASSERT_EQUAL_STRING("test-device", doc["device_id"].as<const char *>());
  TEST_ASSERT_EQUAL(28000, doc["sample_start"].as<uint64_t>());
  TEST_ASSERT_TRUE(doc["pcm_base64"].isNull());
  uint8_t expected[] = {0,128,255,127,0,0,255,255};
  TEST_ASSERT_EQUAL(6 + header_len + sizeof(expected), len);
  TEST_ASSERT_EQUAL_MEMORY(expected, out + 6 + header_len, sizeof(expected));
  TEST_ASSERT_EQUAL(0, audio_frame_build_binary(out, 6, h, pcm, 4));
}


int main() {
  UNITY_BEGIN();
  RUN_TEST(test_binary_pcm_header_and_signed_roundtrip);
  RUN_TEST(test_audio_continuity_metadata_roundtrip_and_size);
  RUN_TEST(test_audio_frame_has_all_required_fields);
  RUN_TEST(test_audio_frame_pcm_roundtrip_matches_backend_decoder);
  RUN_TEST(test_audio_frame_base64_padding_for_odd_byte_counts);
  RUN_TEST(test_audio_frame_rejects_small_buffer);
  RUN_TEST(test_audio_frame_max_size_is_sufficient_for_full_chunk);
  RUN_TEST(test_ppg_payload_schema);
  RUN_TEST(test_full_ppg_batch_fits_mqtt_buffer);
  RUN_TEST(test_ppg_payload_rejects_small_buffer);
  RUN_TEST(test_status_payload_online_and_will);
  return UNITY_END();
}
