#include "ws_client.h"

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <config.h>
#include <secrets.h>

#include "buffer_manager.h"
#include "network_manager.h"
#include "reconnect_manager.h"

static WebSocketsClient g_ws;
static bool g_ws_connected = false;
static ReconnectBackoff g_ws_backoff;
static uint32_t g_seq_no[NUM_AUDIO_CHANNELS];

// Buffer sample mentah dibaca dari ring buffer (int32, hasil audio_acquisition)
// sebelum dikonversi ke int16 LE untuk dikirim. Ukuran dikunci sama dengan
// CHUNK_SAMPLE_COUNT_TEST (lihat config.h) mengikuti kapasitas ring buffer
// Fase 2 saat ini — tinjau ulang bersamaan saat driver I2S per-sample nyata
// diimplementasikan (Langkah 1b).
static int32_t g_raw_chunk_buf[CHUNK_SAMPLE_COUNT_TEST];
static int16_t g_pcm16_buf[CHUNK_SAMPLE_COUNT_TEST];

static void on_ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      g_ws_connected = true;
      reconnect_backoff_reset(&g_ws_backoff);
      Serial.println("[ws] Terhubung ke backend.");
      break;
    case WStype_DISCONNECTED:
      g_ws_connected = false;
      Serial.println("[ws] Terputus dari backend.");
      break;
    case WStype_ERROR:
      Serial.println("[ws] Error event.");
      break;
    default:
      break;
  }
}

void ws_client_init() {
  reconnect_backoff_reset(&g_ws_backoff);
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    g_seq_no[ch] = 0;
  }

  char auth_header[128];
  snprintf(auth_header, sizeof(auth_header), WS_AUTH_HEADER_FMT, DEVICE_API_TOKEN);
  g_ws.setExtraHeaders(auth_header);

  g_ws.onEvent(on_ws_event);
  g_ws.begin(BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH);
}

bool ws_client_is_connected() {
  return g_ws_connected;
}

// Mengirim satu chunk untuk channel_index tsb bila ada data siap di
// buffer_manager. Konversi int32 (hasil audio_acquisition) -> int16 LE
// dilakukan di sini, EKSPLISIT, sebelum base64 encode — lihat catatan
// kritis di ws_client.h dan INTEGRATION_CONTRACT.md §2.3.
static void send_chunk_if_ready(uint8_t channel_index) {
  size_t available = buffer_manager_available(channel_index);
  if (available == 0) return;

  size_t got = buffer_manager_read_chunk(channel_index, g_raw_chunk_buf,
                                          CHUNK_SAMPLE_COUNT_TEST);
  if (got == 0) return;

  // Konversi eksplisit ke int16 LE. audio_acquisition_read() saat ini
  // mengembalikan statistik agregat (avg_abs, sudah dalam rentang int32 hasil
  // shift >>8 dari sample 24-bit INMP441 mentah) sebagai proxy sample logis —
  // clamp ke rentang int16 supaya tidak wrap-around/overflow saat cast.
  for (size_t i = 0; i < got; i++) {
    int32_t v = g_raw_chunk_buf[i];
    if (v > INT16_MAX) v = INT16_MAX;
    if (v < INT16_MIN) v = INT16_MIN;
    g_pcm16_buf[i] = (int16_t)v; // ESP32 little-endian secara native
  }

  String pcm_base64 = base64::encode((const uint8_t *)g_pcm16_buf, got * sizeof(int16_t));

  uint8_t channel_id = AUDIO_CHANNEL_ID_MAP[channel_index];

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["channel_id"] = channel_id;
  doc["sample_rate"] = AUDIO_SAMPLE_RATE_HZ;
  doc["bit_depth"] = 16;
  doc["timestamp_ms"] = network_manager_get_epoch_ms();
  doc["seq_no"] = g_seq_no[channel_index]++;
  doc["chunk_duration_ms"] = CHUNK_DURATION_MS;
  doc["pcm_base64"] = pcm_base64;

  String out;
  serializeJson(doc, out);
  g_ws.sendTXT(out);
}

void ws_client_tick() {
  g_ws.loop();

  if (!g_ws_connected) return;

  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    send_chunk_if_ready(ch);
  }
}

void ws_client_reconnect_tick() {
  if (g_ws_connected) return;
  if (!network_manager_is_connected()) return; // tunggu WiFi pulih dulu

  uint32_t now = millis();
  if (!reconnect_backoff_should_attempt(&g_ws_backoff, now)) return;

  reconnect_backoff_notify_attempt(&g_ws_backoff, now);
  Serial.printf("[ws] Mencoba reconnect (backoff=%lums)...\n",
                (unsigned long)g_ws_backoff.current_delay_ms);
  g_ws.disconnect();
  g_ws.begin(BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH);
}
