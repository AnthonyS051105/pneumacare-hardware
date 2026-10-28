// TEST FASE 1 — PoC akuisisi (env phase1_acquisition). TASK_BREAKDOWN_HARDWARE.md Fase 1.
//
// Menjalankan I2S 4-channel DAN PPG I2C bersamaan (tanpa filter, buffer, atau
// jaringan) untuk memvalidasi risiko teknis terbesar proyek: 4 mic sinkron di
// 1 ESP32 (ARCHITECTURE_HARDWARE.md §4). Per jendela 5 detik dilaporkan:
//   - laju sample audio aktual & overflow DMA (sample hilang)
//   - status tiap mic (hidup / mati)
//   - laju PPG aktual & overflow FIFO sensor
//   - SINKRONISASI antar unit I2S: tepuk tangan keras berjarak SAMA dari mic #1
//     dan mic #3 -> lag hasil cross-correlation harus ~0 sample (|lag| <= 2).
// Bila fase ini FAIL, masalahnya ada di wiring/driver sensor, bukan di jaringan.

#include <Arduino.h>
#include <config.h>
#include <math.h>

#include "audio_acquisition.h"
#include "channel_health.h"
#include "ppg_acquisition.h"

#define REPORT_INTERVAL_MS 5000
#define XCORR_WINDOW 1024
#define XCORR_MAX_LAG 32
// Ambang energi blok untuk dianggap "ada tepukan" (24-bit, |x| rata-rata).
#define IMPULSE_MEAN_ABS_THRESHOLD 200000

static int32_t g_raw[NUM_AUDIO_CHANNELS][AUDIO_I2S_FRAMES_PER_READ];
static ChannelHealth g_health[NUM_AUDIO_CHANNELS];
static bool g_audio_ok = false;

// Riwayat ch index 0 & 2 (unit I2S berbeda) untuk cross-correlation.
static int32_t g_hist_a[XCORR_WINDOW], g_hist_b[XCORR_WINDOW];
static size_t g_hist_pos = 0;
static uint32_t g_impulse_cooldown_until = 0;

static uint32_t g_frames = 0;
static uint32_t g_window_start_us = 0;

static void push_history(size_t frames) {
  for (size_t i = 0; i < frames; i++) {
    g_hist_a[g_hist_pos] = g_raw[0][i];
    g_hist_b[g_hist_pos] = g_raw[2][i];
    g_hist_pos = (g_hist_pos + 1) % XCORR_WINDOW;
  }
}

static int32_t mean_abs(const int32_t *x, size_t n) {
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += (x[i] < 0) ? -(int64_t)x[i] : x[i];
  return (int32_t)(sum / (int64_t)n);
}

// Lag (sample) yang memaksimalkan korelasi b terhadap a. Positif = b terlambat.
static int compute_lag() {
  // Integer 64-bit, bukan double: FPU ESP32 hanya single-precision, double
  // diemulasi software dan cukup lambat untuk membuat DMA I2S overflow.
  int best_lag = 0;
  int64_t best = INT64_MIN;
  for (int lag = -XCORR_MAX_LAG; lag <= XCORR_MAX_LAG; lag++) {
    int64_t acc = 0;
    for (int i = XCORR_MAX_LAG; i < XCORR_WINDOW - XCORR_MAX_LAG; i++) {
      size_t ia = (g_hist_pos + i) % XCORR_WINDOW;
      size_t ib = (g_hist_pos + i + lag) % XCORR_WINDOW;
      acc += (int64_t)(g_hist_a[ia] >> 8) * (int64_t)(g_hist_b[ib] >> 8);
    }
    if (acc > best) {
      best = acc;
      best_lag = lag;
    }
  }
  return best_lag;
}

static void report() {
  float elapsed_s = (micros() - g_window_start_us) / 1e6f;
  float audio_rate = g_frames / elapsed_s;
  float audio_err = fabsf(audio_rate - AUDIO_SAMPLE_RATE_HZ) / AUDIO_SAMPLE_RATE_HZ * 100.0f;
  static uint32_t last_ppg = 0, last_dma_ovf = 0, last_ppg_ovf = 0;
  uint32_t ppg_now = ppg_acquisition_get_samples_read();
  float ppg_rate = (ppg_now - last_ppg) / elapsed_s;
  float ppg_err = fabsf(ppg_rate - PPG_SAMPLE_RATE_HZ) / PPG_SAMPLE_RATE_HZ * 100.0f;
  uint32_t dma_ovf = audio_acquisition_get_dma_overflows() - last_dma_ovf;
  uint32_t ppg_ovf = ppg_acquisition_get_fifo_overflows() - last_ppg_ovf;
  last_ppg = ppg_now;
  last_dma_ovf = audio_acquisition_get_dma_overflows();
  last_ppg_ovf = ppg_acquisition_get_fifo_overflows();

  bool all_ok = true;
  Serial.println("------------------------------------------------------------------");
  bool audio_pass = g_audio_ok && audio_err < 1.0f && dma_ovf == 0;
  all_ok &= audio_pass;
  Serial.printf("[fase1] audio %u ch / %u unit I2S: %.1f Hz (selisih %.2f%%), dma_ovf=%lu, "
                "i2s_err=%lu -> %s\n",
                NUM_AUDIO_CHANNELS, audio_acquisition_units_used(), audio_rate, audio_err,
                (unsigned long)dma_ovf, (unsigned long)audio_acquisition_get_read_errors(),
                audio_pass ? "PASS" : "FAIL");

  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    const ChannelHealth &h = g_health[ch];
    bool pass = h.evaluated && !h.faulty;
    all_ok &= pass;
    Serial.printf("[fase1]   mic ch%u: p2p24=%ld -> %s\n", AUDIO_CHANNEL_ID_MAP[ch],
                  (long)h.last_p2p, pass ? "PASS hidup" : "FAIL tidak ada sinyal (jalankan diag_mic)");
  }

  bool ppg_pass = ppg_acquisition_is_ready() && ppg_err < 5.0f;
  all_ok &= ppg_pass;
  Serial.printf("[fase1] ppg: %.1f Hz (selisih %.1f%%), fifo_ovf=%lu, i2c_err=%lu -> %s\n", ppg_rate,
                ppg_err, (unsigned long)ppg_ovf, (unsigned long)ppg_acquisition_get_i2c_errors(),
                ppg_pass ? "PASS" : "FAIL (jalankan diag_ppg)");

  Serial.printf("[fase1] RINGKASAN: %s — audio & PPG berjalan bersamaan.\n",
                all_ok ? "PASS" : "ADA YANG FAIL");
  if (NUM_AUDIO_CHANNELS >= 3) {
    Serial.println("[fase1] Uji sinkron: tepuk tangan keras berjarak sama dari mic #1 & mic #3.");
  }

  g_frames = 0;
  g_window_start_us = micros();
}

void setup() {
  // Laporan dicetak dari loop yang sama dengan pembacaan I2S — buffer TX besar
  // mencegah Serial.printf memblokir cukup lama hingga DMA overflow.
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== TEST FASE 1: PoC akuisisi I2S 4-channel + PPG ===");

  g_audio_ok = audio_acquisition_init();
  if (!g_audio_ok) Serial.println("[fase1] FAIL init I2S.");
  if (!ppg_acquisition_init()) Serial.println("[fase1] FAIL init PPG (akan dicoba ulang).");

  uint32_t window = (uint32_t)AUDIO_SAMPLE_RATE_HZ * AUDIO_DEAD_CHANNEL_WINDOW_MS / 1000;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    channel_health_init(&g_health[ch], window, AUDIO_DEAD_CHANNEL_P2P_THRESHOLD);
  }
  g_window_start_us = micros();
}

void loop() {
  static uint32_t last_report = millis();

  if (g_audio_ok) {
    size_t frames = audio_acquisition_read(g_raw);
    g_frames += frames;
    for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
      channel_health_update(&g_health[ch], g_raw[ch], frames);
    }

    if (NUM_AUDIO_CHANNELS >= 3 && frames > 0) {
      push_history(frames);
      if (millis() > g_impulse_cooldown_until &&
          mean_abs(g_raw[0], frames) > IMPULSE_MEAN_ABS_THRESHOLD &&
          mean_abs(g_raw[2], frames) > IMPULSE_MEAN_ABS_THRESHOLD) {
        g_impulse_cooldown_until = millis() + 1000;
        int lag = compute_lag();
        Serial.printf("[fase1] Impuls terdeteksi: lag ch3 vs ch1 = %d sample (%.2f ms) -> %s\n", lag,
                      lag * 1000.0f / AUDIO_SAMPLE_RATE_HZ,
                      abs(lag) <= 2 ? "PASS sinkron" : "CEK: pastikan jarak tepukan sama; "
                                                       "selisih 1 sample ~ 2 cm perbedaan jarak");
      }
    }
  } else {
    delay(16);
  }

  PpgSample ppg[PPG_FIFO_DEPTH];
  ppg_acquisition_read(ppg, PPG_FIFO_DEPTH);

  if (millis() - last_report >= REPORT_INTERVAL_MS) {
    last_report = millis();
    report();
  }
}
