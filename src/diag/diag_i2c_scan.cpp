// DIAGNOSTIK: scan bus I2C (env diag_i2c_scan).
//
// Langkah pertama bila MAX30102 tidak terdeteksi: memastikan ada perangkat yang
// menjawab di bus I2C sama sekali. Tidak butuh WiFi/backend.
//
// Hasil yang diharapkan: perangkat di alamat 0x57 (MAX30102).

#include <Arduino.h>
#include <Wire.h>
#include <config.h>

#define SCAN_INTERVAL_MS 5000

static void scan_bus() {
  Serial.printf("\n[i2c] Scan bus SDA=%d SCL=%d @ %d Hz...\n", PIN_I2C_SDA, PIN_I2C_SCL,
                PPG_I2C_CLOCK_HZ);
  int found = 0;
  bool ppg_found = false;

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      found++;
      const char *name = "";
      if (addr == PPG_I2C_ADDRESS) {
        name = " <- MAX30102 (sensor PPG)";
        ppg_found = true;
      }
      Serial.printf("[i2c]   0x%02X%s\n", addr, name);
    } else if (err == 4) {
      Serial.printf("[i2c]   0x%02X error tak dikenal\n", addr);
    }
  }

  if (found == 0) {
    Serial.println("[i2c] FAIL: tidak ada perangkat sama sekali. Cek: kabel SDA/SCL tertukar?"
                   " VIN 3.3V & GND tersambung? Pull-up SDA/SCL ada (modul MAX30102 umumnya"
                   " sudah punya)?");
  } else if (!ppg_found) {
    Serial.printf("[i2c] FAIL: %d perangkat ditemukan, tapi 0x%02X (MAX30102) tidak ada.\n", found,
                  PPG_I2C_ADDRESS);
  } else {
    Serial.printf("[i2c] PASS: MAX30102 terdeteksi di 0x%02X (%d perangkat total).\n",
                  PPG_I2C_ADDRESS, found);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== DIAG: I2C scanner ===");
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, PPG_I2C_CLOCK_HZ);
}

void loop() {
  scan_bus();
  delay(SCAN_INTERVAL_MS);
}
