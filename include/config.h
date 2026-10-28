#pragma once

#include <stdint.h>

// Konfigurasi non-sensitif (boleh di-commit). Kredensial & token ada di secrets.h.
// Lihat SDD_HARDWARE.md §7. Semua angka yang belum final (⚠️ di SRS/SDD) WAJIB
// didefinisikan di sini dengan nama jelas, bukan magic number di modul lain.

// =====================================================================
// Identitas & endpoint backend — INTEGRATION_CONTRACT.md §1, §2.1, §3.1
// =====================================================================

// Harus sama persis dengan HARDWARE_DEVICE_ID di
// software/backend/scripts/seed_hardware_demo.py — skrip itu khusus
// menautkan device hardware ASLI (bukan device dummy seed_dev_patient.py)
// ke akun pasien+clinician demo. Kalau beda, data tetap tersimpan (upsert
// otomatis di device_registry.py) tapi TIDAK tertaut ke pasien manapun,
// sehingga tidak akan muncul di dashboard.
#define DEVICE_ID "pneumacare-a1b2"

// device_id KHUSUS program diagnostik/test fase (src/diag/). Sengaja beda dari
// DEVICE_ID supaya data sintetis (nada uji, PPG palsu) tidak pernah tertaut ke
// pasien demo dan tidak memicu klasifikasi/alert palsu di dashboard.
#define DIAG_DEVICE_ID "pneumacare-0000"

#define BACKEND_WS_HOST "172.30.2.149"
#define BACKEND_WS_PORT 5000

#define MQTT_BROKER_HOST "172.30.2.149"
#define MQTT_BROKER_PORT 1883

// =====================================================================
// Pin mapping — PIN_MAPPING_BOM.md §1. ⚠️ Pin bertanda USULAN belum
// dikonfirmasi skematik final Alfito; ubah di sini DAN di PIN_MAPPING_BOM.md.
// =====================================================================

// Wiring Alfito dikonfirmasi melalui foto pinout, 2026-10-27.
// Dua bus stereo dengan clock terpisah; masing-masing I2S adalah master RX.
#define PIN_I2S_SCK_1 26
#define PIN_I2S_WS_1 25
#define PIN_I2S_SD_1 33 // mic 1: L/R=GND; mic 2: L/R=3V3
#define PIN_I2S_SCK_2 14
#define PIN_I2S_WS_2 27
#define PIN_I2S_SD_2 32 // mic 3: L/R=GND; mic 4: L/R=3V3

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
// INT terhubung opsional; driver MAX30102 tetap memakai polling FIFO.
#define PIN_PPG_INT 19

// ⚠️ USULAN firmware: ADC1 (tetap bisa dibaca saat WiFi aktif, beda dengan ADC2).
#define PIN_BATTERY_SENSE 35
// LED onboard ESP32 DevKit — aman dipakai sebagai indikator status lokal.
#define PIN_STATUS_LED 2

// =====================================================================
// Audio — FR-HW-001/003/010/011, SDD_HARDWARE.md §4-§5
// =====================================================================

// Jumlah channel aktif (1..4). 4 = konfigurasi final (FR-HW-001). Channel yang
// mic-nya tidak terpasang otomatis terdeteksi mati dan TIDAK dikirim (lihat
// AUDIO_DEAD_CHANNEL_*), jadi nilai 4 tetap aman dipakai walau mic belum lengkap.
#define NUM_AUDIO_CHANNELS 4

// Sample rate akuisisi mentah (native INMP441, didukung 7.8–50 kHz per datasheet).
// Backend resample sendiri ke 22000 Hz (INTEGRATION_CONTRACT.md §4.1).
#define AUDIO_SAMPLE_RATE_HZ 16000

// Sumber tiap channel: unit I2S (0 = I2S0/SD_1, 1 = I2S1/SD_2) dan slot di
// dalam frame stereo (0 = word pertama, 1 = word kedua). Index array = channel
// index firmware (0-based); channel_id backend ada di AUDIO_CHANNEL_ID_MAP.
// ⚠️ Urutan slot L/R di DMA ESP32 harus diverifikasi dengan tap test
// (env diag_mic) — bila ketukan mic 1 muncul di channel 2, tukar slot di sini.
struct AudioChannelSource {
  uint8_t i2s_unit;
  uint8_t slot;
};
static const AudioChannelSource AUDIO_CHANNEL_SOURCES[4] = {
    {0, 0}, // mic #1: SD_1, L/R pin = GND
    {0, 1}, // mic #2: SD_1, L/R pin = VDD
    {1, 0}, // mic #3: SD_2, L/R pin = GND
    {1, 1}, // mic #4: SD_2, L/R pin = VDD
};

// channel_id 1-based untuk backend (FR-HW-004, PIN_MAPPING_BOM.md §2).
static const uint8_t AUDIO_CHANNEL_ID_MAP[4] = {
    1, // posterior_upper_left
    2, // posterior_upper_right
    3, // posterior_lower_left
    4, // posterior_lower_right
};

// Frame per pembacaan I2S per unit (1 frame = 2 slot x 32-bit). 256 frame =
// 16 ms pada 16 kHz — juga menentukan seberapa sering PPG di-poll.
#define AUDIO_I2S_FRAMES_PER_READ 256
// DMA: 6 x 256 frame = 96 ms headroom bila task acquisition tertunda (mis.
// di-preempt task WiFi di Core 0) sebelum sample I2S mulai hilang.
#define AUDIO_I2S_DMA_BUF_COUNT 6
#define AUDIO_I2S_DMA_BUF_LEN 256
// Batas tunggu i2s_read() — bila terlampaui, clock I2S dianggap mati.
#define AUDIO_I2S_READ_TIMEOUT_MS 200
// INMP441 butuh ~85 ms startup setelah clock aktif (datasheet) + transien
// filter; sample awal dibuang supaya tidak terkirim sebagai "klik" palsu.
#define AUDIO_STARTUP_DISCARD_MS 250

// Filter (FR-HW-010): high-pass Butterworth orde 2 untuk buang DC offset,
// rumble gerakan & dengung frekuensi sangat rendah. 50 Hz dipilih karena
// suara napas/adventitious berada ~100-2500 Hz; ⚠️ tuning lanjutan butuh data
// nyata. Low-pass opsional (0 = nonaktif): sengaja dimatikan supaya spektrum
// yang diterima model tidak berbeda dari data training tanpa keputusan tim.
#define AUDIO_HPF_CUTOFF_HZ 50.0f
#define AUDIO_LPF_CUTOFF_HZ 0.0f

// Konversi 24-bit (INMP441) -> int16 untuk transmisi: pcm16 = sample24 >> shift
// (dengan saturasi). 8 = 16 bit teratas (tanpa gain digital), 0 = gain +48 dB
// (perilaku firmware lama, mudah clipping saat batuk/gesekan vest). 2 = +36 dB:
// noise floor mic tetap jauh di atas resolusi int16, clipping baru terjadi di
// ~82 dB SPL. ⚠️ Tuning dengan env diag_mic (lihat persen clipping & dBFS).
#define AUDIO_PCM_GAIN_SHIFT 2

// Ukuran chunk per pesan websocket (FR-HW-011). 250 ms: 4000 sample x 2 byte =
// 8 KB PCM (~11 KB JSON+base64) per pesan — buffer kirim kecil & kontigu.
// Backend menyusun ulang jadi segmen 5 detik dari akumulasi chunk_duration_ms.
#define CHUNK_DURATION_MS 250
#define CHUNK_SAMPLE_COUNT (AUDIO_SAMPLE_RATE_HZ * CHUNK_DURATION_MS / 1000)

// Ring buffer per channel (int16, dialokasikan di heap saat init) = 2 chunk
// (500 ms) -> 4 x 16 KB = 64 KB total. Menyerap jitter jaringan sesaat; bila
// backlog melebihi ini, sample TERLAMA di-drop (SDD §4).
#define RING_BUFFER_CHUNKS 2
#define RING_BUFFER_CAPACITY (CHUNK_SAMPLE_COUNT * RING_BUFFER_CHUNKS)

// Deteksi channel mati/macet: bila puncak-ke-puncak sample 24-bit mentah dalam
// satu jendela < ambang, channel ditandai FAULT dan chunk-nya TIDAK dikirim —
// mencegah backend mengklasifikasikan keheningan mic lepas sebagai "none"
// (rasa aman palsu). Noise floor INMP441 asli ~ratusan LSB, jauh di atas ambang.
#define AUDIO_DEAD_CHANNEL_WINDOW_MS 1000
#define AUDIO_DEAD_CHANNEL_P2P_THRESHOLD 16
#define AUDIO_DEAD_CHANNEL_SUPPRESS 1

// =====================================================================
// PPG MAX30102 — FR-HW-002, INTEGRATION_CONTRACT.md §3.3, PIN_MAPPING_BOM.md §1.2
// =====================================================================

#define PPG_I2C_ADDRESS 0x57
#define PPG_I2C_CLOCK_HZ 400000

// Sample rate internal 400 SPS dirata-rata 4 -> output FIFO 100 Hz.
// ⚠️ 100 Hz masih nilai contoh kontrak §3.3 (belum divalidasi tim).
#define PPG_INTERNAL_SAMPLE_RATE_SPS 400
#define PPG_SAMPLE_AVERAGING 4
#define PPG_SAMPLE_RATE_HZ (PPG_INTERNAL_SAMPLE_RATE_SPS / PPG_SAMPLE_AVERAGING)
#define PPG_PULSE_WIDTH_US 411 // resolusi ADC 18-bit
#define PPG_ADC_RANGE_NA 4096
// Arus LED (register LEDx_PA, 0.2 mA/LSB). 0x1F = 6.2 mA. ⚠️ Posisi dada punya
// perfusi rendah — naikkan bila sinyal AC terlalu kecil, turunkan bila
// saturasi (cek dengan env diag_ppg).
#define PPG_LED_RED_CURRENT 0x1F
#define PPG_LED_IR_CURRENT 0x1F

// Sensor dianggap hilang setelah sekian error I2C berturut-turut, lalu
// inisialisasi ulang dicoba berkala tanpa menghentikan akuisisi audio.
#define PPG_MAX_CONSECUTIVE_I2C_ERRORS 5
#define PPG_REINIT_INTERVAL_MS 5000

// Batch publish MQTT: 100 sample = ~1 detik pada 100 Hz.
#define PPG_BATCH_SIZE 100
// Antrean sample dari Core 0 ke Core 1 — 5 detik headroom (4 KB), tetap
// terbatas. Harus melebihi operasi blocking terlama task network (connect
// MQTT: TCP 3 s + CONNACK) supaya PPG tidak terbuang saat reconnect.
#define PPG_SAMPLE_QUEUE_LENGTH (PPG_SAMPLE_RATE_HZ * 5)

// =====================================================================
// Network — FR-HW-020..025, INTEGRATION_CONTRACT.md §2-§3, §6
// =====================================================================

#define WS_PATH "/ws/audio"
#define WS_AUTH_HEADER_FMT "Authorization: Bearer %s"
// Ping/pong websocket untuk mendeteksi koneksi setengah-mati (half-open TCP).
#define WS_HEARTBEAT_PING_INTERVAL_MS 15000
#define WS_HEARTBEAT_PONG_TIMEOUT_MS 5000
#define WS_HEARTBEAT_MAX_MISSED 2
// Maks chunk dikirim per channel per iterasi task network (mengejar backlog
// tanpa memonopoli task terlalu lama).
#define WS_MAX_CHUNKS_PER_CHANNEL_PER_TICK 2

#define MQTT_TOPIC_PPG_FMT "pneumacare/%s/ppg/raw"
#define MQTT_TOPIC_STATUS_FMT "pneumacare/%s/status"
// Username MQTT device — sesuai password_file broker (backend/mosquitto/README.md
// §2.1). Password-nya DEVICE_API_TOKEN (secrets.h).
#define MQTT_AUTH_USERNAME "pneumacare-device"
// PPG_BATCH_SIZE=100 x 2 array uint32 (≤6 digit) ≈ 1.6 KB JSON.
#define MQTT_BUFFER_SIZE 2048
#define MQTT_KEEPALIVE_S 15
#define MQTT_SOCKET_TIMEOUT_S 5

// FR-HW-023: diusulkan 10 detik; backend menandai offline setelah 30 detik.
#define HEARTBEAT_INTERVAL_MS 10000

#define NTP_SERVER "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define NTP_SYNC_TIMEOUT_MS 10000
// Epoch di bawah ini = jam belum tersinkron (default ESP32 mulai dari 1970).
#define NTP_VALID_EPOCH_S 1700000000UL

// FR-HW-024, INTEGRATION_CONTRACT.md §2.4: 1s, 2s, 4s, ... cap 30s.
#define RECONNECT_BACKOFF_INITIAL_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 30000
#define RECONNECT_BACKOFF_MULTIPLIER 2
// WiFi butuh ~2-8 detik untuk asosiasi + DHCP; percobaan ulang lebih cepat dari
// ini justru memutus percobaan yang sedang berjalan.
#define WIFI_RECONNECT_BACKOFF_INITIAL_MS 10000

// Modem sleep WiFi dimatikan: streaming audio ~1.4 Mbps kontinu jauh lebih
// stabil tanpa power-save (latensi DTIM). ⚠️ Menaikkan konsumsi daya —
// evaluasi ulang saat uji baterai (Fase 6, NFR-HW-004).
#define WIFI_DISABLE_POWER_SAVE 1

// =====================================================================
// Baterai — FR-HW-030 (opsional). Nonaktif sampai rangkaian pembagi tegangan
// dikonfirmasi Alfito; saat nonaktif payload status mengirim battery_pct = -1
// (sudah dinormalisasi backend menjadi "tidak diketahui").
// =====================================================================

#define BATTERY_SENSE_ENABLED 0
// Rasio pembagi tegangan (Vbat / Vadc). 2.0 = resistor 100k/100k.
#define BATTERY_DIVIDER_RATIO 2.0f
#define BATTERY_ADC_SAMPLES 16

// =====================================================================
// Task FreeRTOS dual-core — SDD_HARDWARE.md §2
// =====================================================================

#define TASK_ACQUISITION_CORE 0
#define TASK_ACQUISITION_PRIORITY 5
#define TASK_ACQUISITION_STACK_SIZE 6144

#define TASK_NETWORK_CORE 1
#define TASK_NETWORK_PRIORITY 2
#define TASK_NETWORK_STACK_SIZE 10240
#define TASK_NETWORK_LOOP_DELAY_MS 5

// =====================================================================
// Keandalan & diagnostik — NFR-HW-001, FR-HW-031
// =====================================================================

// Task watchdog: reboot otomatis bila salah satu task macet lebih dari ini
// (operasi jaringan blocking terlama ~5 detik, jadi 30 detik aman).
#define TASK_WDT_TIMEOUT_S 30
// Restart bila WiFi terputus terus selama ini (stack WiFi kadang macet dan
// hanya pulih lewat reboot). 0 = nonaktif.
#define WIFI_LOST_RESTART_MS (5UL * 60UL * 1000UL)
// Restart bila heap bebas di bawah ini (mencegah crash acak akibat fragmentasi).
#define HEAP_CRITICAL_RESTART_BYTES 16384

#define STATUS_LOG_INTERVAL_MS 5000
