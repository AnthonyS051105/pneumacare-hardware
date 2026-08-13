#include <Arduino.h>

#include "ppg_acquisition.h"

// =====================================================================
// PoC Fase 1 — Akuisisi I2C dari sensor PPG MAX30102.
//
// Tujuan: validasi paling dasar bahwa ESP32 bisa membaca raw sample (Red +
// IR) dari MAX30102 via I2C. TIDAK ada filtering/estimasi HR/SpO2 di sini
// (itu backend, lihat PRD_HARDWARE.md §1) dan TIDAK ada streaming jaringan
// (itu Fase 3).
//
// Sesuai SDD_HARDWARE.md §8: I2S (audio) dan I2C (PPG) diuji TERPISAH dulu
// sebelum digabung — PoC I2S sebelumnya (audio_acquisition) untuk sementara
// tidak dipanggil dari main.cpp ini, menunggu wiring mikrofon disolder ulang.
// =====================================================================

#define SERIAL_PRINT_INTERVAL_MS 200 // supaya serial monitor tidak banjir

void setup() {
  Serial.begin(115200);
  delay(1000); // beri waktu serial monitor connect setelah reset

  Serial.println();
  Serial.println("=== PoC I2C sensor PPG MAX30102 ===");

  if (!ppg_acquisition_init()) {
    Serial.println("[FATAL] Inisialisasi PPG gagal, cek wiring I2C (SDA/SCL/VDD/GND).");
    while (true) delay(1000);
  }

  Serial.println("Sensor PPG terdeteksi. Mulai baca data...");
}

void loop() {
  static uint32_t last_print_ms = 0;

  PpgReadResult r = ppg_acquisition_read();

  uint32_t now = millis();
  if (r.has_sample && now - last_print_ms >= SERIAL_PRINT_INTERVAL_MS) {
    last_print_ms = now;
    Serial.printf("red=%lu ir=%lu\n", (unsigned long)r.red, (unsigned long)r.ir);
  }
}
