// DIAGNOSTIK: LED status (env diag_led).
//
// Menyalakan LED terus 3 detik, mati 3 detik, lalu memutar semua pola status
// (masing-masing 6 detik) supaya tim tahu arti tiap pola di vest fisik.

#include <Arduino.h>
#include <config.h>

#include "status_led.h"

#define MODE_DURATION_MS 6000

struct ModeInfo {
  StatusLedMode mode;
  const char *description;
};

static const ModeInfo kModes[] = {
    {LED_MODE_BOOT, "BOOT — menyala terus (baru dinyalakan)"},
    {LED_MODE_WIFI_CONNECTING, "WIFI_CONNECTING — kedip cepat 2 Hz"},
    {LED_MODE_BACKEND_CONNECTING, "BACKEND_CONNECTING — kedip 1 Hz (WiFi OK, server belum)"},
    {LED_MODE_STREAMING, "STREAMING — kedip singkat tiap 2 detik (normal)"},
    {LED_MODE_SENSOR_FAULT, "SENSOR_FAULT — kedip sangat cepat (mic/PPG bermasalah)"},
};

static size_t g_index = 0;
static uint32_t g_mode_start_ms = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== DIAG: LED status (GPIO %d) ===\n", PIN_STATUS_LED);

  pinMode(PIN_STATUS_LED, OUTPUT);
  Serial.println("[led] Test 1: LED harus MENYALA 3 detik...");
  digitalWrite(PIN_STATUS_LED, HIGH);
  delay(3000);
  Serial.println("[led] Test 2: LED harus MATI 3 detik...");
  digitalWrite(PIN_STATUS_LED, LOW);
  delay(3000);
  Serial.println("[led] Bila LED tidak berubah: cek pin/polaritas LED, atau board tidak punya "
                 "LED di GPIO ini.");

  status_led_init();
  status_led_set_mode(kModes[0].mode);
  Serial.printf("[led] Pola: %s\n", kModes[0].description);
  g_mode_start_ms = millis();
}

void loop() {
  if (millis() - g_mode_start_ms >= MODE_DURATION_MS) {
    g_index = (g_index + 1) % (sizeof(kModes) / sizeof(kModes[0]));
    status_led_set_mode(kModes[g_index].mode);
    Serial.printf("[led] Pola: %s\n", kModes[g_index].description);
    g_mode_start_ms = millis();
  }
  status_led_tick();
  delay(5);
}
