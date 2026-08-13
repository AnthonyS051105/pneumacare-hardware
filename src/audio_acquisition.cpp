#include "audio_acquisition.h"

#include <Arduino.h>
#include <driver/i2s.h>

#define I2S_MIC_SCK_PIN 32   // bit clock (BCK)
#define I2S_MIC_WS_PIN 25    // word select (LRCLK)
#define I2S_MIC_SD_PIN 33    // data in dari mikrofon

#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE 16000 // ⚠️ belum final, lihat SRS_HARDWARE.md FR-HW-003
#define I2S_READ_BUF_LEN 512  // jumlah sample int32 per pembacaan

static int32_t i2s_read_buf[I2S_READ_BUF_LEN];

void audio_acquisition_init() {
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

AudioReadResult audio_acquisition_read() {
  AudioReadResult result = {0, 0, 0, 0};

  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_PORT, i2s_read_buf, sizeof(i2s_read_buf),
                            &bytes_read, portMAX_DELAY);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] i2s_read gagal: %d\n", err);
    return result;
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

  result.samples_read = samples_read;
  result.min_val = (samples_read > 0) ? min_val : 0;
  result.max_val = (samples_read > 0) ? max_val : 0;
  result.avg_abs = (samples_read > 0) ? (int32_t)(sum_abs / samples_read) : 0;
  return result;
}
