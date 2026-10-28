#include "ws_client.h"

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <esp_system.h>
#include <config.h>
#include <secrets.h>

#include "audio_frame.h"
#include "buffer_manager.h"
#include "network_manager.h"
#include "reconnect_manager.h"

// WebSocketsClient punya reconnect internal dengan interval tetap (default
// 500 ms) — versi lama memanggil begin() manual dengan backoff, tapi library
// tetap mencoba tiap 500 ms sehingga backoff tidak efektif. Sekarang interval
// library sendiri yang diatur mengikuti ReconnectBackoff: library memancarkan
// WStype_DISCONNECTED untuk SETIAP percobaan gagal (TCP gagal, handshake
// ditolak, koneksi putus) dan mencatat waktunya, jadi cukup diatur di handler.
static WebSocketsClient g_ws;
static ReconnectBackoff g_ws_backoff;
static bool g_ws_connected = false;
static const char *g_device_id = DEVICE_ID;
static char g_auth_header[160];

static uint32_t g_seq_no[NUM_AUDIO_CHANNELS];
static char g_stream_id[17]; // random boot identifier, unchanged by reconnect
static int16_t g_pcm_chunk[CHUNK_SAMPLE_COUNT];
// [WEBSOCKETS_MAX_HEADER_SIZE byte cadangan untuk header frame][JSON]. Library
// menulis header di ruang cadangan -> kirim tanpa salinan tambahan 11 KB.
static uint8_t *g_frame_buf = NULL;
static size_t g_frame_buf_size = 0;

static WsClientStats g_stats = {0, 0, 0};

static void schedule_retry_after_failure(const char *reason) {
  g_ws.setReconnectInterval(g_ws_backoff.current_delay_ms);
  Serial.printf("[ws] %s — coba lagi dalam %lu ms.\n", reason,
                (unsigned long)g_ws_backoff.current_delay_ms);
  reconnect_backoff_notify_attempt(&g_ws_backoff, millis());
}

static void on_ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      // First data on a new transport must start with fresh ring content.
      // sample cursor survives clear, so losses remain visible on this boot.
      for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) buffer_manager_clear(ch);
      g_ws_connected = true;
      g_stats.connects++;
      reconnect_backoff_reset(&g_ws_backoff);
      g_ws.setReconnectInterval(g_ws_backoff.current_delay_ms);
      Serial.printf("[ws] Terhubung ke ws://%s:%d%s\n", BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH);
      break;
    case WStype_DISCONNECTED:
      if (g_ws_connected) {
        Serial.println("[ws] Terputus dari backend.");
      }
      g_ws_connected = false;
      // Mencakup TCP gagal, handshake ditolak (mis. token salah -> close 1008)
      // maupun koneksi putus; semuanya mengikuti backoff.
      schedule_retry_after_failure("Koneksi websocket gagal/berakhir");
      break;
    case WStype_ERROR:
      Serial.printf("[ws] Error: %.*s\n", (int)length, payload ? (const char *)payload : "");
      break;
    default:
      break;
  }
}

bool ws_client_init(const char *device_id) {
  g_device_id = device_id;
  snprintf(g_stream_id, sizeof(g_stream_id), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  reconnect_backoff_init(&g_ws_backoff, RECONNECT_BACKOFF_INITIAL_MS, RECONNECT_BACKOFF_MAX_MS);
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) g_seq_no[ch] = 0;

  g_frame_buf_size = WEBSOCKETS_MAX_HEADER_SIZE + audio_frame_max_size(CHUNK_SAMPLE_COUNT);
  g_frame_buf = (uint8_t *)malloc(g_frame_buf_size);
  if (g_frame_buf == NULL) {
    Serial.printf("[ws] Alokasi buffer pesan %u byte gagal.\n", (unsigned)g_frame_buf_size);
    return false;
  }

  // Autentikasi minimal — INTEGRATION_CONTRACT.md §6.
  snprintf(g_auth_header, sizeof(g_auth_header), WS_AUTH_HEADER_FMT, DEVICE_API_TOKEN);
  g_ws.setExtraHeaders(g_auth_header);
  g_ws.onEvent(on_ws_event);
  g_ws.setReconnectInterval(g_ws_backoff.current_delay_ms);
  g_ws.enableHeartbeat(WS_HEARTBEAT_PING_INTERVAL_MS, WS_HEARTBEAT_PONG_TIMEOUT_MS,
                       WS_HEARTBEAT_MAX_MISSED);
  g_ws.begin(BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH);
  return true;
}

bool ws_client_is_connected() {
  return g_ws_connected;
}

// Mengirim maksimal WS_MAX_CHUNKS_PER_CHANNEL_PER_TICK chunk untuk channel tsb.
// Hanya chunk PENUH yang dikirim — chunk_duration_ms harus jujur karena backend
// menyusun segmen 5 detik dari akumulasi field ini (§2.2/§4.1).
static bool send_ready_chunks(uint8_t channel_index) {
  for (uint8_t k = 0; k < WS_MAX_CHUNKS_PER_CHANNEL_PER_TICK; k++) {
    size_t available = buffer_manager_available(channel_index);
    if (available < CHUNK_SAMPLE_COUNT) return true;

    uint64_t backlog_ms = (uint64_t)available * 1000ULL / AUDIO_SAMPLE_RATE_HZ;
    uint64_t now_ms = network_manager_get_epoch_ms();
    uint64_t first_sample_ms = (now_ms > backlog_ms) ? now_ms - backlog_ms : 0;

    uint64_t sample_start = 0;
    size_t got = buffer_manager_read_chunk(channel_index, g_pcm_chunk, CHUNK_SAMPLE_COUNT,
                                           &sample_start);
    if (got != CHUNK_SAMPLE_COUNT) return true;

    AudioFrameHeader header = {
        g_device_id,
        AUDIO_CHANNEL_ID_MAP[channel_index],
        AUDIO_SAMPLE_RATE_HZ,
        first_sample_ms,
        g_seq_no[channel_index],
        CHUNK_DURATION_MS,
        g_stream_id,
        sample_start,
    };
    // seq_no naik walau kirim gagal: chunk itu memang hilang, dan gap seq_no
    // adalah cara backend mendeteksinya (§2.3).
    g_seq_no[channel_index]++;

    char *json = (char *)(g_frame_buf + WEBSOCKETS_MAX_HEADER_SIZE);
    uint32_t build_started_us = micros();
    size_t len = audio_frame_build(json, g_frame_buf_size - WEBSOCKETS_MAX_HEADER_SIZE, header,
                                   g_pcm_chunk, got);
    uint32_t build_us = micros() - build_started_us;
    if (build_us > g_stats.max_build_us) g_stats.max_build_us = build_us;
    if (len == 0) {
      g_stats.send_failures++;
      Serial.println("[ws] BUG: buffer pesan terlalu kecil untuk 1 chunk.");
      return true;
    }

    uint32_t send_started_us = micros();
    bool sent = g_ws.sendTXT(g_frame_buf, len, true);
    uint32_t send_us = micros() - send_started_us;
    if (send_us > g_stats.max_send_us) g_stats.max_send_us = send_us;
    if (!sent) {
      g_stats.send_failures++;
      return false; // koneksi kemungkinan putus — berhenti untuk tick ini
    }
    g_stats.chunks_sent++;
  }
  return true;
}

void ws_client_tick(bool streaming_enabled) {
  if (network_manager_is_connected()) {
    g_ws.loop();
  } else if (g_ws_connected) {
    // Tanpa WiFi, percobaan TCP connect hanya membuang waktu task network.
    g_ws.disconnect();
  }

  bool can_send = g_ws_connected && streaming_enabled;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    if (!can_send) {
      buffer_manager_clear(ch);
      continue;
    }
    if (!send_ready_chunks(ch)) break;
  }
}

void ws_client_get_stats(WsClientStats *out) {
  *out = g_stats;
}
