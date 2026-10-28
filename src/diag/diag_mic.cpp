// DIAGNOSTIK: mikrofon INMP441 x4 (env diag_mic). Tidak butuh WiFi/backend.
//
// Yang dicek per mic:
//   - Terdeteksi atau tidak (SD terbaca 0 terus = mic lepas / L/R salah)
//   - Macet (nilai konstan) atau hidup (ada noise floor)
//   - Level RMS (dBFS) setelah high-pass, DC offset, puncak, % clipping pada
//     AUDIO_PCM_GAIN_SHIFT saat ini (untuk tuning gain)
//   - Sample rate aktual I2S (harus ~AUDIO_SAMPLE_RATE_HZ)
//
// TAP TEST (verifikasi pemetaan channel <-> posisi fisik, FR-HW-004):
//   Ketuk pelan mic #1 (posterior upper-left) -> bar "ch1" harus melonjak.
//   Bila yang melonjak ch lain, tukar entri AUDIO_CHANNEL_SOURCES di config.h.

#include <Arduino.h>
#include <config.h>
#include <math.h>

#include "audio_acquisition.h"
#include "audio_pcm.h"
#include "channel_health.h"
#include "noise_filter.h"

#define METER_INTERVAL_MS 250
#define SUMMARY_INTERVAL_MS 5000
#define METER_BAR_WIDTH 20
#define METER_MIN_DBFS -90.0f
#define METER_MAX_DBFS -20.0f
#define FULL_SCALE_24BIT 8388608.0f
#define WEAK_SIGNAL_DBFS -100.0f
#define CLIP_WARN_PERCENT 0.5f

struct WindowStats {
  int64_t sum_raw;
  int32_t min_raw;
  int32_t max_raw;
  double sum_sq_filtered;
  float peak_filtered;
  uint32_t clipped;
  uint32_t count;
};

static int32_t g_raw[NUM_AUDIO_CHANNELS][AUDIO_I2S_FRAMES_PER_READ];
static WindowStats g_summary[NUM_AUDIO_CHANNELS];
static double g_meter_sum_sq[NUM_AUDIO_CHANNELS];
static uint32_t g_meter_count = 0;
static uint32_t g_frames_in_summary = 0;
static uint32_t g_summary_start_us = 0;
static uint32_t g_last_meter_ms = 0;
static bool g_audio_ok = false;

static void reset_summary() {
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    g_summary[ch] = WindowStats{0, INT32_MAX, INT32_MIN, 0.0, 0.0f, 0, 0};
  }
  g_frames_in_summary = 0;
  g_summary_start_us = micros();
}

static float to_dbfs(double rms) {
  if (rms <= 0.0) return -140.0f;
  return 20.0f * log10f((float)(rms / FULL_SCALE_24BIT));
}

static void print_meter() {
  char line[256];
  size_t pos = 0;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    float db = to_dbfs(sqrt(g_meter_sum_sq[ch] / (g_meter_count ? g_meter_count : 1)));
    float frac = (db - METER_MIN_DBFS) / (METER_MAX_DBFS - METER_MIN_DBFS);
    int bars = (int)(frac * METER_BAR_WIDTH);
    if (bars < 0) bars = 0;
    if (bars > METER_BAR_WIDTH) bars = METER_BAR_WIDTH;

    pos += snprintf(line + pos, sizeof(line) - pos, "ch%u %4.0fdB |", AUDIO_CHANNEL_ID_MAP[ch], db);
    for (int i = 0; i < METER_BAR_WIDTH && pos < sizeof(line) - 2; i++) {
      line[pos++] = (i < bars) ? '#' : '.';
    }
    pos += snprintf(line + pos, sizeof(line) - pos, "|  ");
    g_meter_sum_sq[ch] = 0.0;
  }
  g_meter_count = 0;
  Serial.println(line);
}

static void print_summary() {
  float elapsed_s = (micros() - g_summary_start_us) / 1e6f;
  float rate = elapsed_s > 0 ? g_frames_in_summary / elapsed_s : 0.0f;
  float rate_err = fabsf(rate - AUDIO_SAMPLE_RATE_HZ) / AUDIO_SAMPLE_RATE_HZ * 100.0f;

  Serial.println("------------------------------------------------------------------");
  Serial.printf("[mic] sample rate aktual %.1f Hz (target %d, selisih %.2f%%) %s | dma_ovf=%lu\n",
                rate, AUDIO_SAMPLE_RATE_HZ, rate_err, rate_err < 1.0f ? "PASS" : "FAIL",
                (unsigned long)audio_acquisition_get_dma_overflows());

  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    WindowStats &s = g_summary[ch];
    if (s.count == 0) continue;
    int32_t p2p = s.max_raw - s.min_raw;
    float dc = (float)s.sum_raw / s.count;
    float rms_db = to_dbfs(sqrt(s.sum_sq_filtered / s.count));
    float peak_db = to_dbfs(s.peak_filtered);
    float clip_pct = 100.0f * s.clipped / s.count;
    const AudioChannelSource &src = AUDIO_CHANNEL_SOURCES[ch];

    const char *verdict;
    if (p2p == 0) {
      verdict = "FAIL tidak ada data: cek kabel SD/VDD/GND, dan pin L/R (slot 0 = L/R ke GND, "
                "slot 1 = L/R ke VDD; dua mic satu bus TIDAK boleh L/R sama)";
    } else if (p2p < AUDIO_DEAD_CHANNEL_P2P_THRESHOLD) {
      verdict = "FAIL nilai macet/konstan: mic kemungkinan rusak";
    } else if (rms_db < WEAK_SIGNAL_DBFS) {
      verdict = "WARN sinyal sangat lemah, di bawah noise floor INMP441 normal";
    } else if (clip_pct > CLIP_WARN_PERCENT) {
      verdict = "WARN clipping — naikkan AUDIO_PCM_GAIN_SHIFT di config.h";
    } else {
      verdict = "PASS";
    }

    Serial.printf("[mic] ch%u (I2S%u slot %u, SD GPIO%d): rms=%.1f dBFS puncak=%.1f dBFS "
                  "dc=%.0f p2p=%ld clip=%.2f%% -> %s\n",
                  AUDIO_CHANNEL_ID_MAP[ch], src.i2s_unit, src.slot,
                  src.i2s_unit == 0 ? PIN_I2S_SD_1 : PIN_I2S_SD_2, rms_db, peak_db, dc, (long)p2p,
                  clip_pct, verdict);
  }
  // int16 full-scale setara 32768 * 2^shift pada skala 24-bit.
  float clip_dbfs = 20.0f * log10f(32768.0f * (1 << AUDIO_PCM_GAIN_SHIFT) / FULL_SCALE_24BIT);
  Serial.printf("[mic] Gain: AUDIO_PCM_GAIN_SHIFT=%d -> clipping di atas %.1f dBFS. Saat mic "
                "menempel kulit & napas dalam, puncak sebaiknya 6-20 dB di bawah angka itu.\n",
                AUDIO_PCM_GAIN_SHIFT, clip_dbfs);
  Serial.println("[mic] TAP TEST: ketuk mic #1 -> bar ch1 harus naik (lihat meter).");
  Serial.println("------------------------------------------------------------------");
}

void setup() {
  // Meter & ringkasan dicetak di loop pembacaan I2S — buffer TX besar mencegah
  // Serial.printf memblokir cukup lama hingga DMA overflow.
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== DIAG: mikrofon INMP441 (%d channel) ===\n", NUM_AUDIO_CHANNELS);
  Serial.printf("I2S0 SCK/WS/SD=%d/%d/%d; I2S1 SCK/WS/SD=%d/%d/%d\n",
                PIN_I2S_SCK_1, PIN_I2S_WS_1, PIN_I2S_SD_1,
                PIN_I2S_SCK_2, PIN_I2S_WS_2, PIN_I2S_SD_2);

  noise_filter_init(AUDIO_SAMPLE_RATE_HZ, AUDIO_HPF_CUTOFF_HZ, AUDIO_LPF_CUTOFF_HZ);
  g_audio_ok = audio_acquisition_init();
  if (!g_audio_ok) {
    Serial.println("[mic] FAIL: driver I2S gagal / clock tidak berjalan. Cek SCK & WS, dan "
                   "pastikan tidak ada pin I2S yang bentrok dengan perangkat lain.");
  }
  reset_summary();
}

void loop() {
  if (!g_audio_ok) {
    delay(5000);
    g_audio_ok = audio_acquisition_init();
    reset_summary();
    return;
  }

  size_t frames = audio_acquisition_read(g_raw);
  if (frames == 0) {
    Serial.println("[mic] Timeout membaca I2S.");
    return;
  }
  g_frames_in_summary += frames;

  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    WindowStats &s = g_summary[ch];
    for (size_t i = 0; i < frames; i++) {
      int32_t x = g_raw[ch][i];
      float y = noise_filter_apply(ch, (float)x);
      bool clipped = false;
      audio_pcm_from_sample24(y, AUDIO_PCM_GAIN_SHIFT, &clipped);

      s.sum_raw += x;
      if (x < s.min_raw) s.min_raw = x;
      if (x > s.max_raw) s.max_raw = x;
      s.sum_sq_filtered += (double)y * y;
      if (fabsf(y) > s.peak_filtered) s.peak_filtered = fabsf(y);
      if (clipped) s.clipped++;
      s.count++;
      g_meter_sum_sq[ch] += (double)y * y;
    }
  }
  g_meter_count += frames;

  uint32_t now = millis();
  if (now - g_last_meter_ms >= METER_INTERVAL_MS) {
    g_last_meter_ms = now;
    print_meter();
  }
  if ((micros() - g_summary_start_us) / 1000 >= SUMMARY_INTERVAL_MS) {
    print_summary();
    reset_summary();
  }
}
