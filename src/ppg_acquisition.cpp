#include "ppg_acquisition.h"

#include <Arduino.h>
#include <Wire.h>
#include <MAX30105.h>

#define PPG_I2C_SDA_PIN 21
#define PPG_I2C_SCL_PIN 22

static MAX30105 ppg_sensor;

bool ppg_acquisition_init() {
  Wire.begin(PPG_I2C_SDA_PIN, PPG_I2C_SCL_PIN);

  // I2C_SPEED_FAST (400kHz) sesuai default library; MAX30102 default I2C address 0x57.
  if (!ppg_sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[FATAL] MAX30102 tidak terdeteksi di bus I2C.");
    return false;
  }

  // Konfigurasi dasar: LED merah+IR aktif (ledMode=2), parameter lain default
  // library. Belum di-tuning — penyesuaian lebih lanjut menyusul setelah PoC
  // baseline ini tervalidasi.
  ppg_sensor.setup(/*powerLevel=*/0x1F, /*sampleAverage=*/4, /*ledMode=*/2,
                    /*sampleRate=*/400, /*pulseWidth=*/411, /*adcRange=*/4096);

  return true;
}

PpgReadResult ppg_acquisition_read() {
  PpgReadResult result = {false, 0, 0};

  ppg_sensor.check();

  if (ppg_sensor.available()) {
    result.has_sample = true;
    result.red = ppg_sensor.getRed();
    result.ir = ppg_sensor.getIR();
    ppg_sensor.nextSample();
  }

  return result;
}
