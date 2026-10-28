#include "battery_monitor.h"

#include <Arduino.h>
#include <config.h>

#include "battery_curve.h"

#if BATTERY_SENSE_ENABLED
static int g_smoothed_percent = -1;
#endif

void battery_monitor_init() {
#if BATTERY_SENSE_ENABLED
  analogReadResolution(12);
  // Atenuasi 11 dB -> rentang ukur ~0.15-2.45 V (cukup untuk 4.2 V / rasio 2.0).
  analogSetPinAttenuation(PIN_BATTERY_SENSE, ADC_11db);
#endif
}

uint32_t battery_monitor_read_mv() {
#if BATTERY_SENSE_ENABLED
  uint32_t sum_mv = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    // analogReadMilliVolts memakai kalibrasi eFuse ADC ESP32 (lebih akurat dari analogRead mentah).
    sum_mv += analogReadMilliVolts(PIN_BATTERY_SENSE);
  }
  return (uint32_t)((float)(sum_mv / BATTERY_ADC_SAMPLES) * BATTERY_DIVIDER_RATIO);
#else
  return 0;
#endif
}

int battery_monitor_read_percent() {
#if BATTERY_SENSE_ENABLED
  int percent = battery_percent_from_mv(battery_monitor_read_mv());
  // Low-pass sederhana: tegangan turun-naik mengikuti beban WiFi, jangan
  // sampai angka di dashboard melompat-lompat tiap heartbeat.
  if (g_smoothed_percent < 0) {
    g_smoothed_percent = percent;
  } else {
    g_smoothed_percent = (g_smoothed_percent * 3 + percent + 2) / 4;
  }
  return g_smoothed_percent;
#else
  return -1;
#endif
}
