// DIAGNOSTIK: WiFi + jangkauan backend (env diag_wifi).
//
// Cek: koneksi WiFi (kredensial secrets.h), kualitas sinyal (RSSI), DHCP/IP,
// sinkronisasi NTP, dan apakah port websocket & MQTT backend bisa dijangkau
// TCP dari ESP32 (membedakan "WiFi bermasalah" vs "backend/firewall bermasalah").
// Uji reconnect: matikan router saat program berjalan, nyalakan lagi, dan
// pastikan log menunjukkan WiFi tersambung kembali sendiri (FR-HW-024).

#include <Arduino.h>
#include <WiFi.h>
#include <config.h>
#include <secrets.h>

#include "network_manager.h"

#define REPORT_INTERVAL_MS 10000
#define TCP_TEST_TIMEOUT_MS 3000

static const char *rssi_grade(int rssi) {
  if (rssi >= -60) return "sangat baik";
  if (rssi >= -70) return "baik";
  if (rssi >= -80) return "LEMAH — streaming audio 4 channel bisa tersendat";
  return "BURUK — streaming hampir pasti putus-putus";
}

static void test_tcp(const char *name, const char *host, uint16_t port) {
  WiFiClient client;
  uint32_t start = millis();
  bool ok = client.connect(host, port, TCP_TEST_TIMEOUT_MS);
  uint32_t elapsed = millis() - start;
  client.stop();
  if (ok) {
    Serial.printf("[wifi] PASS %s %s:%u terjangkau (%lu ms)\n", name, host, port, (unsigned long)elapsed);
  } else {
    Serial.printf("[wifi] FAIL %s %s:%u tidak terjangkau — server belum jalan, IP salah di "
                  "config.h, beda subnet, atau firewall laptop memblokir port.\n",
                  name, host, port);
  }
}

static void report() {
  Serial.println("------------------------------------------------------------------");
  if (!network_manager_is_connected()) {
    Serial.printf("[wifi] FAIL belum terhubung ke \"%s\" (status=%d). Cek SSID/password di "
                  "secrets.h, router 2.4 GHz (ESP32 tidak mendukung 5 GHz).\n",
                  WIFI_SSID, WiFi.status());
    return;
  }
  int rssi = WiFi.RSSI();
  Serial.printf("[wifi] PASS terhubung: IP=%s gateway=%s dns=%s\n", WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
  Serial.printf("[wifi] RSSI %d dBm (%s), channel %d, MAC %s\n", rssi, rssi_grade(rssi),
                WiFi.channel(), WiFi.macAddress().c_str());
  Serial.printf("[wifi] %s NTP: %s\n", network_manager_is_time_synced() ? "PASS" : "WARN",
                network_manager_is_time_synced()
                    ? "tersinkron"
                    : "belum sinkron (internet tidak ada? firmware tetap jalan dengan fallback)");
  if (network_manager_is_time_synced()) {
    time_t now = time(NULL);
    Serial.printf("[wifi] waktu UTC: %s", asctime(gmtime(&now)));
  }
  test_tcp("websocket backend", BACKEND_WS_HOST, BACKEND_WS_PORT);
  test_tcp("broker MQTT", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== DIAG: WiFi & jangkauan backend ===");
  network_manager_init();
}

void loop() {
  static uint32_t last_report = 0;
  network_manager_tick();
  if (millis() - last_report >= REPORT_INTERVAL_MS) {
    last_report = millis();
    report();
  }
  delay(50);
}
