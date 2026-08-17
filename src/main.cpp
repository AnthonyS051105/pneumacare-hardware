#include <Arduino.h>
#include <config.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "audio_acquisition.h"
#include "noise_filter.h"
#include "buffer_manager.h"
#include "ppg_acquisition.h"
#include "network_manager.h"
#include "ws_client.h"
#include "mqtt_client.h"

// =====================================================================
// Fase 4 — Integrasi penuh: dual-core FreeRTOS sesuai SDD_HARDWARE.md §2.
//
// Core 0: task "acquisition" (prioritas tinggi)
//   - Baca I2S (audio) & I2C (PPG) terus-menerus
//   - Audio: filter -> tulis ke ring buffer (buffer_manager, sudah punya
//     portMUX per channel — aman diakses lintas-core apa adanya)
//   - PPG: kirim tiap sample ke g_ppg_sample_queue (FreeRTOS queue),
//     TIDAK memanggil mqtt_client langsung (state batch MQTT hanya boleh
//     disentuh dari Core 1 — lihat catatan di mqtt_client.h)
//
// Core 1: task "network" (prioritas normal)
//   - Reconnect WiFi/WS/MQTT, kirim chunk audio, drain g_ppg_sample_queue
//     lalu dorong ke mqtt_client, heartbeat berkala
//
// NUM_AUDIO_CHANNELS (config.h, saat ini=1) tidak pernah di-hardcode di
// bawah — seluruh loop channel memakai konstanta itu, sama seperti
// buffer_manager/noise_filter/ws_client. Menaikkan ke 4 nanti (Langkah 1b)
// tidak butuh perubahan struktur task di file ini.
// =====================================================================

#define STATUS_PRINT_INTERVAL_MS 1000

// Dikirim lewat FreeRTOS queue dari task acquisition (Core 0) ke task network
// (Core 1) — red+ir HARUS sepasang (bukan 2 sample_t independen) supaya backend
// bisa menghitung SpO2 (INTEGRATION_CONTRACT.md §3.3, revisi 17 Agt 2026).
struct PpgQueueSample {
  uint32_t red;
  uint32_t ir;
};

static QueueHandle_t g_ppg_sample_queue;

// --- Task acquisition (Core 0) ---
static void task_acquisition(void *pvParameters) {
  audio_acquisition_init();
  noise_filter_init();
  buffer_manager_init();

  if (!ppg_acquisition_init()) {
    Serial.println("[FATAL] Inisialisasi PPG gagal, cek wiring I2C (SDA/SCL/VDD/GND).");
    vTaskDelete(NULL);
    return;
  }

  Serial.printf("[acquisition] Task siap (Core %d).\n", TASK_ACQUISITION_CORE);

  for (;;) {
    // Audio: i2s_read() di dalam ini blocking (~samples/sample_rate detik),
    // secara alami mem-pace task ini tanpa perlu vTaskDelay tambahan.
    // Setiap sample MENTAH individual dari batch ditulis ke ring buffer
    // (bukan diringkas jadi 1 nilai agregat) — Model A backend butuh bentuk
    // gelombang asli untuk mel-spectrogram, bukan statistik volume.
    //
    // ⚠️ Loop channel di sini TIDAK ADA karena audio_acquisition saat ini
    // hanya membaca 1 sumber I2S fisik (mic #1) — begitu Langkah 1b
    // (driver 4-channel) selesai, baca per-channel akan dilakukan di
    // audio_acquisition itu sendiri lalu di-loop di sini berdasarkan
    // NUM_AUDIO_CHANNELS, bukan diasumsikan tunggal seperti sekarang.
    AudioReadResult r = audio_acquisition_read();
    for (size_t i = 0; i < r.samples_read; i++) {
      int32_t filtered = noise_filter_apply(0, r.samples[i]);
      buffer_manager_write(0, filtered);
    }

    // PPG: non-blocking, kirim ke queue (drop bila queue penuh — jangan
    // block task acquisition demi 1 sample PPG, sesuai prinsip SDD §4:
    // drop, bukan menahan/memblokir akuisisi).
    PpgReadResult ppg = ppg_acquisition_read();
    if (ppg.has_sample) {
      PpgQueueSample q_sample = {ppg.red, ppg.ir};
      if (xQueueSend(g_ppg_sample_queue, &q_sample, 0) != pdTRUE) {
        // Queue penuh (task network lebih lambat dari akuisisi PPG) —
        // sample terbaru di-drop, konsisten dengan kebijakan buffer_manager.
      }
    }
  }
}

// --- Task network (Core 1) ---
static void task_network(void *pvParameters) {
  network_manager_init(); // blocking sampai WiFi awal tersambung; NTP best-effort
  ws_client_init();
  mqtt_client_init();

  Serial.printf("[network] Task siap (Core %d). Mulai streaming...\n", TASK_NETWORK_CORE);

  uint32_t last_heartbeat_ms = 0;
  uint32_t last_status_ms = 0;

  for (;;) {
    network_manager_reconnect_tick();
    ws_client_reconnect_tick();
    mqtt_client_reconnect_tick();

    ws_client_tick(); // sudah channel-agnostic internal (loop NUM_AUDIO_CHANNELS)

    // Drain semua sample PPG yang menumpuk di queue, dorong ke batch mqtt_client.
    // Hanya task ini yang boleh memanggil mqtt_client_push_ppg_sample() — lihat
    // catatan di mqtt_client.h soal kepemilikan single-core atas state batch.
    PpgQueueSample q_sample;
    while (xQueueReceive(g_ppg_sample_queue, &q_sample, 0) == pdTRUE) {
      mqtt_client_push_ppg_sample(q_sample.red, q_sample.ir);
    }

    mqtt_client_tick();

    uint32_t now = millis();

    if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
      last_heartbeat_ms = now;
      mqtt_client_publish_heartbeat();
    }

    if (now - last_status_ms >= STATUS_PRINT_INTERVAL_MS) {
      last_status_ms = now;
      Serial.printf(
          "[status] wifi=%d ws=%d mqtt=%d ch0_available=%u/%u drop_count=%lu "
          "heap_free=%u\n",
          network_manager_is_connected(), ws_client_is_connected(),
          mqtt_client_is_connected(), (unsigned)buffer_manager_available(0),
          (unsigned)RING_BUFFER_CAPACITY,
          (unsigned long)buffer_manager_get_drop_count(0),
          (unsigned)ESP.getFreeHeap());
    }

    // Task ini tidak punya blocking call natural per-iterasi (beda dengan
    // task acquisition yang di-pace i2s_read()) — delay eksplisit mencegah
    // starvation WiFi/FreeRTOS idle task & watchdog trigger (config.h).
    vTaskDelay(pdMS_TO_TICKS(TASK_NETWORK_LOOP_DELAY_MS));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== Fase 4: integrasi dual-core FreeRTOS ===");

  g_ppg_sample_queue = xQueueCreate(PPG_SAMPLE_QUEUE_LENGTH, sizeof(PpgQueueSample));
  if (g_ppg_sample_queue == NULL) {
    Serial.println("[FATAL] Gagal membuat PPG sample queue.");
    while (true) delay(1000);
  }

  xTaskCreatePinnedToCore(task_acquisition, "acquisition", TASK_ACQUISITION_STACK_SIZE,
                           NULL, TASK_ACQUISITION_PRIORITY, NULL, TASK_ACQUISITION_CORE);

  xTaskCreatePinnedToCore(task_network, "network", TASK_NETWORK_STACK_SIZE, NULL,
                           TASK_NETWORK_PRIORITY, NULL, TASK_NETWORK_CORE);

  // setup()/loop() Arduino berjalan sebagai task tersendiri (loopTask) —
  // tidak dipakai lagi untuk logic aplikasi sejak dual-core diaktifkan,
  // biarkan idle supaya tidak berebut CPU dengan 2 task di atas.
}

void loop() {
  vTaskDelete(NULL);
}
