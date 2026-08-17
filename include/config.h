#pragma once

// Konfigurasi non-sensitif (boleh di-commit). Kredensial & token ada di secrets.h.
// Lihat SDD_HARDWARE.md §7. Nilai di bawah ini placeholder Fase 0 — sesuaikan saat
// modul terkait (network_manager, ws_client, mqtt_client, buffer_manager) diimplementasikan.

#define DEVICE_ID "pneumacare-a1b2"

#define BACKEND_WS_HOST "10.216.222.149"
#define BACKEND_WS_PORT 5000

#define MQTT_BROKER_HOST "10.216.222.149"
#define MQTT_BROKER_PORT 1883

#define HEARTBEAT_INTERVAL_MS 10000

// --- Audio: channel count & buffering (Fase 2, SDD_HARDWARE.md §4) ---
//
// ⚠️ Checkpoint 50%: hanya 1 mikrofon fisik tersedia (keterbatasan budget).
// Kode ring buffer/filter di buffer_manager.cpp & noise_filter.cpp WAJIB
// di-loop berdasarkan NUM_AUDIO_CHANNELS, bukan hardcode index 0/1 — supaya
// menaikkan angka ini ke 4 (setelah Langkah 1b/PoC 4-channel selesai, target
// sebelum final November) tidak perlu menulis ulang modul-modul itu.
#define NUM_AUDIO_CHANNELS 1

// Sample rate akuisisi I2S mentah — dipindah ke sini dari audio_acquisition.cpp
// supaya buffer_manager bisa menghitung kapasitas buffer dari nilai yang sama.
// ⚠️ belum final, lihat SRS_HARDWARE.md FR-HW-003.
#define AUDIO_SAMPLE_RATE_HZ 16000

// Ring buffer per-channel.
//
// ⚠️ CATATAN PENTING: SDD §4 menyebut kapasitas "~1-2 detik data pada sample
// rate native sensor" — itu berlaku untuk ring buffer SAMPLE MENTAH per-sample
// dari driver I2S asli (belum diimplementasikan; audio_acquisition_read() saat
// ini hanya mengembalikan 1 nilai agregat per batch 512-sample, bukan array
// sample individual). RING_BUFFER_CAPACITY_PER_CHANNEL di bawah dihitung dari
// AUDIO_SAMPLE_RATE_HZ langsung (~2 detik sample mentah) TAPI DIKUNCI ke nilai
// yang muat di DRAM ESP32 classic (~320KB total, sebagian besar dipakai
// WiFi/FreeRTOS/heap) untuk harness test Fase 2 saat ini yang menulis 1 nilai
// per batch, bukan 16000 sample/detik.
//
// TODO(Langkah 1b / driver I2S per-sample nyata): saat audio_acquisition
// diperluas untuk mengekspos sample mentah individual (bukan agregat), nilai
// ini HARUS ditinjau ulang — 2 detik penuh @ 16kHz = 32000 sample/channel
// (128KB/channel) tidak akan muat untuk >1 channel sekaligus di DRAM ESP32
// classic. Kemungkinan solusi saat itu: turunkan RING_BUFFER_SECONDS,
// downsample sebelum buffer, atau pindah ke PSRAM (bila board mendukung).
#define RING_BUFFER_SECONDS 2
#define RING_BUFFER_CAPACITY_PER_CHANNEL 2000 // ⚠️ lihat catatan di atas, bukan AUDIO_SAMPLE_RATE_HZ*RING_BUFFER_SECONDS penuh
#define RING_BUFFER_CAPACITY RING_BUFFER_CAPACITY_PER_CHANNEL

// Ukuran chunk yang dikirim ke backend — FR-HW-011: 1-2 detik, bukan 10 detik
// penuh (lihat INTEGRATION_CONTRACT.md §2.2 dan ARCHITECTURE_HARDWARE.md §3).
//
// ⚠️ Sama seperti RING_BUFFER_CAPACITY di atas: CHUNK_DURATION_MS*AUDIO_SAMPLE_RATE_HZ
// adalah target PRODUKSI sebenarnya (1 detik sample mentah = 16000 sample), tapi
// TIDAK dipakai langsung sebagai ukuran buffer baca di harness test Fase 2 —
// buffer tsb hanya berkapasitas RING_BUFFER_CAPACITY (lihat catatan di atas).
// CHUNK_SAMPLE_COUNT_TEST dikunci sama dengan RING_BUFFER_CAPACITY supaya
// harness test tidak mengalokasikan buffer baca yang lebih besar dari yang
// bisa pernah tersedia. Tinjau ulang bersamaan dengan RING_BUFFER_CAPACITY.
#define CHUNK_DURATION_MS 1000
#define CHUNK_SAMPLE_COUNT (AUDIO_SAMPLE_RATE_HZ * CHUNK_DURATION_MS / 1000)
#define CHUNK_SAMPLE_COUNT_TEST RING_BUFFER_CAPACITY

// channel_id 1-based untuk backend (FR-HW-004, PIN_MAPPING_BOM.md §2), terpisah
// dari index array 0-based firmware. Hanya index [0] yang valid & terpakai saat
// NUM_AUDIO_CHANNELS=1 — tiga entri berikutnya siap dipakai begitu channel
// ditambah, TIDAK boleh diasumsikan aktif sebelum PoC 4-channel (Langkah 1b) selesai.
static const uint8_t AUDIO_CHANNEL_ID_MAP[4] = {
    1, // posterior_upper_left
    2, // posterior_upper_right
    3, // posterior_lower_left
    4, // posterior_lower_right
};

// --- Fase 3: Network (SDD_HARDWARE.md §3, INTEGRATION_CONTRACT.md) ---

// Websocket audio — INTEGRATION_CONTRACT.md §2.1/§2.3 (format JSON+pcm_base64,
// FINAL 12 Agt 2026 — binary frame TIDAK diimplementasikan, jangan dikerjakan).
#define WS_PATH "/ws/audio"

// MQTT topik — INTEGRATION_CONTRACT.md §3.2. device_id disisipkan runtime.
#define MQTT_TOPIC_PPG_FMT "pneumacare/%s/ppg/raw"
#define MQTT_TOPIC_STATUS_FMT "pneumacare/%s/status"

// PPG — INTEGRATION_CONTRACT.md §3.3. ⚠️ sample_rate_hz PPG belum final di
// proposal (nilai contoh umum, bukan tervalidasi tim) — sesuaikan bila
// Alfito/datasheet MAX30102 menentukan lain. Batch size dipilih supaya
// publish MQTT tidak terlalu sering (mis. tiap ~1 detik pada 100Hz).
#define PPG_SAMPLE_RATE_HZ 100
#define PPG_BATCH_SIZE 100

// NTP — FR-HW-025 (best-effort, boleh lanjut walau gagal).
#define NTP_SERVER "pool.ntp.org"
#define NTP_SYNC_TIMEOUT_MS 10000

// Reconnect — FR-HW-024, INTEGRATION_CONTRACT.md §2.4: 1s,2s,4s,...cap 30s.
#define RECONNECT_BACKOFF_INITIAL_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 30000
#define RECONNECT_BACKOFF_MULTIPLIER 2

// Autentikasi minimal — INTEGRATION_CONTRACT.md §6 (static API token, bukan
// kelas produksi, cukup untuk demo). Token aktual di secrets.h.
#define WS_AUTH_HEADER_FMT "Authorization: Bearer %s"

// Username MQTT device — sesuai user yang didaftarkan di password_file broker
// (backend/mosquitto/README.md §2.1). Password koneksi MQTT-nya adalah
// DEVICE_API_TOKEN (secrets.h), BUKAN username seperti versi sebelumnya.
#define MQTT_AUTH_USERNAME "pneumacare-device"

// --- Fase 4: Task FreeRTOS dual-core (SDD_HARDWARE.md §2) ---
//
// Core 0 = task "acquisition" (I2S/I2C, prioritas tinggi, timing sensitif).
// Core 1 = task "network" (WS/MQTT/reconnect/heartbeat, prioritas normal,
// boleh blocking sesekali saat reconnect tanpa mengganggu akuisisi sinyal).
#define TASK_ACQUISITION_CORE 0
#define TASK_ACQUISITION_PRIORITY 2
#define TASK_ACQUISITION_STACK_SIZE 4096

#define TASK_NETWORK_CORE 1
#define TASK_NETWORK_PRIORITY 1
#define TASK_NETWORK_STACK_SIZE 8192 // lebih besar: WiFi/WS/MQTT/JSON stack

// Task network tidak punya blocking call natural per-iterasi (beda dengan
// task acquisition yang di-pace oleh i2s_read() blocking) — delay eksplisit
// ini mencegah starvation WiFi/FreeRTOS idle task & watchdog trigger.
#define TASK_NETWORK_LOOP_DELAY_MS 10

// Antrean sample PPG dari task acquisition (Core 0, producer) ke task
// network (Core 1, consumer yang men-dorongnya ke mqtt_client). Kapasitas
// dipilih longgar dibanding PPG_BATCH_SIZE supaya jitter jadwal task tidak
// langsung menyebabkan drop — tapi tetap terbatas (bukan unbounded) sesuai
// prinsip SDD §4 (drop, bukan menahan RAM tanpa batas, bila network macet).
#define PPG_SAMPLE_QUEUE_LENGTH (PPG_BATCH_SIZE * 2)
