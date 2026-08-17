#include "network_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <config.h>
#include <secrets.h>

#include "reconnect_manager.h"

static ReconnectBackoff g_wifi_backoff;
static bool g_time_synced = false;

static void try_ntp_sync() {
  // Best-effort — FR-HW-025: kegagalan TIDAK menghentikan boot, hanya dicatat.
  configTime(0, 0, NTP_SERVER);

  uint32_t start = millis();
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 0)) {
    if (millis() - start >= NTP_SYNC_TIMEOUT_MS) {
      Serial.println("[network] NTP sync gagal (timeout) — lanjut tanpa waktu tersinkron, "
                      "timestamp_ms akan memakai fallback millis() sejak boot.");
      g_time_synced = false;
      return;
    }
    delay(200);
  }

  g_time_synced = true;
  Serial.println("[network] NTP sync berhasil.");
}

void network_manager_init() {
  reconnect_backoff_reset(&g_wifi_backoff);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[network] Menghubungkan ke WiFi SSID \"%s\"...\n", WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    // Tidak ada timeout di sini secara sengaja: SDD §3 state machine
    // WIFI_CONNECTING harus retry terus (perangkat kesehatan, tidak boleh
    // menyerah permanen). Log periodik supaya tidak diam total di serial.
    if (millis() - start > 10000) {
      Serial.printf("\n[network] Masih mencoba WiFi... (%lus)\n",
                    (unsigned long)((millis() - start) / 1000));
      start = millis();
    }
  }

  Serial.println();
  Serial.printf("[network] WiFi terhubung. IP: %s\n", WiFi.localIP().toString().c_str());

  try_ntp_sync();
}

bool network_manager_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}

void network_manager_reconnect_tick() {
  if (network_manager_is_connected()) {
    reconnect_backoff_reset(&g_wifi_backoff);
    return;
  }

  uint32_t now = millis();
  if (!reconnect_backoff_should_attempt(&g_wifi_backoff, now)) {
    return;
  }

  reconnect_backoff_notify_attempt(&g_wifi_backoff, now);
  Serial.printf("[network] WiFi terputus, mencoba reconnect (backoff=%lums)...\n",
                (unsigned long)g_wifi_backoff.current_delay_ms);
  WiFi.reconnect();
}

uint64_t network_manager_get_epoch_ms() {
  if (g_time_synced) {
    time_t now;
    time(&now);
    return (uint64_t)now * 1000ULL;
  }
  // Fallback FR-HW-025: waktu sejak boot, bukan epoch absolut. Backend
  // diharapkan memakai waktu terima sebagai fallback bila ini tidak masuk
  // akal (drift check) — lihat INTEGRATION_CONTRACT.md §2.3.
  return (uint64_t)millis();
}

bool network_manager_is_time_synced() {
  return g_time_synced;
}
