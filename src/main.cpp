#include <Arduino.h>
#include <config.h>

#include "audio_acquisition.h"
#include "noise_filter.h"
#include "buffer_manager.h"
#include "ppg_acquisition.h"
#include "network_manager.h"
#include "ws_client.h"
#include "mqtt_client.h"

// =====================================================================
// Fase 3 — Network: WiFi + NTP, ws_client (audio), mqtt_client (PPG +
// heartbeat), reconnect otomatis. Test terhadap simulator backend dari
// repo pneumacare-software (skrip Python "ESP32 palsu" Fase 1 software) —
// lihat instruksi test terpisah, bukan menunggu backend Flask penuh siap.
//
// NUM_AUDIO_CHANNELS=1 (checkpoint 50%, config.h) — ws_client tetap
// di-loop berdasarkan NUM_AUDIO_CHANNELS (bukan hardcode channel_id=1),
// jadi menaikkan ke 4 nanti tidak butuh perubahan di ws_client.cpp.
// =====================================================================

#define STATUS_PRINT_INTERVAL_MS 1000

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== Fase 3: network_manager + ws_client + mqtt_client ===");

  // SDD_HARDWARE.md §3: BOOT -> WIFI_CONNECTING -> NTP_SYNC -> SENSOR_INIT -> STREAMING
  network_manager_init(); // blocking sampai WiFi tersambung; NTP best-effort

  audio_acquisition_init();
  noise_filter_init();
  buffer_manager_init();

  if (!ppg_acquisition_init()) {
    Serial.println("[FATAL] Inisialisasi PPG gagal, cek wiring I2C (SDA/SCL/VDD/GND).");
    while (true) delay(1000);
  }

  ws_client_init();
  mqtt_client_init();

  Serial.println("Semua modul siap. Mulai streaming...");
}

void loop() {
  static uint32_t last_status_ms = 0;
  static uint32_t last_heartbeat_ms = 0;

  // --- "Task acquisition" (nanti Core 0): audio -> filter -> ring buffer ---
  AudioReadResult r = audio_acquisition_read();
  if (r.samples_read > 0) {
    int32_t filtered = noise_filter_apply(0, r.avg_abs);
    buffer_manager_write(0, filtered);
  }

  // --- PPG: baca sample baru, dorong ke batch mqtt_client ---
  PpgReadResult ppg = ppg_acquisition_read();
  if (ppg.has_sample) {
    mqtt_client_push_ppg_sample(ppg.ir);
  }

  uint32_t now = millis();

  // --- "Task network" (nanti Core 1): reconnect, kirim chunk, heartbeat ---
  network_manager_reconnect_tick();
  ws_client_reconnect_tick();
  mqtt_client_reconnect_tick();

  ws_client_tick();
  mqtt_client_tick();

  if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
    last_heartbeat_ms = now;
    mqtt_client_publish_heartbeat();
  }

  if (now - last_status_ms >= STATUS_PRINT_INTERVAL_MS) {
    last_status_ms = now;
    Serial.printf("[status] wifi=%d ws=%d mqtt=%d ch0_available=%u/%u drop_count=%lu\n",
                  network_manager_is_connected(), ws_client_is_connected(),
                  mqtt_client_is_connected(), (unsigned)buffer_manager_available(0),
                  (unsigned)RING_BUFFER_CAPACITY,
                  (unsigned long)buffer_manager_get_drop_count(0));
  }
}
