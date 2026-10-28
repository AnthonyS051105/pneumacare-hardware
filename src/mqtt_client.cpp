#include "mqtt_client.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <config.h>
#include <secrets.h>

#include "network_manager.h"
#include "ppg_payload.h"
#include "reconnect_manager.h"

static WiFiClient g_wifi_client;
static PubSubClient g_mqtt(g_wifi_client);
static ReconnectBackoff g_mqtt_backoff;
static const char *g_device_id = DEVICE_ID;

static char g_topic_ppg[64];
static char g_topic_status[64];
static char g_will_payload[128];
// PubSubClient merakit paket di buffer internalnya sendiri (MQTT_BUFFER_SIZE),
// payload disiapkan di sini lebih dulu.
static char g_payload[MQTT_BUFFER_SIZE];

static uint32_t g_batch_red[PPG_BATCH_SIZE];
static uint32_t g_batch_ir[PPG_BATCH_SIZE];
static size_t g_batch_count = 0;
static uint64_t g_batch_first_sample_ms = 0;

static uint32_t g_last_heartbeat_ms = 0;
static MqttClientStats g_stats = {0, 0, 0, 0};

static void publish_status_online(int battery_pct) {
  size_t len = status_payload_build(g_payload, sizeof(g_payload), g_device_id, "online",
                                    network_manager_get_epoch_ms(), true, battery_pct);
  // retained=true (§3.2): dashboard yang baru subscribe tetap tahu status terakhir.
  if (len == 0 || !g_mqtt.publish(g_topic_status, (const uint8_t *)g_payload, len, true)) {
    g_stats.publish_failures++;
  }
}

static bool try_connect() {
  // Last Will "offline" retained, tanpa battery_pct (lihat status_payload_build).
  bool ok = g_mqtt.connect(g_device_id, MQTT_AUTH_USERNAME, DEVICE_API_TOKEN, g_topic_status,
                           /*willQos=*/1, /*willRetain=*/true, g_will_payload,
                           /*cleanSession=*/true);
  if (ok) {
    g_stats.connects++;
    reconnect_backoff_reset(&g_mqtt_backoff);
    Serial.printf("[mqtt] Terhubung ke broker %s:%d.\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  }
  return ok;
}

void mqtt_client_init(const char *device_id) {
  g_device_id = device_id;
  reconnect_backoff_init(&g_mqtt_backoff, RECONNECT_BACKOFF_INITIAL_MS, RECONNECT_BACKOFF_MAX_MS);
  g_batch_count = 0;

  snprintf(g_topic_ppg, sizeof(g_topic_ppg), MQTT_TOPIC_PPG_FMT, g_device_id);
  snprintf(g_topic_status, sizeof(g_topic_status), MQTT_TOPIC_STATUS_FMT, g_device_id);
  // timestamp 0: Last Will disusun sekali saat connect, broker tidak mengisi
  // waktu — backend memakai waktu terima untuk transisi status.
  status_payload_build(g_will_payload, sizeof(g_will_payload), g_device_id, "offline", 0, false, 0);

  g_mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  g_mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  g_mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  // Batasi lama blocking menunggu CONNACK (default library 15 detik).
  g_mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
}

bool mqtt_client_is_connected() {
  return g_mqtt.connected();
}

static void flush_batch() {
  if (g_batch_count == 0) return;

  if (!g_mqtt.connected()) {
    // Broker tidak terhubung: buang batch (bukan menahan RAM tanpa batas,
    // konsisten dengan kebijakan drop di buffer_manager, SDD §4).
    g_stats.samples_dropped += g_batch_count;
    g_batch_count = 0;
    return;
  }

  size_t len = ppg_payload_build(g_payload, sizeof(g_payload), g_device_id,
                                 g_batch_first_sample_ms, PPG_SAMPLE_RATE_HZ, g_batch_ir,
                                 g_batch_red, g_batch_count);
  if (len == 0) {
    Serial.printf("[mqtt] BUG: payload PPG melebihi buffer %u byte.\n", (unsigned)MQTT_BUFFER_SIZE);
    g_stats.publish_failures++;
  } else if (!g_mqtt.publish(g_topic_ppg, (const uint8_t *)g_payload, len, false)) {
    // PubSubClient hanya mengembalikan false tanpa detail — penyebab umum:
    // koneksi putus di tengah atau paket > MQTT_BUFFER_SIZE.
    g_stats.publish_failures++;
    Serial.printf("[mqtt] Gagal publish ppg/raw (payload=%u byte).\n", (unsigned)len);
  } else {
    g_stats.batches_published++;
  }
  g_batch_count = 0;
}

void mqtt_client_push_ppg_sample(uint32_t red_sample, uint32_t ir_sample) {
  if (g_batch_count >= PPG_BATCH_SIZE) flush_batch();

  if (g_batch_count == 0) {
    // Perkiraan waktu tangkap sample pertama batch (latensi antrean ~ms).
    g_batch_first_sample_ms = network_manager_get_epoch_ms();
  }
  g_batch_red[g_batch_count] = red_sample;
  g_batch_ir[g_batch_count] = ir_sample;
  g_batch_count++;
}

void mqtt_client_tick(int battery_pct) {
  uint32_t now = millis();

  if (g_mqtt.connected()) {
    g_mqtt.loop();
    if (g_batch_count >= PPG_BATCH_SIZE) flush_batch();
    if (now - g_last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
      g_last_heartbeat_ms = now;
      publish_status_online(battery_pct);
    }
    return;
  }

  if (!network_manager_is_connected()) return; // tunggu WiFi pulih dulu
  if (!reconnect_backoff_should_attempt(&g_mqtt_backoff, now)) return;

  reconnect_backoff_notify_attempt(&g_mqtt_backoff, now);
  if (try_connect()) {
    // Langsung umumkan online (menimpa Last Will "offline" yang mungkin retained).
    g_last_heartbeat_ms = now;
    publish_status_online(battery_pct);
  } else {
    Serial.printf("[mqtt] Gagal connect (state=%d) — coba lagi dalam %lu ms.\n", g_mqtt.state(),
                  (unsigned long)g_mqtt_backoff.current_delay_ms);
  }
}

void mqtt_client_get_stats(MqttClientStats *out) {
  *out = g_stats;
}
