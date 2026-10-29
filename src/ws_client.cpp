#include "ws_client.h"

#include <Arduino.h>
#include <atomic>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <config.h>
#include <secrets.h>

#include "audio_frame.h"
#include "capture_scheduler.h"
#include "capture_delivery.h"
#include "ws_frame_writer.h"
#include "audio_transport_policy.h"
#include "platform_lock.h"
#include "buffer_manager.h"
#include "network_manager.h"
#include "reconnect_manager.h"

// WebSocketsClient punya reconnect internal dengan interval tetap (default
// 500 ms) — versi lama memanggil begin() manual dengan backoff, tapi library
// tetap mencoba tiap 500 ms sehingga backoff tidak efektif. Sekarang interval
// library sendiri yang diatur mengikuti ReconnectBackoff: library memancarkan
// WStype_DISCONNECTED untuk SETIAP percobaan gagal (TCP gagal, handshake
// ditolak, koneksi putus) dan mencatat waktunya, jadi cukup diatur di handler.
// Pinned WebSockets2.7.3 keeps the TCP client protected. Handshake/control frames
// remain with the library; only periodic application frames use nonblocking send.
class AudioWebSocketClient : public WebSocketsClient {
 public:
  int write_audio(const uint8_t *data,size_t size) {
    if(!_client.tcp || _client.isSSL)return -1; // this endpoint is ws://, not TLS
    int n=::send(_client.tcp->fd(),data,size,MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    return n>0?n:-1;
  }
  void abort_audio() {
    // A partially written frame cannot be followed by a WS close/ping frame.
    if(_client.tcp)_client.tcp->stop();
    disconnect();
  }
};
static AudioWebSocketClient g_ws;
static_assert(WEBSOCKETS_MAX_HEADER_SIZE == 14, "WsFrameWriter reserves 14 bytes");
static WsFrameWriter g_pending;
static uint32_t g_frame_started_us;
static CaptureSnapshot g_frame_capture;
static int write_audio(const uint8_t *data,size_t size,void *) {
  return g_ws.write_audio(data,size);
}
static ReconnectBackoff g_ws_backoff;
static std::atomic<bool> g_ws_connected{false};
static const char *g_device_id = DEVICE_ID;
static char g_auth_header[160];

static uint32_t g_seq_no[NUM_AUDIO_CHANNELS];
static char g_stream_id[17]; // random boot identifier, unchanged by reconnect
static int16_t g_pcm_chunk[CHUNK_SAMPLE_COUNT];
// [WEBSOCKETS_MAX_HEADER_SIZE byte cadangan untuk header frame][payload]. Library
// menulis header di ruang cadangan -> kirim tanpa salinan tambahan 11 KB.
static uint8_t *g_frame_buf = NULL;
static size_t g_frame_buf_size = 0;

static CaptureDelivery g_delivery;
static uint32_t g_ready_logged = 0;
static bool g_first_chunk_logged = false;
static uint8_t g_next_channel = 0;
static WsClientStats g_stats = {0, 0, 0};
static platform_lock_t g_stats_lock = PLATFORM_LOCK_INITIALIZER;
static void stats_increment(uint32_t WsClientStats::*field) {
  platform_lock_enter(&g_stats_lock);
  (g_stats.*field)++;
  platform_lock_exit(&g_stats_lock);
}
static void stats_max(uint32_t WsClientStats::*field, uint32_t value) {
  platform_lock_enter(&g_stats_lock);
  if (value > (g_stats.*field)) g_stats.*field = value;
  platform_lock_exit(&g_stats_lock);
}

static void cancel_audio_frame() {
  if(g_pending.pending())stats_max(&WsClientStats::max_send_us,micros()-g_frame_started_us);
  g_pending.reset();
}

static void schedule_retry_after_failure(const char *reason) {
  g_ws.setReconnectInterval(g_ws_backoff.current_delay_ms);
  Serial.printf("[ws] %s — coba lagi dalam %lu ms.\n", reason,
                (unsigned long)g_ws_backoff.current_delay_ms);
  reconnect_backoff_notify_attempt(&g_ws_backoff, millis());
}

static void on_ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      cancel_audio_frame();
      if (AUDIO_PERIODIC_CAPTURE) {
        auto capture=capture_snapshot();
        if(capture.state!=CAPTURE_IDLE)
          Serial.printf("[capture] aborted id=%lu reason=connection_change\n",(unsigned long)capture.id);
        capture_abort(millis());
      }
      // First data on a new transport must start with fresh ring content.
      // sample cursor survives clear, so losses remain visible on this boot.
      for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) buffer_manager_clear(ch);
      g_ws_connected = true;
      stats_increment(&WsClientStats::connects);
      reconnect_backoff_reset(&g_ws_backoff);
      g_ws.setReconnectInterval(g_ws_backoff.current_delay_ms);
      Serial.printf("[ws] Terhubung ke ws://%s:%d%s\n", BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH);
      break;
    case WStype_DISCONNECTED:
      cancel_audio_frame();
      if (AUDIO_PERIODIC_CAPTURE) {
        auto capture=capture_snapshot();
        if(capture.state!=CAPTURE_IDLE)
          Serial.printf("[capture] aborted id=%lu reason=connection_change\n",(unsigned long)capture.id);
        capture_abort(millis());
      }
      if (g_ws_connected) {
        Serial.println("[ws] Terputus dari backend.");
      }
      g_ws_connected = false;
      // Mencakup TCP gagal, handshake ditolak (mis. token salah -> close 1008)
      // maupun koneksi putus; semuanya mengikuti backoff.
      schedule_retry_after_failure("Koneksi websocket gagal/berakhir");
      break;
    case WStype_TEXT:
      if (AUDIO_PERIODIC_CAPTURE && length <= 256) {
        JsonDocument doc;
        if (deserializeJson(doc, payload, length) == DeserializationError::Ok &&
            doc["type"] == "audio_capture_ack" && doc["capture_id"].is<uint32_t>()) {
          uint32_t id = doc["capture_id"].as<uint32_t>();
          if (doc["status"] == "accepted" && capture_ack(id, millis())) {
            Serial.printf("[capture] accepted id=%lu channels=4 samples_per_channel=%u\n",
                          (unsigned long)id, (unsigned)AUDIO_CAPTURE_SAMPLE_COUNT);
          } else if (doc["status"] == "rejected" && id == capture_snapshot().id) {
            Serial.printf("[capture] aborted id=%lu reason=backend_rejected\n", (unsigned long)id);
            capture_abort(millis());
          }
        }
      }
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
  if (AUDIO_PERIODIC_CAPTURE) {
    capture_init(AUDIO_CAPTURE_SAMPLE_COUNT, AUDIO_CAPTURE_INTERVAL_MS, AUDIO_CAPTURE_TIMEOUT_MS);
    Serial.printf("[capture] mode=periodic duration_ms=%u interval_ms=%u timeout_ms=%u\n",
                  AUDIO_CAPTURE_DURATION_MS, AUDIO_CAPTURE_INTERVAL_MS, AUDIO_CAPTURE_TIMEOUT_MS);
  }
  snprintf(g_stream_id, sizeof(g_stream_id), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  if(AUDIO_PERIODIC_CAPTURE)
    Serial.printf("[capture] session stream_id=%s ring_ms=%u nonblocking=1\n",g_stream_id,AUDIO_RING_BUFFER_MS);
  reconnect_backoff_init(&g_ws_backoff, RECONNECT_BACKOFF_INITIAL_MS, RECONNECT_BACKOFF_MAX_MS);
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) g_seq_no[ch] = 0;

  g_frame_buf_size = WEBSOCKETS_MAX_HEADER_SIZE + (AUDIO_WS_BINARY ? audio_frame_binary_max_size(CHUNK_SAMPLE_COUNT) :
                                           audio_frame_max_size(CHUNK_SAMPLE_COUNT));
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

static bool pump_audio_frame() {
  uint32_t started=micros();
  auto result=g_pending.pump(write_audio,nullptr,millis());
  stats_max(&WsClientStats::max_pump_us,micros()-started);
  if(result==FRAME_PENDING)return true;
  uint32_t elapsed=micros()-g_frame_started_us;
  stats_max(&WsClientStats::max_send_us,elapsed);
  if(result!=FRAME_COMPLETE) {
    stats_increment(&WsClientStats::send_failures);
    Serial.printf("[capture] frame_failed id=%lu elapsed_us=%lu\n",
                  (unsigned long)g_frame_capture.id,(unsigned long)elapsed);
    return false;
  }
  stats_increment(&WsClientStats::chunks_sent);
  if(!g_first_chunk_logged) {
    g_first_chunk_logged=true;
    Serial.printf("[capture] first_chunk id=%lu recording=%d retained=%u elapsed_ms=%lu send_us=%lu\n",
                  (unsigned long)g_frame_capture.id,g_frame_capture.state==CAPTURE_RECORDING,
                  (unsigned)g_frame_capture.retained,
                  (unsigned long)(millis()-g_frame_capture.start_ms),(unsigned long)elapsed);
  }
  return true;
}

// Mengirim maksimal WS_MAX_CHUNKS_PER_CHANNEL_PER_TICK chunk untuk channel tsb.
// Hanya chunk PENUH yang dikirim — chunk_duration_ms harus jujur karena backend
// menyusun segmen 5 detik dari akumulasi field ini (§2.2/§4.1).
static bool send_ready_chunks(uint8_t channel_index) {
  if(AUDIO_PERIODIC_CAPTURE && g_pending.pending())return pump_audio_frame();
  for (uint8_t k = 0; k < (AUDIO_PERIODIC_CAPTURE ? 1 : WS_MAX_CHUNKS_PER_CHANNEL_PER_TICK); k++) {
    size_t available = buffer_manager_available(channel_index);
    if (available < CHUNK_SAMPLE_COUNT) return true;

    uint64_t backlog_ms = (uint64_t)available * 1000ULL / AUDIO_SAMPLE_RATE_HZ;
    uint64_t now_ms = network_manager_get_epoch_ms();
    uint64_t first_sample_ms = (now_ms > backlog_ms) ? now_ms - backlog_ms : 0;

    uint64_t sample_start = 0;
    size_t got = buffer_manager_read_chunk(channel_index, g_pcm_chunk, CHUNK_SAMPLE_COUNT,
                                           &sample_start);
    if (got != CHUNK_SAMPLE_COUNT) return true;

    if (AUDIO_PERIODIC_CAPTURE && !g_delivery.consume(channel_index, sample_start, got)) {
      Serial.printf("[capture] discontinuity id=%lu channel=%u sample_start=%llu\n",
                    (unsigned long)capture_snapshot().id, AUDIO_CHANNEL_ID_MAP[channel_index],
                    (unsigned long long)sample_start);
      return false;
    }

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
    if (AUDIO_PERIODIC_CAPTURE) {
      auto capture = capture_snapshot();
      header.capture_id = capture.id;
      header.capture_duration_ms = AUDIO_CAPTURE_DURATION_MS;
      header.capture_offset_samples = g_delivery.sent(channel_index) - got;
      header.capture_start_ms = capture.epoch_ms;
      header.timestamp_ms = capture.epoch_ms + header.capture_offset_samples * 1000ULL / AUDIO_SAMPLE_RATE_HZ;
    }
    // seq_no naik walau kirim gagal: chunk itu memang hilang, dan gap seq_no
    // adalah cara backend mendeteksinya (§2.3).
    g_seq_no[channel_index]++;

    char *json = (char *)(g_frame_buf + WEBSOCKETS_MAX_HEADER_SIZE);
    uint32_t build_started_us = micros();
    size_t len = AUDIO_WS_BINARY
        ? audio_frame_build_binary((uint8_t *)json, g_frame_buf_size - WEBSOCKETS_MAX_HEADER_SIZE,
                                   header, g_pcm_chunk, got)
        : audio_frame_build(json, g_frame_buf_size - WEBSOCKETS_MAX_HEADER_SIZE, header,
                            g_pcm_chunk, got);
    uint32_t build_us = micros() - build_started_us;
    stats_max(&WsClientStats::max_build_us, build_us);
    if (len == 0) {
      stats_increment(&WsClientStats::send_failures);
      Serial.println("[ws] BUG: buffer pesan terlalu kecil untuk 1 chunk.");
      return false;
    }

#if AUDIO_TRANSPORT_TEST_STALL_MS > 0
    static bool test_stalled = false;
    if (!test_stalled) {
      test_stalled = true;
      Serial.printf("[transport] test_stall_begin_ms=%u\n", AUDIO_TRANSPORT_TEST_STALL_MS);
      delay(AUDIO_TRANSPORT_TEST_STALL_MS);
      Serial.println("[transport] test_stall_end=1");
    }
#endif
    if(AUDIO_PERIODIC_CAPTURE) {
      g_frame_capture=capture_snapshot();g_frame_started_us=micros();
      if(!g_pending.begin(g_frame_buf,len,esp_random(),AUDIO_WS_BINARY?2:1,
                          millis(),AUDIO_CAPTURE_FRAME_TIMEOUT_MS))return false;
      return pump_audio_frame();
    }
    uint32_t send_started_us = micros();
    bool sent = AUDIO_WS_BINARY ? g_ws.sendBIN(g_frame_buf, len, true)
                                : g_ws.sendTXT(g_frame_buf, len, true);
    uint32_t send_us = micros() - send_started_us;
    stats_max(&WsClientStats::max_send_us, send_us);
    if (!sent) {
      stats_increment(&WsClientStats::send_failures);
      return false; // koneksi kemungkinan putus — berhenti untuk tick ini
    }
    stats_increment(&WsClientStats::chunks_sent);

  }
  return true;
}

void ws_client_tick(bool streaming_enabled) {
  if (network_manager_is_connected()) {
    // Never insert a library ping/pong/control frame into a partial audio frame.
    auto poll_capture = capture_snapshot();
    if(!AUDIO_PERIODIC_CAPTURE || ws_may_poll(g_pending.pending(),g_ws_connected,poll_capture.state)) {
      uint32_t started=micros();g_ws.loop();
      uint32_t elapsed=micros()-started;
      stats_max(&WsClientStats::max_loop_us,elapsed);
      if(elapsed>50000)
        Serial.printf("[transport] ws_poll_us=%lu capture=%lu state=%u connected=%d\n",
                      (unsigned long)elapsed,(unsigned long)poll_capture.id,
                      (unsigned)poll_capture.state,(int)g_ws_connected.load());
    }
  } else if (g_ws_connected) {
    // Tanpa WiFi, percobaan TCP connect hanya membuang waktu task network.
    cancel_audio_frame();g_ws.abort_audio();
  }

  bool can_send = g_ws_connected && streaming_enabled;
  if (AUDIO_PERIODIC_CAPTURE) {
    auto capture = capture_snapshot();
    if(capture.state!=CAPTURE_IDLE && capture.source_fault!=CAPTURE_SOURCE_OK) {
      Serial.printf("[capture] aborted id=%lu reason=%s lost_samples=unknown retained=%u\n",
                    (unsigned long)capture.id,
                    capture.source_fault==CAPTURE_SOURCE_DMA_GAP?"source_dma_gap":"source_i2s_gap",
                    (unsigned)capture.retained);
      capture_abort(millis());cancel_audio_frame();g_ws.abort_audio();return;
    }
    if (!can_send || capture_expired(millis())) {
      if (capture.state != CAPTURE_IDLE) {
        Serial.printf("[capture] aborted id=%lu reason=%s\n", (unsigned long)capture.id,
                      can_send ? "timeout" : "disconnected");
        capture_abort(millis());
        if(g_pending.pending()){cancel_audio_frame();g_ws.abort_audio();}
      }
      for (uint8_t ch=0;ch<NUM_AUDIO_CHANNELS;ch++) buffer_manager_clear(ch);
      return;
    }
    if (capture.state == CAPTURE_IDLE) {
      for (uint8_t ch=0;ch<NUM_AUDIO_CHANNELS;ch++) buffer_manager_clear(ch);
      uint32_t drops[4];
      for (uint8_t ch=0;ch<NUM_AUDIO_CHANNELS;ch++) drops[ch]=buffer_manager_get_drop_count(ch);
      if (capture_begin(millis(), network_manager_get_epoch_ms())) {
        auto next = capture_snapshot();
        g_delivery.begin(next.id,AUDIO_CAPTURE_SAMPLE_COUNT,drops);
        g_next_channel=0;g_first_chunk_logged=false;
        Serial.printf("[capture] started id=%lu duration_ms=%u start_ms=%lu\n",(unsigned long)next.id,AUDIO_CAPTURE_DURATION_MS,(unsigned long)next.start_ms);
      }
      return;
    }
    if (capture.state == CAPTURE_WAIT_ACK) return;
    uint32_t drops[4];
    for (uint8_t ch=0;ch<NUM_AUDIO_CHANNELS;ch++) drops[ch]=buffer_manager_get_drop_count(ch);
    if (g_delivery.overflowed(drops)) {
      Serial.printf("[capture] aborted id=%lu reason=ring_overflow\n",(unsigned long)capture.id);
      capture_abort(millis());cancel_audio_frame();g_ws.abort_audio();return;
    }
    if (capture.state == CAPTURE_READY && g_ready_logged != capture.id) {
      g_ready_logged=capture.id;
      Serial.printf("[capture] ready id=%lu elapsed_ms=%lu\n",(unsigned long)capture.id,
                    (unsigned long)(millis()-capture.start_ms));
      for (uint8_t ch=0;ch<NUM_AUDIO_CHANNELS;ch++) {
        if (buffer_manager_available(ch)+g_delivery.sent(ch) != AUDIO_CAPTURE_SAMPLE_COUNT) {
          Serial.printf("[capture] aborted id=%lu reason=incomplete_channel\n",(unsigned long)capture.id);
          capture_abort(millis());cancel_audio_frame();g_ws.abort_audio();return;
        }
      }
    }
    uint8_t ch=g_next_channel;
    if ((g_pending.pending() || g_delivery.sent(ch) < AUDIO_CAPTURE_SAMPLE_COUNT) && !send_ready_chunks(ch)) {
      Serial.printf("[capture] aborted id=%lu reason=send_failed_or_gap\n",(unsigned long)capture.id);
      capture_abort(millis());cancel_audio_frame();g_ws.abort_audio();return;
    }
    // Acquisition on the other core may have advanced while this tick sent bytes.
    for (uint8_t i=0;i<NUM_AUDIO_CHANNELS;i++) drops[i]=buffer_manager_get_drop_count(i);
    if (g_delivery.overflowed(drops)) {
      Serial.printf("[capture] aborted id=%lu reason=ring_overflow\n",(unsigned long)capture.id);
      capture_abort(millis());cancel_audio_frame();g_ws.abort_audio();return;
    }
    if(!g_pending.pending())g_next_channel=(g_next_channel+1)%NUM_AUDIO_CHANNELS;
    if(!g_pending.pending() && g_delivery.complete() && capture_snapshot().state == CAPTURE_READY) {
      capture_mark_sent();
      Serial.printf("[capture] sent id=%lu awaiting_ack=1 elapsed_ms=%lu\n",
                    (unsigned long)capture.id,(unsigned long)(millis()-capture.start_ms));
    }
    return;
  }

  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    if (!can_send) {
      buffer_manager_clear(ch);
      continue;
    }
    if (!send_ready_chunks(ch)) break;
  }
}

void ws_client_get_stats(WsClientStats *out) {
  platform_lock_enter(&g_stats_lock);
  *out = g_stats;
  platform_lock_exit(&g_stats_lock);
}
