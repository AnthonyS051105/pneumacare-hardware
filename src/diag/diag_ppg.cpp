// DIAGNOSTIK: sensor PPG MAX30102 (env diag_ppg). Tidak butuh WiFi/backend.
//
// Urutan cek:
//   1. Respons I2C + PART_ID (0x15) + revisi
//   2. Sensor suhu internal (bukti die sensor hidup)
//   3. Laju sample FIFO aktual vs PPG_SAMPLE_RATE_HZ
//   4. LED Red & IR (bandingkan LED mati vs nyala — TEMPELKAN JARI/KULIT)
//   5. Pemantauan kontinu: kontak kulit, saturasi, perfusion index (AC/DC)
//      — indikator kualitas sinyal untuk uji posisi dada (PRD_HARDWARE.md §5)
//
// Perintah serial: 'p' = toggle mode Serial Plotter (red,ir), 'r' = ulangi cek.

#include <Arduino.h>
#include <config.h>
#include <math.h>

#include "ppg_acquisition.h"

#define RATE_TEST_DURATION_MS 5000
#define LED_SETTLE_MS 300
#define LED_TEST_SAMPLES 25
#define LED_MIN_DELTA_COUNTS 1000 // kenaikan minimal saat LED nyala (ada pantulan kulit)
#define SKIN_CONTACT_IR_MIN 50000
#define SATURATION_LEVEL 260000   // mendekati maks 18-bit (262143)
#define MONITOR_WINDOW_MS 5000

static bool g_plot_mode = false;

static void flush_fifo() {
  PpgSample tmp[PPG_FIFO_DEPTH];
  // Sensor tetap menghasilkan data: batasi pengurasan agar cek tidak macet.
  for (size_t i = 0; i < PPG_FIFO_DEPTH; i++) {
    if (ppg_acquisition_read(tmp, PPG_FIFO_DEPTH) == 0) break;
  }
}

static bool mean_of_samples(size_t n, float *red_mean, float *ir_mean) {
  double red_sum = 0, ir_sum = 0;
  size_t got = 0;
  uint32_t start = millis();
  PpgSample buf[PPG_FIFO_DEPTH];
  while (got < n && millis() - start < 2000) {
    size_t k = ppg_acquisition_read(buf, PPG_FIFO_DEPTH);
    for (size_t i = 0; i < k && got < n; i++, got++) {
      red_sum += buf[i].red;
      ir_sum += buf[i].ir;
    }
    delay(5);
  }
  if (got < n) return false;
  *red_mean = red_sum / n;
  *ir_mean = ir_sum / n;
  return true;
}

static void run_checks() {
  Serial.println("\n------------------------------------------------------------------");
  // 1. Identitas
  if (!ppg_acquisition_init()) {
    Serial.println("[ppg] FAIL: sensor tidak terdeteksi/ID salah. Jalankan env diag_i2c_scan.");
    return;
  }
  uint8_t part = 0, rev = 0;
  if (!ppg_read_part_info(&part, &rev)) {
    Serial.println("[ppg] FAIL: pembacaan identitas terputus. Periksa kabel I2C.");
    return;
  }
  Serial.printf("[ppg] PASS identitas: PART_ID=0x%02X REV=0x%02X\n", part, rev);

  // 2. Suhu die
  float temp = ppg_read_temperature_c();
  if (isnan(temp)) {
    Serial.println("[ppg] FAIL sensor suhu tidak merespons.");
  } else {
    Serial.printf("[ppg] %s suhu die = %.2f C\n", (temp > 0 && temp < 70) ? "PASS" : "WARN", temp);
  }

  // 3. Laju sample
  flush_fifo();
  uint32_t start_count = ppg_acquisition_get_samples_read();
  uint32_t start_ovf = ppg_acquisition_get_fifo_overflows();
  uint32_t start_ms = millis();
  PpgSample buf[PPG_FIFO_DEPTH];
  while (millis() - start_ms < RATE_TEST_DURATION_MS) {
    ppg_acquisition_read(buf, PPG_FIFO_DEPTH);
    delay(20);
  }
  float rate = (ppg_acquisition_get_samples_read() - start_count) * 1000.0f / (millis() - start_ms);
  float err = fabsf(rate - PPG_SAMPLE_RATE_HZ) / PPG_SAMPLE_RATE_HZ * 100.0f;
  Serial.printf("[ppg] %s laju sample %.1f Hz (target %d, selisih %.1f%%), fifo_overflow=%lu\n",
                err < 5.0f ? "PASS" : "FAIL", rate, PPG_SAMPLE_RATE_HZ, err,
                (unsigned long)(ppg_acquisition_get_fifo_overflows() - start_ovf));

  // 4. LED
  Serial.println("[ppg] Test LED — tempelkan jari/kulit ke sensor sekarang...");
  delay(2000);
  float dark_red, dark_ir, lit_red, lit_ir;
  bool dark_configured = ppg_set_led_currents(0, 0);
  delay(LED_SETTLE_MS);
  flush_fifo();
  bool ok_dark = dark_configured && mean_of_samples(LED_TEST_SAMPLES, &dark_red, &dark_ir);
  // Selalu coba kembalikan arus normal, termasuk bila pengukuran gelap gagal.
  bool lit_configured = ppg_set_led_currents(PPG_LED_RED_CURRENT, PPG_LED_IR_CURRENT);
  delay(LED_SETTLE_MS);
  flush_fifo();
  bool ok_lit = lit_configured && mean_of_samples(LED_TEST_SAMPLES, &lit_red, &lit_ir);
  if (!ok_dark || !ok_lit) {
    Serial.println("[ppg] FAIL test LED: konfigurasi I2C gagal atau tidak cukup sample terbaca.");
  } else {
    bool red_ok = (lit_red - dark_red) > LED_MIN_DELTA_COUNTS;
    bool ir_ok = (lit_ir - dark_ir) > LED_MIN_DELTA_COUNTS;
    Serial.printf("[ppg] LED red: mati=%.0f nyala=%.0f -> %s\n", dark_red, lit_red,
                  red_ok ? "PASS" : "FAIL (tidak ada pantulan: tidak ada kulit di sensor, atau LED red rusak)");
    Serial.printf("[ppg] LED IR : mati=%.0f nyala=%.0f -> %s\n", dark_ir, lit_ir,
                  ir_ok ? "PASS" : "FAIL (tidak ada pantulan: tidak ada kulit di sensor, atau LED IR rusak)");
  }
  Serial.println("[ppg] Pemantauan kontinu dimulai ('p' = plotter, 'r' = ulangi cek).");
  Serial.println("------------------------------------------------------------------");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== DIAG: PPG MAX30102 (SDA=%d SCL=%d addr 0x%02X) ===\n", PIN_I2C_SDA,
                PIN_I2C_SCL, PPG_I2C_ADDRESS);
  Serial.println("Tempelkan jari dengan stabil pada sensor untuk uji LED dan gelombang.");
  Serial.println("Data raw untuk uji hardware; program ini tidak menghitung BPM/SpO2.");
  run_checks();
}

void loop() {
  static uint32_t window_start = millis();
  static uint32_t n = 0, red_min = UINT32_MAX, red_max = 0, ir_min = UINT32_MAX, ir_max = 0;
  static double red_sum = 0, ir_sum = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'p') g_plot_mode = !g_plot_mode;
    if (c == 'r') {
      g_plot_mode = false;
      run_checks();
      // Sampel sebelum cek ulang tidak boleh tercampur ke jendela berikutnya.
      window_start = millis();
      n = 0;
      red_sum = ir_sum = 0;
      red_min = ir_min = UINT32_MAX;
      red_max = ir_max = 0;
    }
  }

  PpgSample buf[PPG_FIFO_DEPTH];
  size_t k = ppg_acquisition_read(buf, PPG_FIFO_DEPTH);
  for (size_t i = 0; i < k; i++) {
    if (g_plot_mode) Serial.printf("red:%lu,ir:%lu\n", (unsigned long)buf[i].red, (unsigned long)buf[i].ir);
    red_sum += buf[i].red;
    ir_sum += buf[i].ir;
    if (buf[i].red < red_min) red_min = buf[i].red;
    if (buf[i].red > red_max) red_max = buf[i].red;
    if (buf[i].ir < ir_min) ir_min = buf[i].ir;
    if (buf[i].ir > ir_max) ir_max = buf[i].ir;
    n++;
  }

  if (millis() - window_start >= MONITOR_WINDOW_MS) {
    if (!g_plot_mode) {
      if (!ppg_acquisition_is_ready()) {
        Serial.println("[ppg] Sensor tidak siap (lepas?) — mencoba init ulang otomatis...");
      } else if (n > 0) {
        float red_dc = red_sum / n, ir_dc = ir_sum / n;
        // Perfusion index kasar = amplitudo AC / DC. Termasuk artefak gerak,
        // jadi ukur saat subjek diam. Dada tipikal jauh lebih kecil dari jari.
        float pi_ir = ir_dc > 0 ? 100.0f * (ir_max - ir_min) / ir_dc : 0;
        const char *contact = ir_dc < SKIN_CONTACT_IR_MIN ? "pantulan IR rendah (cek posisi jari)" : "pantulan IR cukup (indikasi kontak)";
        const char *sat = (red_max >= SATURATION_LEVEL || ir_max >= SATURATION_LEVEL)
                              ? " | SATURASI: turunkan PPG_LED_*_CURRENT atau naikkan PPG_ADC_RANGE_NA"
                              : "";
        Serial.printf("[ppg] %lu sample | red dc=%.0f ac=%lu | ir dc=%.0f ac=%lu | PI_ir=%.2f%% | %s%s\n",
                      (unsigned long)n, red_dc, (unsigned long)(red_max - red_min), ir_dc,
                      (unsigned long)(ir_max - ir_min), pi_ir, contact, sat);
      } else {
        Serial.println("[ppg] FAIL: sensor merespons tetapi tidak ada data FIFO selama 5 detik. Tekan 'r' untuk cek ulang.");
      }
    }
    window_start = millis();
    n = 0;
    red_sum = ir_sum = 0;
    red_min = ir_min = UINT32_MAX;
    red_max = ir_max = 0;
  }
  delay(10);
}
