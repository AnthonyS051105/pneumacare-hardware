#include <Arduino.h>
#include <config.h>

#include "audio_acquisition.h"
#include "noise_filter.h"
#include "buffer_manager.h"

// =====================================================================
// Fase 2 — Test buffer_manager + noise_filter dengan akuisisi I2S nyata
// (1 channel aktif, NUM_AUDIO_CHANNELS=1 di config.h — lihat catatan di sana
// soal keterbatasan budget checkpoint 50%).
//
// Skenario: task acquisition (di sini: loop() utama) menulis terus-menerus
// ke ring buffer per channel. Setiap CONSUMER_INTERVAL_MS, kita simulasikan
// "task network" membaca chunk dari buffer — tapi sengaja jauh lebih jarang
// dari laju tulis, supaya backlog menumpuk dan kebijakan drop-oldest
// (SDD_HARDWARE.md §4) benar-benar teruji, bukan cuma path buffer kosong.
//
// TIDAK ada koneksi jaringan nyata di sini (itu Fase 3) — "consumer" hanya
// mensimulasikan pola baca lambat.
// =====================================================================

#define STATUS_PRINT_INTERVAL_MS 1000
#define SLOW_CONSUMER_INTERVAL_MS 3000 // sengaja lebih lambat dari laju isi buffer

static int32_t g_chunk_out_buf[CHUNK_SAMPLE_COUNT_TEST];

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== Fase 2: buffer_manager + noise_filter (1 channel) ===");
  Serial.printf("NUM_AUDIO_CHANNELS=%d RING_BUFFER_CAPACITY=%d CHUNK_SAMPLE_COUNT=%d\n",
                NUM_AUDIO_CHANNELS, RING_BUFFER_CAPACITY, CHUNK_SAMPLE_COUNT);

  audio_acquisition_init();
  noise_filter_init();
  buffer_manager_init();

  Serial.println("Semua modul siap. Mulai akuisisi + buffering...");
}

void loop() {
  static uint32_t last_status_ms = 0;
  static uint32_t last_consume_ms = 0;

  // --- "Task acquisition": baca I2S, filter, tulis ke ring buffer channel 0 ---
  // audio_acquisition_read() saat ini mengembalikan statistik agregat per batch
  // (bukan sample individual) — avg_abs dipakai sebagai satu sample logis per
  // batch untuk keperluan uji buffering ini. Akses ke sample mentah per-sample
  // menyusul saat audio_acquisition diperluas di Langkah 1b (PoC 4-channel).
  AudioReadResult r = audio_acquisition_read();
  if (r.samples_read > 0) {
    int32_t filtered = noise_filter_apply(0, r.avg_abs);
    buffer_manager_write(0, filtered);
  }

  uint32_t now = millis();

  // --- "Task network" simulasi: baca chunk jauh lebih jarang ---
  if (now - last_consume_ms >= SLOW_CONSUMER_INTERVAL_MS) {
    last_consume_ms = now;
    size_t got = buffer_manager_read_chunk(0, g_chunk_out_buf, CHUNK_SAMPLE_COUNT_TEST);
    Serial.printf("[consumer] chunk dibaca: %u sample\n", (unsigned)got);
  }

  // --- Status berkala: available + drop_count, untuk verifikasi drop-oldest ---
  if (now - last_status_ms >= STATUS_PRINT_INTERVAL_MS) {
    last_status_ms = now;
    size_t available = buffer_manager_available(0);
    uint32_t drops = buffer_manager_get_drop_count(0);
    Serial.printf("[status] ch0 available=%u/%u drop_count=%lu\n",
                  (unsigned)available, (unsigned)RING_BUFFER_CAPACITY,
                  (unsigned long)drops);
  }
}
