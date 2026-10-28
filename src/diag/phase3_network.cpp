// TEST FASE 3 — konektivitas jaringan (env phase3_network). Tanpa sensor.
// TASK_BREAKDOWN_HARDWARE.md Fase 3.
//
// Memakai modul jaringan PRODUKSI (network_manager, ws_client, mqtt_client,
// reconnect_manager) dengan data sintetis real-time:
//   - Audio: nada 200/400/600/800 Hz per channel ditulis ke buffer_manager
//     dengan laju persis AUDIO_SAMPLE_RATE_HZ (task Core 0, pengganti mikrofon)
//   - PPG: gelombang ~72 bpm 100 Hz lewat FreeRTOS queue (pengganti MAX30102)
// Semua data memakai DIAG_DEVICE_ID (tidak tertaut pasien).
//
// Uji reconnect (FR-HW-024): saat berjalan, matikan router / hentikan backend /
// hentikan Mosquitto, lalu nyalakan lagi. Log harus menunjukkan percobaan
// ulang dengan jeda 1s, 2s, 4s, ... maks 30s, dan streaming pulih sendiri.

#include <Arduino.h>
#include <config.h>
#include <math.h>

#include "buffer_manager.h"
#include "mqtt_client.h"
#include "network_manager.h"
#include "ppg_acquisition.h"
#include "ws_client.h"

#define GENERATOR_BLOCK_MS 16
#define GENERATOR_BLOCK_FRAMES (AUDIO_SAMPLE_RATE_HZ * GENERATOR_BLOCK_MS / 1000)
#define TONE_AMPLITUDE 8000
#define REPORT_INTERVAL_MS 5000

static QueueHandle_t g_ppg_queue;

static void task_generator(void *) {
  static int16_t block[GENERATOR_BLOCK_FRAMES];
  uint32_t phase = 0;
  float ppg_phase_acc = 0.0f;
  uint32_t ppg_n = 0;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
      float freq = 200.0f * (ch + 1);
      for (size_t i = 0; i < GENERATOR_BLOCK_FRAMES; i++) {
        block[i] = (int16_t)(TONE_AMPLITUDE *
                             sinf(2.0f * 3.14159265f * freq * (phase + i) / AUDIO_SAMPLE_RATE_HZ));
      }
      buffer_manager_write_block(ch, block, GENERATOR_BLOCK_FRAMES);
    }
    phase = (phase + GENERATOR_BLOCK_FRAMES) % AUDIO_SAMPLE_RATE_HZ;

    // PPG: PPG_SAMPLE_RATE_HZ * 16 ms = 1.6 sample per blok -> akumulator pecahan.
    ppg_phase_acc += PPG_SAMPLE_RATE_HZ * GENERATOR_BLOCK_MS / 1000.0f;
    while (ppg_phase_acc >= 1.0f) {
      ppg_phase_acc -= 1.0f;
      float beat = sinf(2.0f * 3.14159265f * 1.2f * ppg_n / PPG_SAMPLE_RATE_HZ);
      ppg_n = (ppg_n + 1) % (PPG_SAMPLE_RATE_HZ * 5); // 1.2 Hz x 5 s = 6 siklus genap
      PpgSample s = {(uint32_t)(90000 + 500 * beat), (uint32_t)(120000 + 800 * beat)};
      xQueueSend(g_ppg_queue, &s, 0);
    }

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(GENERATOR_BLOCK_MS));
  }
}

static void report() {
  static uint32_t last_chunks = 0, last_batches = 0;
  WsClientStats ws;
  MqttClientStats mqtt;
  ws_client_get_stats(&ws);
  mqtt_client_get_stats(&mqtt);

  float secs = REPORT_INTERVAL_MS / 1000.0f;
  float chunk_rate = (ws.chunks_sent - last_chunks) / secs;
  float batch_rate = (mqtt.batches_published - last_batches) / secs;
  last_chunks = ws.chunks_sent;
  last_batches = mqtt.batches_published;

  float expected_chunk_rate = NUM_AUDIO_CHANNELS * 1000.0f / CHUNK_DURATION_MS;
  float expected_batch_rate = (float)PPG_SAMPLE_RATE_HZ / PPG_BATCH_SIZE;
  uint32_t drops = 0;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) drops += buffer_manager_get_drop_count(ch);

  bool ws_pass = ws_client_is_connected() && chunk_rate >= expected_chunk_rate * 0.9f;
  bool mqtt_pass = mqtt_client_is_connected() && batch_rate >= expected_batch_rate * 0.8f;

  Serial.println("------------------------------------------------------------------");
  Serial.printf("[fase3] wifi=%d (%d dBm) ntp=%d heap=%u min=%u\n", network_manager_is_connected(),
                network_manager_rssi(), network_manager_is_time_synced(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  Serial.printf("[fase3] websocket: %.1f chunk/s (target %.1f), total=%lu gagal=%lu connect=%lu "
                "drop_buffer=%lu -> %s\n",
                chunk_rate, expected_chunk_rate, (unsigned long)ws.chunks_sent,
                (unsigned long)ws.send_failures, (unsigned long)ws.connects, (unsigned long)drops,
                ws_pass ? "PASS" : "FAIL");
  Serial.printf("[fase3] mqtt: %.2f batch/s (target %.2f), total=%lu gagal=%lu connect=%lu "
                "sample_dibuang=%lu -> %s\n",
                batch_rate, expected_batch_rate, (unsigned long)mqtt.batches_published,
                (unsigned long)mqtt.publish_failures, (unsigned long)mqtt.connects,
                (unsigned long)mqtt.samples_dropped, mqtt_pass ? "PASS" : "FAIL");
}

static void task_network(void *) {
  network_manager_init();
  ws_client_init(DIAG_DEVICE_ID);
  mqtt_client_init(DIAG_DEVICE_ID);
  uint32_t last_report = millis();

  for (;;) {
    network_manager_tick();
    bool streaming = network_manager_is_connected();
    ws_client_tick(streaming);

    PpgSample s;
    while (xQueueReceive(g_ppg_queue, &s, 0) == pdTRUE) {
      if (streaming) mqtt_client_push_ppg_sample(s.red, s.ir);
    }
    mqtt_client_tick(-1);

    if (millis() - last_report >= REPORT_INTERVAL_MS) {
      last_report = millis();
      report();
    }
    vTaskDelay(pdMS_TO_TICKS(TASK_NETWORK_LOOP_DELAY_MS));
  }
}

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== TEST FASE 3: jaringan (device %s, data sintetis) ===\n", DIAG_DEVICE_ID);
  Serial.printf("ws://%s:%d%s | mqtt %s:%d\n", BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH,
                MQTT_BROKER_HOST, MQTT_BROKER_PORT);

  g_ppg_queue = xQueueCreate(PPG_SAMPLE_QUEUE_LENGTH, sizeof(PpgSample));
  if (g_ppg_queue == NULL || !buffer_manager_init(NUM_AUDIO_CHANNELS, RING_BUFFER_CAPACITY)) {
    Serial.println("[fase3] FAIL alokasi memori.");
    return;
  }
  xTaskCreatePinnedToCore(task_generator, "generator", 4096, NULL, TASK_ACQUISITION_PRIORITY, NULL,
                          TASK_ACQUISITION_CORE);
  xTaskCreatePinnedToCore(task_network, "network", TASK_NETWORK_STACK_SIZE, NULL,
                          TASK_NETWORK_PRIORITY, NULL, TASK_NETWORK_CORE);
}

void loop() {
  vTaskDelete(NULL);
}
