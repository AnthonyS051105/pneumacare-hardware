#include "audio_acquisition.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
static_assert(NUM_AUDIO_CHANNELS >= 1 && NUM_AUDIO_CHANNELS <= 4,
              "NUM_AUDIO_CHANNELS harus 1..4");

#define I2S_SLOTS_PER_FRAME 2
#define I2S_EVENT_QUEUE_LEN 8

static const i2s_port_t kPorts[2] = {I2S_NUM_0, I2S_NUM_1};
static const int kClockPins[2] = {PIN_I2S_SCK_1, PIN_I2S_SCK_2};
static const int kWordSelectPins[2] = {PIN_I2S_WS_1, PIN_I2S_WS_2};
static const int kDataPins[2] = {PIN_I2S_SD_1, PIN_I2S_SD_2};

static int32_t g_dma_read_buf[2][AUDIO_I2S_FRAMES_PER_READ * I2S_SLOTS_PER_FRAME];
static QueueHandle_t g_event_queue[2] = {NULL, NULL};
static bool g_installed[2] = {false, false};
static uint8_t g_units_used = 0;
static uint32_t g_read_errors = 0;
static uint32_t g_dma_overflows = 0;

static uint8_t compute_units_needed() {
  uint8_t units = 1;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    if (AUDIO_CHANNEL_SOURCES[ch].i2s_unit == 1) units = 2;
  }
  return units;
}

static i2s_config_t make_config() {
  i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = AUDIO_SAMPLE_RATE_HZ,
      // INMP441 butuh 64 SCK per frame stereo -> slot 32-bit, data 24-bit MSB.
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = AUDIO_I2S_DMA_BUF_COUNT,
      .dma_buf_len = AUDIO_I2S_DMA_BUF_LEN,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
  };
  return cfg;
}

static bool install_unit(uint8_t unit) {
  i2s_config_t cfg = make_config();
  esp_err_t err = i2s_driver_install(kPorts[unit], &cfg, I2S_EVENT_QUEUE_LEN, &g_event_queue[unit]);
  if (err != ESP_OK) {
    Serial.printf("[audio] i2s_driver_install unit %u gagal: %d\n", unit, err);
    return false;
  }
  g_installed[unit] = true;
  return true;
}

void audio_acquisition_deinit() {
  for (uint8_t unit = 0; unit < 2; unit++) {
    if (g_installed[unit]) {
      i2s_driver_uninstall(kPorts[unit]);
      g_installed[unit] = false;
      g_event_queue[unit] = NULL;
    }
  }
  g_units_used = 0;
}

static bool set_unit_pins(uint8_t unit) {
  i2s_pin_config_t pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = kClockPins[unit],
      .ws_io_num = kWordSelectPins[unit],
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = kDataPins[unit],
  };
  esp_err_t err = i2s_set_pin(kPorts[unit], &pins);
  if (err != ESP_OK) {
    Serial.printf("[audio] i2s_set_pin unit %u gagal: %d\n", unit, err);
    return false;
  }
  // INMP441 men-tri-state SD di luar slotnya; pull-down membuat mic yang lepas
  // terbaca 0 (terdeteksi channel_health sebagai mati), bukan noise floating.
  gpio_pulldown_en((gpio_num_t)kDataPins[unit]);
  return true;
}

// Membaca satu blok mentah dari satu unit. Mengembalikan jumlah frame.
static size_t read_unit(uint8_t unit) {
  size_t bytes_read = 0;
  esp_err_t err = i2s_read(kPorts[unit], g_dma_read_buf[unit], sizeof(g_dma_read_buf[unit]),
                           &bytes_read, pdMS_TO_TICKS(AUDIO_I2S_READ_TIMEOUT_MS));
  if (err != ESP_OK || bytes_read < sizeof(g_dma_read_buf[unit])) {
    g_read_errors++;
  }
  return bytes_read / (sizeof(int32_t) * I2S_SLOTS_PER_FRAME);
}

static void drain_events(uint8_t unit) {
  if (g_event_queue[unit] == NULL) return;
  i2s_event_t event;
  while (xQueueReceive(g_event_queue[unit], &event, 0) == pdTRUE) {
    if (event.type == I2S_EVENT_RX_Q_OVF) g_dma_overflows++;
  }
}

bool audio_acquisition_init() {
  audio_acquisition_deinit();
  g_units_used = compute_units_needed();
  bool dual = (g_units_used == 2);

  // Wiring Alfito: setiap bus punya pin SCK/WS sendiri dan harus menjadi master.
  // Hentikan kedua unit sebelum start berurutan; ini mengurangi offset awal,
  // tetapi TIDAK menjamin kesamaan fase antarbus (ukur di phase1_acquisition).
  for (uint8_t unit = 0; unit < g_units_used; unit++) {
    if (!install_unit(unit)) {
      audio_acquisition_deinit();
      return false;
    }
    if (i2s_stop(kPorts[unit]) != ESP_OK || !set_unit_pins(unit)) {
      audio_acquisition_deinit();
      return false;
    }
  }
  for (uint8_t unit = 0; unit < g_units_used; unit++) {
    if (i2s_start(kPorts[unit]) != ESP_OK) {
      audio_acquisition_deinit();
      return false;
    }
  }

  // 5) Buang sample startup INMP441 + validasi clock benar-benar berjalan.
  uint32_t frames_to_discard = (uint32_t)AUDIO_SAMPLE_RATE_HZ * AUDIO_STARTUP_DISCARD_MS / 1000;
  uint32_t discarded = 0;
  while (discarded < frames_to_discard) {
    size_t frames = read_unit(0);
    if (dual) {
      size_t frames_second = read_unit(1);
      if (frames_second < frames) frames = frames_second;
    }
    if (frames == 0) {
      Serial.println("[audio] Timeout membaca I2S saat startup — clock/wiring SCK/WS bermasalah.");
      return false;
    }
    discarded += frames;
  }
  for (uint8_t unit = 0; unit < g_units_used; unit++) drain_events(unit);
  g_read_errors = 0;
  g_dma_overflows = 0;

  Serial.printf("[audio] I2S siap: %u channel, %u unit (%s), %u Hz.\n", NUM_AUDIO_CHANNELS,
                g_units_used, dual ? "dua master, clock terpisah" : "master", AUDIO_SAMPLE_RATE_HZ);
  return true;
}

size_t audio_acquisition_read(int32_t out[NUM_AUDIO_CHANNELS][AUDIO_I2S_FRAMES_PER_READ]) {
  size_t frames = read_unit(0);
  if (g_units_used == 2) {
    size_t frames_second = read_unit(1);
    if (frames_second < frames) frames = frames_second;
  }

  for (uint8_t unit = 0; unit < g_units_used; unit++) drain_events(unit);

  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    const AudioChannelSource &src = AUDIO_CHANNEL_SOURCES[ch];
    const int32_t *raw = g_dma_read_buf[src.i2s_unit];
    for (size_t i = 0; i < frames; i++) {
      // 24-bit signifikan di MSB slot 32-bit; shift aritmetika menjaga tanda.
      out[ch][i] = raw[i * I2S_SLOTS_PER_FRAME + src.slot] >> 8;
    }
  }
  return frames;
}

uint8_t audio_acquisition_units_used() {
  return g_units_used;
}

uint32_t audio_acquisition_get_read_errors() {
  return g_read_errors;
}

uint32_t audio_acquisition_get_dma_overflows() {
  return g_dma_overflows;
}
