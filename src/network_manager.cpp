#include "network_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>
#include <config.h>
#include <secrets.h>

#include "reconnect_manager.h"

static ReconnectBackoff g_wifi_backoff;
static bool g_was_connected = false;
static bool g_time_sync_logged = false;
static uint32_t g_disconnected_since_ms = 0;

void network_manager_init() {
  reconnect_backoff_init(&g_wifi_backoff, WIFI_RECONNECT_BACKOFF_INITIAL_MS,
                         RECONNECT_BACKOFF_MAX_MS);
  g_wifi_backoff.last_attempt_ms = millis(); // WiFi.begin() di bawah = attempt pertama

  WiFi.persistent(false); // jangan tulis ulang kredensial ke flash tiap boot
  WiFi.setHostname(DEVICE_ID);
  WiFi.mode(WIFI_STA);
#if WIFI_DISABLE_POWER_SAVE
  WiFi.setSleep(false);
#endif
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // SNTP mulai sekarang dan otomatis mencoba lagi begitu WiFi tersambung.
  configTime(0, 0, NTP_SERVER, NTP_SERVER_2);

  g_disconnected_since_ms = millis();
  Serial.printf("[network] Menghubungkan ke WiFi SSID \"%s\"...\n", WIFI_SSID);
}

bool network_manager_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}

void network_manager_tick() {
  bool connected = network_manager_is_connected();
  uint32_t now = millis();

  if (connected && !g_was_connected) {
    Serial.printf("[network] WiFi terhubung. IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    reconnect_backoff_reset(&g_wifi_backoff);
  } else if (!connected && g_was_connected) {
    Serial.println("[network] WiFi terputus.");
    g_disconnected_since_ms = now;
    // Percobaan pertama setelah putus menunggu delay awal backoff, memberi
    // kesempatan auto-reconnect bawaan ESP32 pulih lebih dulu.
    g_wifi_backoff.last_attempt_ms = now;
  }
  g_was_connected = connected;

  if (!g_time_sync_logged && network_manager_is_time_synced()) {
    g_time_sync_logged = true;
    Serial.println("[network] NTP tersinkron.");
  }

  if (connected) return;

  if (!reconnect_backoff_should_attempt(&g_wifi_backoff, now)) return;
  reconnect_backoff_notify_attempt(&g_wifi_backoff, now);
  Serial.printf("[network] Mencoba reconnect WiFi (backoff berikutnya %lums)...\n",
                (unsigned long)g_wifi_backoff.current_delay_ms);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

uint32_t network_manager_disconnected_duration_ms() {
  if (network_manager_is_connected()) return 0;
  return millis() - g_disconnected_since_ms;
}

int network_manager_rssi() {
  return network_manager_is_connected() ? WiFi.RSSI() : 0;
}

bool network_manager_is_time_synced() {
  return time(NULL) > (time_t)NTP_VALID_EPOCH_S;
}

uint64_t network_manager_get_epoch_ms() {
  if (network_manager_is_time_synced()) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
  }
  return (uint64_t)millis();
}
