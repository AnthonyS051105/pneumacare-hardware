#include <Arduino.h>

#include "audio_acquisition.h"
#include "ppg_acquisition.h"
#include "noise_filter.h"
#include "buffer_manager.h"
#include "network_manager.h"
#include "ws_client.h"
#include "mqtt_client.h"
#include "reconnect_manager.h"

void setup() {
  Serial.begin(115200);

  audio_acquisition_init();
  ppg_acquisition_init();
  noise_filter_init();
  buffer_manager_init();
  network_manager_init();
  ws_client_init();
  mqtt_client_init();
  reconnect_manager_init();
}

void loop() {
  // TODO(Fase 4): task orchestration FreeRTOS, lihat SDD_HARDWARE.md §2.
}
