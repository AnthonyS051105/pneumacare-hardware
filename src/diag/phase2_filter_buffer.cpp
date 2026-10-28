// TEST FASE 2 — filtering & buffering (env phase2_filter_buffer).
// TASK_BREAKDOWN_HARDWARE.md Fase 2: "test buffer dengan skenario network lambat —
// pastikan drop policy bekerja, tidak crash".
//
// Menjalankan pipeline audio PERSIS seperti firmware produksi (audio_pipeline di
// Core 0), sementara Core 1 mensimulasikan konsumen jaringan dengan 4 skenario
// berurutan (berulang):
//   NORMAL  10 s : baca tiap chunk siap      -> drop HARUS 0
//   LAMBAT  10 s : baca 50% kecepatan        -> drop HARUS > 0 (kebijakan drop-oldest aktif)
//   MACET    5 s : tidak membaca sama sekali -> drop HARUS > 0, tanpa crash/heap bocor
//   PULIH   10 s : kembali normal            -> drop HARUS berhenti bertambah
// Juga memverifikasi filter: rata-rata PCM (DC) setelah high-pass harus ~0.

#include <Arduino.h>
#include <config.h>

#include "audio_pipeline.h"
#include "buffer_manager.h"

enum Scenario { SC_NORMAL, SC_SLOW, SC_STALL, SC_RECOVER, SC_COUNT };
static const char *const kScenarioNames[] = {"NORMAL", "LAMBAT", "MACET", "PULIH"};
static const uint32_t kScenarioDurationMs[] = {10000, 10000, 5000, 10000};
// Selisih DC (int16) yang masih dianggap "DC sudah terbuang".
#define DC_TOLERANCE 50

static volatile bool g_audio_ok = false;
static int16_t g_chunk[CHUNK_SAMPLE_COUNT];

static void task_acquisition(void *) {
  g_audio_ok = audio_pipeline_init();
  if (!g_audio_ok) Serial.println("[fase2] FAIL init I2S — jalankan phase1_acquisition dulu.");
  for (;;) {
    if (g_audio_ok) {
      audio_pipeline_process_block();
    } else {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

static void task_consumer(void *) {
  Scenario sc = SC_NORMAL;
  uint32_t sc_start = millis();
  uint32_t last_slow_read = 0;
  uint32_t drops_at_start[NUM_AUDIO_CHANNELS];
  int64_t pcm_sum[NUM_AUDIO_CHANNELS] = {0};
  uint32_t pcm_count[NUM_AUDIO_CHANNELS] = {0};
  uint32_t heap_at_first_report = 0;

  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) drops_at_start[ch] = 0;
  Serial.printf("[fase2] Skenario %s dimulai.\n", kScenarioNames[sc]);

  for (;;) {
    uint32_t now = millis();
    bool read_now = (sc == SC_NORMAL || sc == SC_RECOVER) ||
                    (sc == SC_SLOW && now - last_slow_read >= 2 * CHUNK_DURATION_MS);
    if (sc == SC_SLOW && read_now) last_slow_read = now;

    if (read_now) {
      for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
        if (buffer_manager_available(ch) < CHUNK_SAMPLE_COUNT) continue;
        size_t got = buffer_manager_read_chunk(ch, g_chunk, CHUNK_SAMPLE_COUNT);
        for (size_t i = 0; i < got; i++) pcm_sum[ch] += g_chunk[i];
        pcm_count[ch] += got;
      }
    }

    if (now - sc_start >= kScenarioDurationMs[sc]) {
      Serial.println("------------------------------------------------------------------");
      bool all_pass = true;
      for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
        AudioChannelStats stats;
        audio_pipeline_get_stats(ch, &stats);
        uint32_t drops = buffer_manager_get_drop_count(ch) - drops_at_start[ch];
        float dc = pcm_count[ch] ? (float)pcm_sum[ch] / pcm_count[ch] : 0.0f;

        if (stats.faulty) {
          Serial.printf("[fase2]   ch%u: mic tidak aktif (dilewati — cek diag_mic)\n",
                        AUDIO_CHANNEL_ID_MAP[ch]);
          continue;
        }
        bool expect_drop = (sc == SC_SLOW || sc == SC_STALL);
        bool drop_ok = expect_drop ? drops > 0 : drops == 0;
        bool dc_ok = pcm_count[ch] == 0 || (dc > -DC_TOLERANCE && dc < DC_TOLERANCE);
        all_pass &= drop_ok && dc_ok;
        Serial.printf("[fase2]   ch%u: drop=%lu (%s) | dc_pcm=%.1f (%s) | clip=%lu\n",
                      AUDIO_CHANNEL_ID_MAP[ch], (unsigned long)drops,
                      drop_ok ? "sesuai" : (expect_drop ? "harusnya ada drop" : "harusnya 0"),
                      dc, dc_ok ? "DC terbuang" : "DC MASIH ADA", (unsigned long)stats.clip_count);
        drops_at_start[ch] = buffer_manager_get_drop_count(ch);
        pcm_sum[ch] = 0;
        pcm_count[ch] = 0;
      }

      uint32_t heap = ESP.getFreeHeap();
      if (heap_at_first_report == 0) heap_at_first_report = heap;
      int32_t heap_delta = (int32_t)heap - (int32_t)heap_at_first_report;
      Serial.printf("[fase2] Skenario %s selesai -> %s | heap=%u (perubahan %+ld sejak awal) min=%u\n",
                    kScenarioNames[sc], (all_pass && g_audio_ok) ? "PASS" : "FAIL", (unsigned)heap,
                    (long)heap_delta, (unsigned)ESP.getMinFreeHeap());

      sc = (Scenario)((sc + 1) % SC_COUNT);
      sc_start = now;
      Serial.printf("[fase2] Skenario %s dimulai.\n", kScenarioNames[sc]);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== TEST FASE 2: filter (HPF %.0f Hz) + ring buffer (%u sample/ch) ===\n",
                AUDIO_HPF_CUTOFF_HZ, (unsigned)RING_BUFFER_CAPACITY);

  if (!buffer_manager_init(NUM_AUDIO_CHANNELS, RING_BUFFER_CAPACITY)) {
    Serial.println("[fase2] FAIL alokasi ring buffer.");
    return;
  }
  xTaskCreatePinnedToCore(task_acquisition, "acquisition", TASK_ACQUISITION_STACK_SIZE, NULL,
                          TASK_ACQUISITION_PRIORITY, NULL, TASK_ACQUISITION_CORE);
  xTaskCreatePinnedToCore(task_consumer, "consumer", 4096, NULL, TASK_NETWORK_PRIORITY, NULL,
                          TASK_NETWORK_CORE);
}

void loop() {
  vTaskDelete(NULL);
}
