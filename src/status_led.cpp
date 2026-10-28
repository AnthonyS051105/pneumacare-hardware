#include "status_led.h"

#include <Arduino.h>
#include <config.h>

struct BlinkPattern {
  uint16_t on_ms;
  uint16_t period_ms; // 0 = menyala terus
};

static const BlinkPattern kPatterns[] = {
    {0, 0},       // BOOT
    {250, 500},   // WIFI_CONNECTING
    {500, 1000},  // BACKEND_CONNECTING
    {50, 2000},   // STREAMING
    {62, 125},    // SENSOR_FAULT
};

static StatusLedMode g_mode = LED_MODE_BOOT;
static uint32_t g_mode_start_ms = 0;

void status_led_init() {
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, HIGH);
  g_mode = LED_MODE_BOOT;
  g_mode_start_ms = millis();
}

void status_led_set_mode(StatusLedMode mode) {
  if (mode == g_mode) return;
  g_mode = mode;
  g_mode_start_ms = millis();
}

void status_led_tick() {
  const BlinkPattern &p = kPatterns[g_mode];
  bool on = true;
  if (p.period_ms > 0) {
    on = ((millis() - g_mode_start_ms) % p.period_ms) < p.on_ms;
  }
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
}
