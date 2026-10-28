// DIAGNOSTIK: websocket audio ke backend (env diag_ws). Tanpa mikrofon.
//
// Mengirim nada sinus sintetis untuk 4 channel dengan format pesan & laju yang
// SAMA dengan firmware produksi (CHUNK_DURATION_MS, AUDIO_SAMPLE_RATE_HZ),
// memakai DIAG_DEVICE_ID supaya tidak tertaut ke pasien demo.
//
// Cek: handshake + token (INTEGRATION_CONTRACT.md §6), dan apakah bandwidth
// WiFi->backend cukup untuk 4 channel real-time (waktu kirim per putaran harus
// jauh di bawah CHUNK_DURATION_MS). Dengarkan/plot segmen di backend untuk
// device pneumacare-0000: harus terdengar nada bersih 200/400/600/800 Hz —
// bukti end-to-end bahwa PCM int16 LE terdekode benar (§2.3).

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <config.h>
#include <math.h>
#include <secrets.h>

#include "audio_frame.h"
#include "network_manager.h"

#define REPORT_INTERVAL_MS 5000
#define TONE_AMPLITUDE 8000
#define DIAG_CHANNELS 4

static WebSocketsClient g_ws;
static bool g_connected = false;
static uint32_t g_connected_at_ms = 0;
static uint32_t g_quick_disconnects = 0;
static char g_auth_header[160];

static int16_t g_pcm[CHUNK_SAMPLE_COUNT];
static uint8_t *g_frame = NULL;
static size_t g_frame_size = 0;
static uint32_t g_seq[DIAG_CHANNELS] = {0};
static uint32_t g_phase[DIAG_CHANNELS] = {0};

static uint32_t g_sent = 0, g_failed = 0, g_bytes = 0, g_send_us_total = 0, g_rounds = 0;
static uint32_t g_max_round_ms = 0;

static void on_event(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    g_connected = true;
    g_connected_at_ms = millis();
    Serial.println("[ws] PASS handshake diterima backend (token valid).");
  } else if (type == WStype_DISCONNECTED) {
    if (g_connected && millis() - g_connected_at_ms < 2000) {
      g_quick_disconnects++;
      Serial.println("[ws] FAIL koneksi langsung ditutup server — kemungkinan token "
                     "DEVICE_API_TOKEN (secrets.h) beda dengan DEVICE_AUTH_TOKEN backend.");
    } else if (g_connected) {
      Serial.println("[ws] Terputus.");
    }
    g_connected = false;
  }
}

static void send_round(uint32_t round_start_ms) {
  uint64_t ts = network_manager_get_epoch_ms();
  for (uint8_t ch = 0; ch < DIAG_CHANNELS; ch++) {
    float freq = 200.0f * (ch + 1);
    for (size_t i = 0; i < CHUNK_SAMPLE_COUNT; i++) {
      g_pcm[i] = (int16_t)(TONE_AMPLITUDE * sinf(2.0f * 3.14159265f * freq * (g_phase[ch] + i) /
                                                 AUDIO_SAMPLE_RATE_HZ));
    }
    // Semua frekuensi kelipatan 200 Hz -> genap berulang tiap 1 detik; wrap
    // menjaga presisi float sinf() saat program berjalan berjam-jam.
    g_phase[ch] = (g_phase[ch] + CHUNK_SAMPLE_COUNT) % AUDIO_SAMPLE_RATE_HZ;

    AudioFrameHeader h = {DIAG_DEVICE_ID, (uint8_t)(ch + 1), AUDIO_SAMPLE_RATE_HZ, ts,
                          g_seq[ch]++, CHUNK_DURATION_MS};
    char *json = (char *)(g_frame + WEBSOCKETS_MAX_HEADER_SIZE);
    size_t len = audio_frame_build(json, g_frame_size - WEBSOCKETS_MAX_HEADER_SIZE, h, g_pcm,
                                   CHUNK_SAMPLE_COUNT);
    uint32_t t0 = micros();
    if (len > 0 && g_ws.sendTXT(g_frame, len, true)) {
      g_sent++;
      g_bytes += len;
    } else {
      g_failed++;
    }
    g_send_us_total += micros() - t0;
  }
  uint32_t round_ms = millis() - round_start_ms;
  if (round_ms > g_max_round_ms) g_max_round_ms = round_ms;
  g_rounds++;
}

static void report() {
  float avg_round_ms = g_rounds ? g_send_us_total / 1000.0f / g_rounds : 0;
  float kbps = g_bytes * 8.0f / REPORT_INTERVAL_MS;
  const char *verdict = !g_connected ? "FAIL tidak terhubung"
                        : g_failed > 0 ? "FAIL ada pengiriman gagal"
                        : avg_round_ms < CHUNK_DURATION_MS * 0.5f ? "PASS bandwidth cukup"
                        : avg_round_ms < CHUNK_DURATION_MS ? "WARN bandwidth mepet"
                                                           : "FAIL bandwidth tidak cukup (backlog)";
  Serial.printf("[ws] terkirim=%lu gagal=%lu | %.0f kbit/s | kirim 4 ch rata2 %.1f ms, maks %lu ms "
                "(budget %d ms) | RSSI %d -> %s\n",
                (unsigned long)g_sent, (unsigned long)g_failed, kbps, avg_round_ms,
                (unsigned long)g_max_round_ms, CHUNK_DURATION_MS, network_manager_rssi(), verdict);
  g_sent = g_failed = g_bytes = g_send_us_total = g_rounds = g_max_round_ms = 0;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== DIAG: websocket ws://%s:%d%s (device %s) ===\n", BACKEND_WS_HOST,
                BACKEND_WS_PORT, WS_PATH, DIAG_DEVICE_ID);

  g_frame_size = WEBSOCKETS_MAX_HEADER_SIZE + audio_frame_max_size(CHUNK_SAMPLE_COUNT);
  g_frame = (uint8_t *)malloc(g_frame_size);

  network_manager_init();
  snprintf(g_auth_header, sizeof(g_auth_header), WS_AUTH_HEADER_FMT, DEVICE_API_TOKEN);
  g_ws.setExtraHeaders(g_auth_header);
  g_ws.onEvent(on_event);
  g_ws.setReconnectInterval(3000);
  g_ws.begin(BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH);
}

void loop() {
  static uint32_t next_round_ms = 0;
  static uint32_t last_report_ms = 0;

  network_manager_tick();
  if (network_manager_is_connected()) g_ws.loop();

  uint32_t now = millis();
  if (g_connected && (int32_t)(now - next_round_ms) >= 0) {
    next_round_ms = now + CHUNK_DURATION_MS;
    send_round(now);
  }
  if (now - last_report_ms >= REPORT_INTERVAL_MS) {
    last_report_ms = now;
    report();
  }
  delay(2);
}
