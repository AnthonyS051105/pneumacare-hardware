#include <Arduino.h>
#include <config.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "audio_acquisition.h"
#include "audio_pipeline.h"
#include "battery_monitor.h"
#include "buffer_manager.h"
#include "mqtt_client.h"
#include "network_manager.h"
#include "ppg_acquisition.h"
#include "status_led.h"
#include "ws_client.h"

// =====================================================================
// Firmware produksi PNEUMACARE — integrasi penuh dual-core FreeRTOS
// (SDD_HARDWARE.md §2) dengan state machine koneksi (SDD_HARDWARE.md §3).
//
// Core 0: task "acquisition" (prioritas lebih tinggi, timing sensitif)
//   - I2S 4-channel -> filter -> PCM int16 -> ring buffer (audio_pipeline)
//   - PPG MAX30102 -> g_ppg_queue (FreeRTOS queue ke Core 1)
//   - Sensor gagal: dicoba init ulang berkala, TANPA menghentikan sensor lain
//
// Core 1: task "network"
//   WIFI_CONNECTING -> NTP_SYNC (best-effort, ber-timeout) -> STREAMING
//   STREAMING -> (WiFi putus) -> WIFI_CONNECTING; WS/MQTT reconnect sendiri
//   dengan exponential backoff tanpa pernah menyerah permanen.
//
// Keandalan (NFR-HW-001): task watchdog untuk kedua task, restart otomatis
// bila WiFi hilang terlalu lama atau heap kritis, alasan reset dicetak saat boot.
// =====================================================================

#define AUDIO_REINIT_INTERVAL_MS 5000
#define AUDIO_MAX_CONSECUTIVE_READ_FAILURES 10
#define PPG_POLL_INTERVAL_WITHOUT_AUDIO_MS 10

enum NetworkState {
  NET_WIFI_CONNECTING,
  NET_NTP_SYNC,
  NET_STREAMING,
};

static const char *const kNetworkStateNames[] = {"WIFI_CONNECTING", "NTP_SYNC", "STREAMING"};

static QueueHandle_t g_ppg_queue = NULL;

// Ditulis Core 0, dibaca Core 1 (akses 32-bit di ESP32 atomik).
static volatile bool g_audio_ok = false;
static volatile uint32_t g_ppg_queue_drops = 0;

// ---------------------------------------------------------------------
// Task acquisition (Core 0)
// ---------------------------------------------------------------------

static void poll_ppg() {
  PpgSample samples[PPG_FIFO_DEPTH];
  size_t n = ppg_acquisition_read(samples, PPG_FIFO_DEPTH);
  for (size_t i = 0; i < n; i++) {
    // Jangan pernah memblokir akuisisi demi antrean PPG yang penuh (SDD §4).
    if (xQueueSend(g_ppg_queue, &samples[i], 0) != pdTRUE) g_ppg_queue_drops++;
  }
}

static void task_acquisition(void *) {
  esp_task_wdt_add(NULL);

  // SENSOR_INIT (SDD §3): kegagalan dicatat & dicoba lagi, bukan berhenti.
  g_audio_ok = audio_pipeline_init();
  if (!g_audio_ok) Serial.println("[acquisition] Init I2S gagal — dicoba ulang berkala.");
  if (!ppg_acquisition_init()) Serial.println("[acquisition] Init PPG gagal — dicoba ulang berkala.");

  uint32_t last_audio_init_ms = millis();
  uint32_t consecutive_failures = 0;

  Serial.printf("[acquisition] Task berjalan (Core %d).\n", xPortGetCoreID());

  for (;;) {
    esp_task_wdt_reset();

    if (!g_audio_ok) {
      if (millis() - last_audio_init_ms >= AUDIO_REINIT_INTERVAL_MS) {
        last_audio_init_ms = millis();
        g_audio_ok = audio_pipeline_init();
        consecutive_failures = 0;
      }
      poll_ppg();
      vTaskDelay(pdMS_TO_TICKS(PPG_POLL_INTERVAL_WITHOUT_AUDIO_MS));
      continue;
    }

    // Blocking ~16 ms (dipacu DMA I2S) — sekaligus mem-pace polling PPG.
    if (audio_pipeline_process_block() == 0) {
      if (++consecutive_failures >= AUDIO_MAX_CONSECUTIVE_READ_FAILURES) {
        Serial.println("[acquisition] I2S berhenti merespons — init ulang.");
        g_audio_ok = false;
        last_audio_init_ms = 0;
      }
    } else {
      consecutive_failures = 0;
    }

    poll_ppg();
  }
}

// ---------------------------------------------------------------------
// Task network (Core 1)
// ---------------------------------------------------------------------

static bool any_sensor_fault() {
  if (!g_audio_ok || !ppg_acquisition_is_ready()) return true;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    AudioChannelStats stats;
    audio_pipeline_get_stats(ch, &stats);
    if (stats.faulty) return true;
  }
  return false;
}

static void update_led(NetworkState state) {
  if (state == NET_WIFI_CONNECTING) {
    status_led_set_mode(LED_MODE_WIFI_CONNECTING);
  } else if (any_sensor_fault()) {
    status_led_set_mode(LED_MODE_SENSOR_FAULT);
  } else if (!ws_client_is_connected() || !mqtt_client_is_connected()) {
    status_led_set_mode(LED_MODE_BACKEND_CONNECTING);
  } else {
    status_led_set_mode(LED_MODE_STREAMING);
  }
  status_led_tick();
}

static void print_status(NetworkState state, int battery_pct) {
  static uint32_t last_ppg_samples = 0;
  static uint32_t last_print_ms = 0;
  uint32_t now = millis();

  WsClientStats ws;
  MqttClientStats mqtt;
  ws_client_get_stats(&ws);
  mqtt_client_get_stats(&mqtt);

  Serial.printf("[status] up=%lus state=%s wifi=%d(%ddBm) ntp=%d ws=%d mqtt=%d heap=%u min=%u bat=%d\n",
                (unsigned long)(now / 1000), kNetworkStateNames[state],
                network_manager_is_connected(), network_manager_rssi(),
                network_manager_is_time_synced(), ws_client_is_connected(),
                mqtt_client_is_connected(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(), battery_pct);

  Serial.printf("[status] audio ok=%d dma_ovf=%lu i2s_err=%lu chunks_sent=%lu ws_fail=%lu\n",
                g_audio_ok, (unsigned long)audio_acquisition_get_dma_overflows(),
                (unsigned long)audio_acquisition_get_read_errors(), (unsigned long)ws.chunks_sent,
                (unsigned long)ws.send_failures);
  Serial.printf("[status] ws build_max_us=%lu send_max_us=%lu (since boot)\n",
                (unsigned long)ws.max_build_us, (unsigned long)ws.max_send_us);
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    AudioChannelStats s;
    audio_pipeline_get_stats(ch, &s);
    float clip_pct = s.sample_count ? 100.0f * s.clip_count / s.sample_count : 0.0f;
    Serial.printf("[status]   ch%u %s p2p24=%ld peak16=%d clip=%.3f%% drop=%lu\n",
                  AUDIO_CHANNEL_ID_MAP[ch], s.faulty ? "FAULT" : (s.evaluated ? "ok   " : "init "),
                  (long)s.last_p2p, s.last_peak, clip_pct,
                  (unsigned long)buffer_manager_get_drop_count(ch));
  }

  uint32_t ppg_samples = ppg_acquisition_get_samples_read();
  float elapsed_s = last_print_ms ? (now - last_print_ms) / 1000.0f : 0.0f;
  float ppg_rate = elapsed_s > 0 ? (ppg_samples - last_ppg_samples) / elapsed_s : 0.0f;
  last_ppg_samples = ppg_samples;
  last_print_ms = now;

  Serial.printf("[status] ppg ready=%d rate=%.1fHz fifo_ovf=%lu i2c_err=%lu queue_drop=%lu "
                "batches=%lu mqtt_fail=%lu mqtt_drop=%lu\n",
                ppg_acquisition_is_ready(), ppg_rate,
                (unsigned long)ppg_acquisition_get_fifo_overflows(),
                (unsigned long)ppg_acquisition_get_i2c_errors(), (unsigned long)g_ppg_queue_drops,
                (unsigned long)mqtt.batches_published, (unsigned long)mqtt.publish_failures,
                (unsigned long)mqtt.samples_dropped);
}

static void check_recovery() {
#if WIFI_LOST_RESTART_MS > 0
  if (network_manager_disconnected_duration_ms() > WIFI_LOST_RESTART_MS) {
    Serial.println("[recovery] WiFi tidak pulih terlalu lama — restart perangkat.");
    Serial.flush();
    ESP.restart();
  }
#endif
  if (ESP.getFreeHeap() < HEAP_CRITICAL_RESTART_BYTES) {
    Serial.printf("[recovery] Heap kritis (%u byte) — restart perangkat.\n",
                  (unsigned)ESP.getFreeHeap());
    Serial.flush();
    ESP.restart();
  }
}

static void task_network(void *) {
  network_manager_init();
  if (!ws_client_init(DEVICE_ID)) {
    Serial.println("[FATAL] Buffer websocket tidak bisa dialokasikan — restart.");
    Serial.flush();
    ESP.restart();
  }
  mqtt_client_init(DEVICE_ID);
  battery_monitor_init();
  esp_task_wdt_add(NULL);

  NetworkState state = NET_WIFI_CONNECTING;
  uint32_t ntp_wait_start_ms = 0;
  uint32_t last_status_ms = 0;
  uint32_t last_battery_ms = 0;
  int battery_pct = battery_monitor_read_percent();

  Serial.printf("[network] Task berjalan (Core %d).\n", xPortGetCoreID());

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();

    network_manager_tick();
    bool wifi = network_manager_is_connected();

    NetworkState previous = state;
    switch (state) {
      case NET_WIFI_CONNECTING:
        if (wifi) {
          state = NET_NTP_SYNC;
          ntp_wait_start_ms = now;
        }
        break;
      case NET_NTP_SYNC:
        if (!wifi) {
          state = NET_WIFI_CONNECTING;
        } else if (network_manager_is_time_synced()) {
          state = NET_STREAMING;
        } else if (now - ntp_wait_start_ms >= NTP_SYNC_TIMEOUT_MS) {
          Serial.println("[network] NTP belum sinkron (timeout) — streaming dengan timestamp "
                         "fallback; SNTP terus mencoba di latar belakang.");
          state = NET_STREAMING;
        }
        break;
      case NET_STREAMING:
        if (!wifi) state = NET_WIFI_CONNECTING;
        break;
    }
    if (state != previous) {
      Serial.printf("[network] State %s -> %s\n", kNetworkStateNames[previous],
                    kNetworkStateNames[state]);
    }

    bool streaming = (state == NET_STREAMING);

    ws_client_tick(streaming);

    // Drain sample PPG dari Core 0. Di luar STREAMING, sample dibuang supaya
    // antrean tidak penuh dan batch pertama nanti berisi data segar.
    PpgSample sample;
    while (xQueueReceive(g_ppg_queue, &sample, 0) == pdTRUE) {
      if (streaming) mqtt_client_push_ppg_sample(sample.red, sample.ir);
    }

    if (now - last_battery_ms >= HEARTBEAT_INTERVAL_MS) {
      last_battery_ms = now;
      battery_pct = battery_monitor_read_percent();
    }
    mqtt_client_tick(battery_pct);

    update_led(state);

    if (now - last_status_ms >= STATUS_LOG_INTERVAL_MS) {
      last_status_ms = now;
      print_status(state, battery_pct);
    }

    check_recovery();

    // Tidak ada blocking call natural per iterasi — delay mencegah starvation
    // idle task & memberi waktu stack WiFi.
    vTaskDelay(pdMS_TO_TICKS(TASK_NETWORK_LOOP_DELAY_MS));
  }
}

// ---------------------------------------------------------------------

static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart (recovery)";
    case ESP_RST_PANIC: return "PANIC/crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog (task macet)";
    case ESP_RST_WDT: return "watchdog lain";
    case ESP_RST_BROWNOUT: return "brownout (tegangan baterai drop)";
    case ESP_RST_DEEPSLEEP: return "bangun dari deep sleep";
    case ESP_RST_EXT: return "reset eksternal";
    default: return "tidak diketahui";
  }
}

void setup() {
  // Log dari task acquisition (mis. transisi status mic) tidak boleh memblokir
  // pembacaan I2S lama-lama karena buffer TX serial penuh.
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  delay(500);

  status_led_init();

  Serial.println();
  Serial.println("=== PNEUMACARE firmware ===");
  Serial.printf("device_id=%s reset=%s\n", DEVICE_ID, reset_reason_name(esp_reset_reason()));
  Serial.printf("audio: %u ch @ %u Hz, chunk %u ms, gain shift %u, HPF %.0f Hz | ppg: %u Hz batch %u\n",
                NUM_AUDIO_CHANNELS, AUDIO_SAMPLE_RATE_HZ, CHUNK_DURATION_MS, AUDIO_PCM_GAIN_SHIFT,
                AUDIO_HPF_CUTOFF_HZ, PPG_SAMPLE_RATE_HZ, PPG_BATCH_SIZE);
  Serial.printf("backend ws://%s:%d%s | mqtt %s:%d\n", BACKEND_WS_HOST, BACKEND_WS_PORT, WS_PATH,
                MQTT_BROKER_HOST, MQTT_BROKER_PORT);

  // Task watchdog berlaku untuk task yang mendaftar sendiri (esp_task_wdt_add).
  esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);

  g_ppg_queue = xQueueCreate(PPG_SAMPLE_QUEUE_LENGTH, sizeof(PpgSample));
  bool buffers_ok = buffer_manager_init(NUM_AUDIO_CHANNELS, RING_BUFFER_CAPACITY);
  if (g_ppg_queue == NULL || !buffers_ok) {
    Serial.println("[FATAL] Alokasi antrean PPG / ring buffer audio gagal — restart dalam 5 detik.");
    delay(5000);
    ESP.restart();
  }
  Serial.printf("heap bebas setelah alokasi buffer: %u byte\n", (unsigned)ESP.getFreeHeap());

  xTaskCreatePinnedToCore(task_acquisition, "acquisition", TASK_ACQUISITION_STACK_SIZE, NULL,
                          TASK_ACQUISITION_PRIORITY, NULL, TASK_ACQUISITION_CORE);
  xTaskCreatePinnedToCore(task_network, "network", TASK_NETWORK_STACK_SIZE, NULL,
                          TASK_NETWORK_PRIORITY, NULL, TASK_NETWORK_CORE);
}

void loop() {
  // loopTask Arduino tidak dipakai — seluruh logic ada di 2 task di atas.
  vTaskDelete(NULL);
}
