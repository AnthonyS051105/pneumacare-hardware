#include "buffer_manager.h"

#include <Arduino.h>
#include <config.h>

struct AudioRingBuffer {
  int32_t samples[RING_BUFFER_CAPACITY];
  size_t head;   // index tulis berikutnya
  size_t tail;   // index sample tertua (baca berikutnya)
  size_t count;  // jumlah sample valid saat ini
  uint32_t drop_count;
  portMUX_TYPE mux;
};

static AudioRingBuffer g_channel_buffers[NUM_AUDIO_CHANNELS];

void buffer_manager_init() {
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    g_channel_buffers[ch].head = 0;
    g_channel_buffers[ch].tail = 0;
    g_channel_buffers[ch].count = 0;
    g_channel_buffers[ch].drop_count = 0;
    g_channel_buffers[ch].mux = portMUX_INITIALIZER_UNLOCKED;
  }
}

void buffer_manager_write(uint8_t channel_index, int32_t sample) {
  if (channel_index >= NUM_AUDIO_CHANNELS) return;

  AudioRingBuffer &buf = g_channel_buffers[channel_index];

  portENTER_CRITICAL(&buf.mux);

  if (buf.count == RING_BUFFER_CAPACITY) {
    // Buffer penuh: drop sample TERLAMA (SDD §4) untuk beri ruang sample baru.
    buf.tail = (buf.tail + 1) % RING_BUFFER_CAPACITY;
    buf.count--;
    buf.drop_count++;
  }

  buf.samples[buf.head] = sample;
  buf.head = (buf.head + 1) % RING_BUFFER_CAPACITY;
  buf.count++;

  portEXIT_CRITICAL(&buf.mux);
}

size_t buffer_manager_available(uint8_t channel_index) {
  if (channel_index >= NUM_AUDIO_CHANNELS) return 0;

  AudioRingBuffer &buf = g_channel_buffers[channel_index];
  portENTER_CRITICAL(&buf.mux);
  size_t count = buf.count;
  portEXIT_CRITICAL(&buf.mux);
  return count;
}

size_t buffer_manager_read_chunk(uint8_t channel_index, int32_t *out_buf,
                                  size_t max_samples) {
  if (channel_index >= NUM_AUDIO_CHANNELS) return 0;

  AudioRingBuffer &buf = g_channel_buffers[channel_index];

  portENTER_CRITICAL(&buf.mux);

  size_t to_read = (buf.count < max_samples) ? buf.count : max_samples;
  for (size_t i = 0; i < to_read; i++) {
    out_buf[i] = buf.samples[buf.tail];
    buf.tail = (buf.tail + 1) % RING_BUFFER_CAPACITY;
  }
  buf.count -= to_read;

  portEXIT_CRITICAL(&buf.mux);

  return to_read;
}

uint32_t buffer_manager_get_drop_count(uint8_t channel_index) {
  if (channel_index >= NUM_AUDIO_CHANNELS) return 0;

  AudioRingBuffer &buf = g_channel_buffers[channel_index];
  portENTER_CRITICAL(&buf.mux);
  uint32_t drops = buf.drop_count;
  portEXIT_CRITICAL(&buf.mux);
  return drops;
}
