// DIAGNOSTIK: MQTT ke broker (env diag_mqtt). Tanpa sensor PPG.
//
// Cek: koneksi + autentikasi broker (MQTT_AUTH_USERNAME + DEVICE_API_TOKEN),
// publish status & batch PPG sintetis dengan skema INTEGRATION_CONTRACT.md §3,
// dan LOOPBACK (subscribe ke topik sendiri) untuk membuktikan pesan benar-benar
// melewati broker, lengkap dengan latensinya. Memakai DIAG_DEVICE_ID.

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <config.h>
#include <math.h>
#include <secrets.h>

#include "network_manager.h"
#include "ppg_payload.h"

#define PUBLISH_INTERVAL_MS 5000

static WiFiClient g_net;
static PubSubClient g_mqtt(g_net);
static char g_topic_ppg[64], g_topic_status[64];
static char g_payload[MQTT_BUFFER_SIZE];
static uint32_t g_published_at_us = 0;
static bool g_waiting_loopback = false;
static uint32_t g_loopbacks = 0;

static const char *state_name(int state) {
  switch (state) {
    case -4: return "timeout (broker tidak menjawab)";
    case -3: return "koneksi putus";
    case -2: return "TCP gagal (IP/port salah, broker mati, firewall)";
    case -1: return "terputus";
    case 0: return "terhubung";
    case 4: return "username/password ditolak (cek DEVICE_API_TOKEN & passwd broker)";
    case 5: return "tidak diizinkan";
    default: return "lainnya";
  }
}

static void on_message(char *topic, uint8_t *payload, unsigned int length) {
  if (g_waiting_loopback && strcmp(topic, g_topic_ppg) == 0) {
    g_waiting_loopback = false;
    g_loopbacks++;
    Serial.printf("[mqtt] PASS loopback: pesan %u byte kembali dari broker dalam %.1f ms\n", length,
                  (micros() - g_published_at_us) / 1000.0f);
  }
}

static void publish_test() {
  // Status §3.4 (tidak retained supaya diag tidak meninggalkan status palsu).
  size_t len = status_payload_build(g_payload, sizeof(g_payload), DIAG_DEVICE_ID, "online",
                                    network_manager_get_epoch_ms(), true, -1);
  bool ok_status = g_mqtt.publish(g_topic_status, (const uint8_t *)g_payload, len, false);

  // Batch PPG §3.3 ukuran penuh (sinyal sinus ~72 bpm) — sekaligus menguji MQTT_BUFFER_SIZE.
  static uint32_t ir[PPG_BATCH_SIZE], red[PPG_BATCH_SIZE];
  for (int i = 0; i < PPG_BATCH_SIZE; i++) {
    float beat = sinf(2.0f * 3.14159265f * 1.2f * i / PPG_SAMPLE_RATE_HZ);
    ir[i] = (uint32_t)(120000 + 800 * beat);
    red[i] = (uint32_t)(90000 + 500 * beat);
  }
  len = ppg_payload_build(g_payload, sizeof(g_payload), DIAG_DEVICE_ID,
                          network_manager_get_epoch_ms(), PPG_SAMPLE_RATE_HZ, ir, red, PPG_BATCH_SIZE);
  g_published_at_us = micros();
  bool ok_ppg = len > 0 && g_mqtt.publish(g_topic_ppg, (const uint8_t *)g_payload, len, false);
  g_waiting_loopback = ok_ppg;

  Serial.printf("[mqtt] publish status=%s ppg(%u byte)=%s\n", ok_status ? "OK" : "GAGAL",
                (unsigned)len, ok_ppg ? "OK" : "GAGAL (payload > MQTT_BUFFER_SIZE?)");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== DIAG: MQTT %s:%d (device %s, user %s) ===\n", MQTT_BROKER_HOST,
                MQTT_BROKER_PORT, DIAG_DEVICE_ID, MQTT_AUTH_USERNAME);
  snprintf(g_topic_ppg, sizeof(g_topic_ppg), MQTT_TOPIC_PPG_FMT, DIAG_DEVICE_ID);
  snprintf(g_topic_status, sizeof(g_topic_status), MQTT_TOPIC_STATUS_FMT, DIAG_DEVICE_ID);

  network_manager_init();
  g_mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  g_mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  g_mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  g_mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
  g_mqtt.setCallback(on_message);
}

void loop() {
  static uint32_t last_attempt = 0, last_publish = 0;
  network_manager_tick();
  uint32_t now = millis();

  if (!network_manager_is_connected()) {
    delay(100);
    return;
  }

  if (!g_mqtt.connected()) {
    if (now - last_attempt < 3000) return;
    last_attempt = now;
    char client_id[40];
    snprintf(client_id, sizeof(client_id), "%s-diag", DIAG_DEVICE_ID);
    if (g_mqtt.connect(client_id, MQTT_AUTH_USERNAME, DEVICE_API_TOKEN)) {
      Serial.println("[mqtt] PASS terhubung & autentikasi diterima broker.");
      g_mqtt.subscribe(g_topic_ppg);
    } else {
      Serial.printf("[mqtt] FAIL connect: state=%d (%s)\n", g_mqtt.state(), state_name(g_mqtt.state()));
    }
    return;
  }

  g_mqtt.loop();
  if (now - last_publish >= PUBLISH_INTERVAL_MS) {
    if (g_waiting_loopback) {
      Serial.println("[mqtt] WARN loopback sebelumnya tidak kembali (ACL broker melarang "
                     "subscribe? publish tetap bisa berhasil).");
    }
    last_publish = now;
    publish_test();
  }
  delay(5);
}
