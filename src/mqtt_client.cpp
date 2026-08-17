#include "mqtt_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <config.h>
#include <secrets.h>

#include "network_manager.h"
#include "reconnect_manager.h"

// Buffer MQTT default (MQTT_MAX_PACKET_SIZE=256) terlalu kecil untuk payload
// PPG_BATCH_SIZE sample — dinaikkan lewat setBufferSize() di init.
#define MQTT_BUFFER_SIZE 1024

static WiFiClient g_wifi_client;
static PubSubClient g_mqtt(g_wifi_client);
static ReconnectBackoff g_mqtt_backoff;

static char g_topic_ppg[64];
static char g_topic_status[64];
static char g_mqtt_client_id[32];

static uint32_t g_ppg_batch[PPG_BATCH_SIZE];
static size_t g_ppg_batch_count = 0;

void mqtt_client_init() {
  reconnect_backoff_reset(&g_mqtt_backoff);
  g_ppg_batch_count = 0;

  snprintf(g_topic_ppg, sizeof(g_topic_ppg), MQTT_TOPIC_PPG_FMT, DEVICE_ID);
  snprintf(g_topic_status, sizeof(g_topic_status), MQTT_TOPIC_STATUS_FMT, DEVICE_ID);
  snprintf(g_mqtt_client_id, sizeof(g_mqtt_client_id), "%s", DEVICE_ID);

  g_mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  g_mqtt.setBufferSize(MQTT_BUFFER_SIZE);

  // Autentikasi minimal — INTEGRATION_CONTRACT.md §6: username tetap
  // MQTT_AUTH_USERNAME, token dipakai sebagai password (bukan kelas
  // produksi, cukup untuk demo — lihat backend/mosquitto/README.md §2.1).
  if (g_mqtt.connect(g_mqtt_client_id, MQTT_AUTH_USERNAME, DEVICE_API_TOKEN)) {
    Serial.println("[mqtt] Terhubung ke broker.");
  } else {
    Serial.printf("[mqtt] Gagal connect awal (state=%d), akan retry via reconnect_tick.\n",
                  g_mqtt.state());
  }
}

bool mqtt_client_is_connected() {
  return g_mqtt.connected();
}

void mqtt_client_push_ppg_sample(uint32_t ir_sample) {
  if (g_ppg_batch_count >= PPG_BATCH_SIZE) return; // batch penuh, tunggu tick() flush
  g_ppg_batch[g_ppg_batch_count++] = ir_sample;
}

static void flush_ppg_batch_if_ready() {
  if (g_ppg_batch_count < PPG_BATCH_SIZE) return;
  if (!g_mqtt.connected()) {
    // Backend belum siap menerima — buang batch supaya tidak menahan RAM
    // tanpa batas (kebijakan sama seperti drop-oldest di buffer_manager,
    // SDD §4), bukan menumpuk sample PPG lama yang sudah tidak relevan.
    g_ppg_batch_count = 0;
    return;
  }

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["timestamp_ms"] = network_manager_get_epoch_ms();
  doc["sample_rate_hz"] = PPG_SAMPLE_RATE_HZ;
  JsonArray samples = doc["samples"].to<JsonArray>();
  for (size_t i = 0; i < g_ppg_batch_count; i++) {
    samples.add(g_ppg_batch[i]);
  }

  String out;
  serializeJson(doc, out);
  g_mqtt.publish(g_topic_ppg, out.c_str());

  g_ppg_batch_count = 0;
}

void mqtt_client_publish_heartbeat() {
  if (!g_mqtt.connected()) return;

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["status"] = "online";
  doc["timestamp_ms"] = network_manager_get_epoch_ms();
  // ⚠️ battery_pct: PIN_MAPPING_BOM.md §1.3 "Battery voltage sense (ADC)"
  // masih TODO (belum dikonfirmasi Alfito) — FR-HW-030 bersifat opsional.
  // Placeholder eksplisit -1 (bukan angka valid) sampai sirkuit pembagi
  // tegangan tersedia, supaya backend tidak salah mengira ini nilai nyata.
  doc["battery_pct"] = -1;

  String out;
  serializeJson(doc, out);
  // retained=true sesuai §3.2: dashboard perlu tahu status terakhir meski
  // baru subscribe setelah device sudah online.
  g_mqtt.publish(g_topic_status, out.c_str(), true);
}

void mqtt_client_tick() {
  if (g_mqtt.connected()) {
    g_mqtt.loop();
    flush_ppg_batch_if_ready();
  }
}

void mqtt_client_reconnect_tick() {
  if (g_mqtt.connected()) {
    reconnect_backoff_reset(&g_mqtt_backoff);
    return;
  }
  if (!network_manager_is_connected()) return; // tunggu WiFi pulih dulu

  uint32_t now = millis();
  if (!reconnect_backoff_should_attempt(&g_mqtt_backoff, now)) return;

  reconnect_backoff_notify_attempt(&g_mqtt_backoff, now);
  Serial.printf("[mqtt] Mencoba reconnect (backoff=%lums, state=%d)...\n",
                (unsigned long)g_mqtt_backoff.current_delay_ms, g_mqtt.state());

  if (g_mqtt.connect(g_mqtt_client_id, MQTT_AUTH_USERNAME, DEVICE_API_TOKEN)) {
    Serial.println("[mqtt] Reconnect berhasil.");
  }
}
