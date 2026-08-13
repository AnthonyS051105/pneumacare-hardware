#include <Arduino.h>

#include "audio_acquisition.h"

// =====================================================================
// PoC Fase 1 — Akuisisi I2S 1-channel dari SATU mikrofon INMP441.
//
// Tujuan: validasi paling dasar bahwa ESP32 bisa membaca data digital dari
// 1 mikrofon I2S. TIDAK ada streaming jaringan di sini (itu Fase 3) dan
// TIDAK ada logika multi-channel (itu langkah PoC berikutnya, lihat
// SDD_HARDWARE.md §5 dan ARCHITECTURE_HARDWARE.md §4 soal risiko teknis
// 4-channel I2S sinkron).
// =====================================================================

#define SERIAL_PRINT_INTERVAL_MS 500 // supaya serial monitor tidak banjir

void setup() {
  Serial.begin(115200);
  delay(1000); // beri waktu serial monitor connect setelah reset

  Serial.println();
  Serial.println("=== PoC I2S 1-channel INMP441 ===");

  audio_acquisition_init();

  Serial.println("I2S driver terpasang. Mulai baca data...");
}

void loop() {
  static uint32_t last_print_ms = 0;

  AudioReadResult r = audio_acquisition_read();

  uint32_t now = millis();
  if (r.samples_read > 0 && now - last_print_ms >= SERIAL_PRINT_INTERVAL_MS) {
    last_print_ms = now;
    Serial.printf("samples=%u min=%ld max=%ld avg_abs=%ld\n",
                  (unsigned)r.samples_read, (long)r.min_val, (long)r.max_val,
                  (long)r.avg_abs);
  }
}
