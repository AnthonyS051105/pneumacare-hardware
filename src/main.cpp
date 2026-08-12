#include <Arduino.h>
#include <driver/i2s.h>

// =====================================================================
// PoC Fase 1 — Akuisisi I2S 1-channel dari SATU mikrofon INMP441.
//
// Tujuan: validasi paling dasar bahwa ESP32 bisa membaca data digital dari
// 1 mikrofon I2S. TIDAK ada streaming jaringan di sini (itu Fase 3) dan
// TIDAK ada logika multi-channel (itu langkah PoC berikutnya, lihat
// SDD_HARDWARE.md §5 dan ARCHITECTURE_HARDWARE.md §4 soal risiko teknis
// 4-channel I2S sinkron).
//
// Pin dikonfirmasi manual oleh Tony (2026-08-12) untuk mic #1 — BUKAN
// nilai final PIN_MAPPING_BOM.md §1.1 (masih placeholder TODO di sana).
// Update PIN_MAPPING_BOM.md begitu Alfito konfirmasi skematik final.
// =====================================================================

#define I2S_MIC_SCK_PIN 32   // bit clock (BCK)
#define I2S_MIC_WS_PIN 25    // word select (LRCLK)
#define I2S_MIC_SD_PIN 33    // data in dari mikrofon

#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE 16000        // ⚠️ belum final, lihat SRS_HARDWARE.md FR-HW-003
#define I2S_READ_BUF_LEN 512         // jumlah sample int32 per pembacaan
#define SERIAL_PRINT_INTERVAL_MS 500 // supaya serial monitor tidak banjir

static int32_t i2s_read_buf[I2S_READ_BUF_LEN];

static void i2s_mic_init() {
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 64,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
  };

  i2s_pin_config_t pin_config = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = I2S_MIC_SCK_PIN,
      .ws_io_num = I2S_MIC_WS_PIN,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_MIC_SD_PIN,
  };

  esp_err_t err;

  err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[FATAL] i2s_driver_install gagal: %d\n", err);
    while (true) delay(1000);
  }

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[FATAL] i2s_set_pin gagal: %d\n", err);
    while (true) delay(1000);
  }

  i2s_zero_dma_buffer(I2S_PORT);
}

void setup() {
  Serial.begin(115200);
  delay(1000); // beri waktu serial monitor connect setelah reset

  Serial.println();
  Serial.println("=== PoC I2S 1-channel INMP441 ===");
  Serial.printf("Pin: SCK=%d WS=%d SD=%d, sample_rate=%d Hz\n",
                I2S_MIC_SCK_PIN, I2S_MIC_WS_PIN, I2S_MIC_SD_PIN, I2S_SAMPLE_RATE);

  i2s_mic_init();

  Serial.println("I2S driver terpasang. Mulai baca data...");
}

void loop() {
  static uint32_t last_print_ms = 0;

  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_PORT, i2s_read_buf, sizeof(i2s_read_buf),
                            &bytes_read, portMAX_DELAY);

  if (err != ESP_OK) {
    Serial.printf("[ERROR] i2s_read gagal: %d\n", err);
    return;
  }

  size_t samples_read = bytes_read / sizeof(int32_t);

  // INMP441 output 24-bit signifikan di MSB dari word 32-bit -> geser kanan 8.
  int32_t min_val = INT32_MAX;
  int32_t max_val = INT32_MIN;
  int64_t sum_abs = 0;

  for (size_t i = 0; i < samples_read; i++) {
    int32_t sample = i2s_read_buf[i] >> 8;
    if (sample < min_val) min_val = sample;
    if (sample > max_val) max_val = sample;
    sum_abs += abs(sample);
  }

  uint32_t now = millis();
  if (samples_read > 0 && now - last_print_ms >= SERIAL_PRINT_INTERVAL_MS) {
    last_print_ms = now;
    int32_t avg_abs = sum_abs / samples_read;
    Serial.printf("samples=%u min=%ld max=%ld avg_abs=%ld\n",
                  (unsigned)samples_read, (long)min_val, (long)max_val, (long)avg_abs);
  }
}
