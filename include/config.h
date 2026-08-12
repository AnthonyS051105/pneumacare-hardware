#pragma once

// Konfigurasi non-sensitif (boleh di-commit). Kredensial & token ada di secrets.h.
// Lihat SDD_HARDWARE.md §7. Nilai di bawah ini placeholder Fase 0 — sesuaikan saat
// modul terkait (network_manager, ws_client, mqtt_client, buffer_manager) diimplementasikan.

#define DEVICE_ID "pneumacare-a1b2"

#define BACKEND_WS_HOST "192.168.1.100"
#define BACKEND_WS_PORT 5000

#define MQTT_BROKER_HOST "192.168.1.100"
#define MQTT_BROKER_PORT 1883

#define HEARTBEAT_INTERVAL_MS 10000
