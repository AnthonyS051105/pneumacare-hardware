// DIAGNOSTIK: pembacaan tegangan baterai (env diag_battery).
//
// Membaca ADC di PIN_BATTERY_SENSE walaupun BATTERY_SENSE_ENABLED = 0, untuk
// memvalidasi rangkaian pembagi tegangan SEBELUM fitur diaktifkan di firmware.
// Bandingkan "Vbat" dengan multimeter di terminal baterai: selisih > ~100 mV
// berarti BATTERY_DIVIDER_RATIO di config.h perlu dikoreksi.

#include <Arduino.h>
#include <config.h>

#include "battery_curve.h"

#define READ_INTERVAL_MS 2000

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== DIAG: baterai (ADC GPIO %d, rasio pembagi %.2f) ===\n", PIN_BATTERY_SENSE,
                BATTERY_DIVIDER_RATIO);
  Serial.printf("BATTERY_SENSE_ENABLED di config.h = %d\n", BATTERY_SENSE_ENABLED);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_SENSE, ADC_11db);
}

void loop() {
  uint32_t sum = 0, min_mv = UINT32_MAX, max_mv = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    uint32_t mv = analogReadMilliVolts(PIN_BATTERY_SENSE);
    sum += mv;
    if (mv < min_mv) min_mv = mv;
    if (mv > max_mv) max_mv = mv;
    delay(2);
  }
  uint32_t adc_mv = sum / BATTERY_ADC_SAMPLES;
  uint32_t vbat_mv = (uint32_t)(adc_mv * BATTERY_DIVIDER_RATIO);

  const char *verdict;
  if (adc_mv < 100) {
    verdict = "FAIL — tidak ada tegangan (pembagi belum terpasang / pin salah / baterai lepas)";
  } else if (vbat_mv > 4350) {
    verdict = "WARN — di atas tegangan maks Li-Po 4.2 V: rasio pembagi salah?";
  } else if (vbat_mv < 3000) {
    verdict = "WARN — di bawah 3.0 V: baterai habis atau rasio pembagi salah";
  } else if (max_mv - min_mv > 150) {
    verdict = "WARN — pembacaan sangat berisik, tambahkan kapasitor 100 nF di pin ADC";
  } else {
    verdict = "PASS";
  }

  Serial.printf("[battery] Vadc=%lu mV (min %lu, max %lu) Vbat=%lu mV -> %d%%  %s\n",
                (unsigned long)adc_mv, (unsigned long)min_mv, (unsigned long)max_mv,
                (unsigned long)vbat_mv, battery_percent_from_mv(vbat_mv), verdict);
  delay(READ_INTERVAL_MS);
}
