#include "ppg_acquisition.h"

#include <Arduino.h>
#include <Wire.h>
#include <config.h>
#include <math.h>

// Register map MAX30102 (datasheet Rev 1, Table 1).
#define REG_INT_STATUS_1 0x00
#define REG_INT_ENABLE_1 0x02
#define REG_INT_ENABLE_2 0x03
#define REG_FIFO_WR_PTR 0x04
#define REG_OVF_COUNTER 0x05
#define REG_FIFO_RD_PTR 0x06
#define REG_FIFO_DATA 0x07
#define REG_FIFO_CONFIG 0x08
#define REG_MODE_CONFIG 0x09
#define REG_SPO2_CONFIG 0x0A
#define REG_LED1_PA 0x0C // Red
#define REG_LED2_PA 0x0D // IR
#define REG_TEMP_INT 0x1F
#define REG_TEMP_FRAC 0x20
#define REG_TEMP_CONFIG 0x21
#define REG_REV_ID 0xFE
#define REG_PART_ID 0xFF

#define MAX30102_PART_ID 0x15
#define MODE_RESET 0x40
#define MODE_SPO2 0x03 // Red + IR
#define FIFO_ROLLOVER_EN 0x10
#define FIFO_A_FULL_17_FREE 0x0F
#define SAMPLE_BYTES 6 // 3 byte Red + 3 byte IR
#define SAMPLE_MASK 0x3FFFF

// Buffer Wire ESP32 = 128 byte -> maks 21 sample (126 byte) per transaksi.
#define MAX_SAMPLES_PER_I2C_READ 21

// Konversi nilai fisik config.h -> kode register; static_assert menolak nilai
// yang tidak didukung saat compile, bukan diam-diam salah konfigurasi.
constexpr uint8_t smp_ave_code(int n) {
  return n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : n == 16 ? 4 : n == 32 ? 5 : 0xFF;
}
constexpr uint8_t sample_rate_code(int sps) {
  return sps == 50 ? 0 : sps == 100 ? 1 : sps == 200 ? 2 : sps == 400 ? 3 : sps == 800 ? 4
       : sps == 1000 ? 5 : sps == 1600 ? 6 : sps == 3200 ? 7 : 0xFF;
}
constexpr uint8_t pulse_width_code(int us) {
  return us == 69 ? 0 : us == 118 ? 1 : us == 215 ? 2 : us == 411 ? 3 : 0xFF;
}
constexpr uint8_t adc_range_code(int na) {
  return na == 2048 ? 0 : na == 4096 ? 1 : na == 8192 ? 2 : na == 16384 ? 3 : 0xFF;
}

static_assert(smp_ave_code(PPG_SAMPLE_AVERAGING) != 0xFF, "PPG_SAMPLE_AVERAGING tidak valid");
static_assert(sample_rate_code(PPG_INTERNAL_SAMPLE_RATE_SPS) != 0xFF,
              "PPG_INTERNAL_SAMPLE_RATE_SPS tidak valid");
static_assert(pulse_width_code(PPG_PULSE_WIDTH_US) != 0xFF, "PPG_PULSE_WIDTH_US tidak valid");
static_assert(adc_range_code(PPG_ADC_RANGE_NA) != 0xFF, "PPG_ADC_RANGE_NA tidak valid");
static_assert(PPG_INTERNAL_SAMPLE_RATE_SPS % PPG_SAMPLE_AVERAGING == 0,
              "Sample rate harus habis dibagi averaging");

static bool g_wire_started = false;
static bool g_ready = false;
static uint8_t g_consecutive_errors = 0;
static uint32_t g_last_init_attempt_ms = 0;
static uint32_t g_samples_read = 0;
static uint32_t g_fifo_overflows = 0;
static uint32_t g_i2c_errors = 0;

static void note_i2c_error() {
  g_i2c_errors++;
  if (++g_consecutive_errors >= PPG_MAX_CONSECUTIVE_I2C_ERRORS && g_ready) {
    g_ready = false;
    Serial.println("[ppg] Sensor tidak merespons (error I2C berulang) — akan dicoba init ulang.");
  }
}

bool ppg_write_register(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(PPG_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() != 0) {
    note_i2c_error();
    return false;
  }
  return true;
}

static bool read_registers(uint8_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(PPG_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    note_i2c_error();
    return false;
  }
  size_t got = Wire.requestFrom((uint16_t)PPG_I2C_ADDRESS, (uint8_t)len);
  if (got != len) {
    while (Wire.available()) Wire.read();
    note_i2c_error();
    return false;
  }
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

bool ppg_read_register(uint8_t reg, uint8_t *value) {
  return read_registers(reg, value, 1);
}

bool ppg_read_part_info(uint8_t *part_id, uint8_t *rev_id) {
  return ppg_read_register(REG_PART_ID, part_id) && ppg_read_register(REG_REV_ID, rev_id);
}

bool ppg_set_led_currents(uint8_t red, uint8_t ir) {
  return ppg_write_register(REG_LED1_PA, red) && ppg_write_register(REG_LED2_PA, ir);
}

static bool configure_sensor() {
  if (!ppg_write_register(REG_MODE_CONFIG, MODE_RESET)) return false;

  // Bit RESET kembali 0 otomatis setelah reset selesai (< 1 ms tipikal).
  uint32_t start = millis();
  uint8_t mode = MODE_RESET;
  while (mode & MODE_RESET) {
    if (millis() - start > 100) return false;
    delay(1);
    if (!ppg_read_register(REG_MODE_CONFIG, &mode)) return false;
  }

  uint8_t fifo_config =
      (uint8_t)(smp_ave_code(PPG_SAMPLE_AVERAGING) << 5) | FIFO_ROLLOVER_EN | FIFO_A_FULL_17_FREE;
  uint8_t spo2_config = (uint8_t)(adc_range_code(PPG_ADC_RANGE_NA) << 5) |
                        (uint8_t)(sample_rate_code(PPG_INTERNAL_SAMPLE_RATE_SPS) << 2) |
                        pulse_width_code(PPG_PULSE_WIDTH_US);

  uint8_t status;
  return ppg_write_register(REG_INT_ENABLE_1, 0x00) &&      // mode polling
         ppg_write_register(REG_INT_ENABLE_2, 0x00) &&
         ppg_write_register(REG_FIFO_CONFIG, fifo_config) &&
         ppg_write_register(REG_SPO2_CONFIG, spo2_config) &&
         ppg_set_led_currents(PPG_LED_RED_CURRENT, PPG_LED_IR_CURRENT) &&
         ppg_write_register(REG_FIFO_WR_PTR, 0x00) &&
         ppg_write_register(REG_OVF_COUNTER, 0x00) &&
         ppg_write_register(REG_FIFO_RD_PTR, 0x00) &&
         ppg_read_register(REG_INT_STATUS_1, &status) &&    // bersihkan flag power-ready
         ppg_write_register(REG_MODE_CONFIG, MODE_SPO2);
}

bool ppg_acquisition_init() {
  g_last_init_attempt_ms = millis();

  if (!g_wire_started) {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, PPG_I2C_CLOCK_HZ);
    Wire.setTimeOut(PPG_I2C_TIMEOUT_MS);
    g_wire_started = true;
  }

  g_consecutive_errors = 0;
  uint8_t part_id = 0, rev_id = 0;
  if (!ppg_read_part_info(&part_id, &rev_id)) {
    Serial.printf("[ppg] Tidak ada respons di alamat I2C 0x%02X (cek SDA=%d SCL=%d VIN GND).\n",
                  PPG_I2C_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL);
    g_ready = false;
    return false;
  }
  if (part_id != MAX30102_PART_ID) {
    Serial.printf("[ppg] PART_ID=0x%02X, bukan MAX30102 (0x%02X) — cek part number.\n", part_id,
                  MAX30102_PART_ID);
    g_ready = false;
    return false;
  }
  if (!configure_sensor()) {
    Serial.println("[ppg] Konfigurasi register MAX30102 gagal.");
    g_ready = false;
    return false;
  }

  g_consecutive_errors = 0;
  g_ready = true;
  Serial.printf("[ppg] MAX30102 siap (rev 0x%02X): %d Hz, LED red=0x%02X ir=0x%02X.\n", rev_id,
                PPG_SAMPLE_RATE_HZ, PPG_LED_RED_CURRENT, PPG_LED_IR_CURRENT);
  return true;
}

bool ppg_acquisition_is_ready() {
  return g_ready;
}

size_t ppg_acquisition_read(PpgSample *out, size_t max_samples) {
  if (!g_ready) {
    if (millis() - g_last_init_attempt_ms >= PPG_REINIT_INTERVAL_MS) {
      ppg_acquisition_init();
    }
    return 0;
  }

  // WR_PTR, OVF_COUNTER, RD_PTR berurutan -> 1 transaksi.
  uint8_t ptrs[3];
  if (!read_registers(REG_FIFO_WR_PTR, ptrs, sizeof(ptrs))) return 0;
  uint8_t wr = ptrs[0] & 0x1F;
  uint8_t ovf = ptrs[1] & 0x1F;
  uint8_t rd = ptrs[2] & 0x1F;

  size_t available = (uint8_t)(wr - rd) & 0x1F;
  if (available == 0 && ovf > 0) available = PPG_FIFO_DEPTH; // FIFO penuh
  g_fifo_overflows += ovf;
  if (available > max_samples) available = max_samples;

  size_t total = 0;
  while (total < available) {
    size_t batch = available - total;
    if (batch > MAX_SAMPLES_PER_I2C_READ) batch = MAX_SAMPLES_PER_I2C_READ;

    uint8_t raw[MAX_SAMPLES_PER_I2C_READ * SAMPLE_BYTES];
    if (!read_registers(REG_FIFO_DATA, raw, batch * SAMPLE_BYTES)) break;

    for (size_t i = 0; i < batch; i++) {
      const uint8_t *s = &raw[i * SAMPLE_BYTES];
      out[total + i].red = (((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]) & SAMPLE_MASK;
      out[total + i].ir = (((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 8) | s[5]) & SAMPLE_MASK;
    }
    total += batch;
  }

  if (total > 0) g_consecutive_errors = 0;
  g_samples_read += total;
  return total;
}

uint32_t ppg_acquisition_get_samples_read() {
  return g_samples_read;
}

uint32_t ppg_acquisition_get_fifo_overflows() {
  return g_fifo_overflows;
}

uint32_t ppg_acquisition_get_i2c_errors() {
  return g_i2c_errors;
}

float ppg_read_temperature_c() {
  if (!ppg_write_register(REG_TEMP_CONFIG, 0x01)) return NAN;
  uint32_t start = millis();
  uint8_t cfg = 0x01;
  while (cfg & 0x01) {
    if (millis() - start > 100) return NAN;
    delay(5);
    if (!ppg_read_register(REG_TEMP_CONFIG, &cfg)) return NAN;
  }
  uint8_t t_int = 0, t_frac = 0;
  if (!ppg_read_register(REG_TEMP_INT, &t_int) || !ppg_read_register(REG_TEMP_FRAC, &t_frac)) {
    return NAN;
  }
  return (float)(int8_t)t_int + (float)(t_frac & 0x0F) * 0.0625f;
}
