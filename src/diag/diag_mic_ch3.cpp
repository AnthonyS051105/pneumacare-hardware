// INMP441 channel 3 saja, tanpa WiFi/backend atau inisialisasi I2S0.
// Upload: pio run -e diag_mic_ch3 -t upload
// Monitor: pio device monitor -b 115200
// VDD=3V3, GND=GND, SCK=14, WS=27, SD=32, L/R=GND.
// Slot mengikuti AUDIO_CHANNEL_SOURCES[2] agar sama dengan firmware produksi.

#include <Arduino.h>
#include <config.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <math.h>

#include "audio_pcm.h"
#include "noise_filter.h"

static constexpr uint8_t kChannelIndex = 2;
static constexpr i2s_port_t kPort = I2S_NUM_1;
static constexpr uint32_t kSummaryMs = 5000;
static constexpr uint32_t kMeterMs = 250;
static constexpr double kFullScale = 8388608.0;
static_assert(NUM_AUDIO_CHANNELS > kChannelIndex, "Channel 3 harus tersedia");

struct Stats {
  uint32_t count = 0;
  int64_t sum = 0;
  int32_t minimum = INT32_MAX;
  int32_t maximum = INT32_MIN;
  double sum_sq = 0;
  float peak = 0;
  uint32_t clipped = 0;
};

static int32_t g_dma[AUDIO_I2S_FRAMES_PER_READ * 2];
static Stats g_stats;
static QueueHandle_t g_events = nullptr;
static bool g_installed = false;
static uint32_t g_started_ms = 0, g_meter_ms = 0, g_retry_ms = 0;
static uint32_t g_read_errors = 0, g_overflows = 0;
static double g_meter_sq = 0;
static uint32_t g_meter_count = 0;

static float dbfs(double amplitude) {
  return amplitude > 0 ? 20.0 * log10(amplitude / kFullScale) : -140.0f;
}

static void stop_mic() {
  if (g_installed) i2s_driver_uninstall(kPort);
  g_installed = false;
  g_events = nullptr;
}

static bool start_mic() {
  stop_mic();
  if (AUDIO_CHANNEL_SOURCES[kChannelIndex].i2s_unit != 1 ||
      AUDIO_CHANNEL_SOURCES[kChannelIndex].slot > 1) {
    Serial.println("[ch3] FAIL: pemetaan channel 3 harus memakai I2S1 slot 0/1.");
    return false;
  }
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = AUDIO_SAMPLE_RATE_HZ;
  // Dua slot 32-bit menjaga 64 clock/frame; hanya slot channel 3 dianalisis.
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = AUDIO_I2S_DMA_BUF_COUNT;
  cfg.dma_buf_len = AUDIO_I2S_DMA_BUF_LEN;
  esp_err_t err = i2s_driver_install(kPort, &cfg, 8, &g_events);
  if (err != ESP_OK) {
    Serial.printf("[ch3] FAIL install I2S1: %d\n", err);
    return false;
  }
  g_installed = true;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_I2S_SCK_2;
  pins.ws_io_num = PIN_I2S_WS_2;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_I2S_SD_2;
  err = i2s_set_pin(kPort, &pins);
  if (err != ESP_OK) {
    Serial.printf("[ch3] FAIL konfigurasi pin: %d\n", err);
    stop_mic();
    return false;
  }
  // SD tri-state ketika mic tidak mengirim; hindari input mengambang.
  gpio_pulldown_en((gpio_num_t)PIN_I2S_SD_2);
  uint32_t discarded = 0;
  while (discarded < AUDIO_SAMPLE_RATE_HZ * AUDIO_STARTUP_DISCARD_MS / 1000) {
    size_t bytes = 0;
    err = i2s_read(kPort, g_dma, sizeof(g_dma), &bytes,
                   pdMS_TO_TICKS(AUDIO_I2S_READ_TIMEOUT_MS));
    if (err != ESP_OK || bytes < 2 * sizeof(int32_t)) {
      Serial.println("[ch3] FAIL: pembacaan I2S1 timeout saat startup.");
      stop_mic();
      return false;
    }
    discarded += bytes / (2 * sizeof(int32_t));
  }
  i2s_event_t event;
  while (xQueueReceive(g_events, &event, 0) == pdTRUE) {}
  noise_filter_init(AUDIO_SAMPLE_RATE_HZ, AUDIO_HPF_CUTOFF_HZ, AUDIO_LPF_CUTOFF_HZ);
  g_stats = Stats{};
  g_meter_sq = 0;
  g_meter_count = g_read_errors = g_overflows = 0;
  g_started_ms = g_meter_ms = millis();
  Serial.println("[ch3] Siap. Bicara/tepuk tangan di dekat mic #3; level harus naik.");
  return true;
}

static void print_summary(uint32_t now) {
  float rate = g_stats.count * 1000.0f / (now - g_started_ms);
  int32_t p2p = g_stats.count ? g_stats.maximum - g_stats.minimum : 0;
  float rms = dbfs(g_stats.count ? sqrt(g_stats.sum_sq / g_stats.count) : 0);
  float clip = g_stats.count ? 100.0f * g_stats.clipped / g_stats.count : 0;
  const char *verdict = "PASS sinyal bervariasi (verifikasi respons suara)";
  if (!g_stats.count || p2p < AUDIO_DEAD_CHANNEL_P2P_THRESHOLD)
    verdict = "FAIL data kosong/konstan: cek VDD/GND/SD dan L/R=GND serta pemetaan slot";
  else if (g_read_errors || g_overflows || fabsf(rate - AUDIO_SAMPLE_RATE_HZ) > AUDIO_SAMPLE_RATE_HZ * 0.01f)
    verdict = "WARN akuisisi: laju tidak sesuai, read error, atau overflow DMA";
  else if (clip > 0.5f)
    verdict = "WARN clipping: kurangi suara/gain (naikkan AUDIO_PCM_GAIN_SHIFT)";
  else if (rms < -100.0f)
    verdict = "WARN sinyal sangat lemah: coba bicara dekat mic";
  Serial.printf("[ch3] %.1f Hz (target %d) | rms=%.1f dBFS peak=%.1f dBFS "
                "dc=%.0f p2p=%ld clip=%.2f%% | read_err=%lu dma_ovf=%lu\n",
                rate, AUDIO_SAMPLE_RATE_HZ, rms, dbfs(g_stats.peak),
                g_stats.count ? (double)g_stats.sum / g_stats.count : 0,
                (long)p2p, clip, (unsigned long)g_read_errors, (unsigned long)g_overflows);
  Serial.printf("[ch3] %s\n", verdict);
  g_stats = Stats{};
  g_read_errors = g_overflows = 0;
  g_started_ms = now;
}

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== INMP441 CHANNEL 3 SAJA ===\nI2S1 SCK=%d WS=%d SD=%d | slot=%u | L/R=GND\n",
                PIN_I2S_SCK_2, PIN_I2S_WS_2, PIN_I2S_SD_2,
                AUDIO_CHANNEL_SOURCES[kChannelIndex].slot);
  start_mic();
  g_retry_ms = millis();
}

void loop() {
  if (!g_installed) {
    if (millis() - g_retry_ms >= 5000) {
      start_mic();
      g_retry_ms = millis();
    }
    delay(10);
    return;
  }
  size_t bytes = 0;
  esp_err_t err = i2s_read(kPort, g_dma, sizeof(g_dma), &bytes,
                          pdMS_TO_TICKS(AUDIO_I2S_READ_TIMEOUT_MS));
  if (err != ESP_OK || bytes != sizeof(g_dma)) g_read_errors++;
  size_t frames = err == ESP_OK ? bytes / (2 * sizeof(int32_t)) : 0;
  i2s_event_t event;
  while (xQueueReceive(g_events, &event, 0) == pdTRUE) {
    if (event.type == I2S_EVENT_RX_Q_OVF) g_overflows++;
  }
  for (size_t i = 0; i < frames; i++) {
    int32_t raw = g_dma[2 * i + AUDIO_CHANNEL_SOURCES[kChannelIndex].slot] >> 8;
    float filtered = noise_filter_apply(kChannelIndex, (float)raw);
    bool clipped = false;
    audio_pcm_from_sample24(filtered, AUDIO_PCM_GAIN_SHIFT, &clipped);
    g_stats.count++;
    g_stats.sum += raw;
    if (raw < g_stats.minimum) g_stats.minimum = raw;
    if (raw > g_stats.maximum) g_stats.maximum = raw;
    g_stats.sum_sq += (double)filtered * filtered;
    if (fabsf(filtered) > g_stats.peak) g_stats.peak = fabsf(filtered);
    if (clipped) g_stats.clipped++;
    g_meter_sq += (double)filtered * filtered;
    g_meter_count++;
  }
  uint32_t now = millis();
  if (now - g_meter_ms >= kMeterMs) {
    float level = dbfs(g_meter_count ? sqrt(g_meter_sq / g_meter_count) : 0);
    int bars = constrain((int)((level + 90) * 20 / 70), 0, 20);
    char meter[21];
    for (int i = 0; i < 20; i++) meter[i] = i < bars ? '#' : '.';
    meter[20] = '\0';
    Serial.printf("ch3 %6.1f dBFS |%s|\n", level, meter);
    g_meter_sq = 0;
    g_meter_count = 0;
    g_meter_ms = now;
  }
  if (now - g_started_ms >= kSummaryMs) print_summary(now);
}
