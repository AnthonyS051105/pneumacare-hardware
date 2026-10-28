#include "buffer_manager.h"

#include <stdlib.h>
#include <string.h>

#include "platform_lock.h"

#define BUFFER_MANAGER_MAX_CHANNELS 4

struct AudioRingBuffer {
  int16_t *samples;
  size_t head;  // index tulis berikutnya
  size_t tail;  // index sample tertua (baca berikutnya)
  size_t count; // jumlah sample valid saat ini
  uint32_t drop_count;
  uint64_t samples_written; // timeline per channel; tidak direset saat clear
  platform_lock_t lock;
};

static AudioRingBuffer g_buffers[BUFFER_MANAGER_MAX_CHANNELS];
static uint8_t g_num_channels = 0;
static size_t g_capacity = 0;

void buffer_manager_deinit() {
  for (uint8_t ch = 0; ch < BUFFER_MANAGER_MAX_CHANNELS; ch++) {
    free(g_buffers[ch].samples);
    g_buffers[ch].samples = NULL;
  }
  g_num_channels = 0;
  g_capacity = 0;
}

bool buffer_manager_init(uint8_t num_channels, size_t capacity) {
  buffer_manager_deinit();
  if (num_channels == 0 || num_channels > BUFFER_MANAGER_MAX_CHANNELS || capacity == 0) {
    return false;
  }

  for (uint8_t ch = 0; ch < num_channels; ch++) {
    AudioRingBuffer &buf = g_buffers[ch];
    buf.samples = (int16_t *)malloc(capacity * sizeof(int16_t));
    if (buf.samples == NULL) {
      buffer_manager_deinit();
      return false;
    }
    buf.head = 0;
    buf.tail = 0;
    buf.count = 0;
    buf.drop_count = 0;
    buf.samples_written = 0;
    buf.lock = PLATFORM_LOCK_INITIALIZER;
  }

  g_num_channels = num_channels;
  g_capacity = capacity;
  return true;
}

size_t buffer_manager_capacity() {
  return g_capacity;
}

void buffer_manager_write_block(uint8_t channel_index, const int16_t *samples, size_t n) {
  if (channel_index >= g_num_channels || n == 0) return;

  AudioRingBuffer &buf = g_buffers[channel_index];

  // Blok lebih besar dari kapasitas: hanya bagian terbaru yang bisa disimpan.
  size_t skipped_input = 0;
  if (n > g_capacity) {
    skipped_input = n - g_capacity;
    samples += skipped_input;
    n = g_capacity;
  }

  platform_lock_enter(&buf.lock);

  buf.samples_written += n + skipped_input;
  size_t free_space = g_capacity - buf.count;
  if (n > free_space) {
    // Drop sample TERLAMA (SDD §4) untuk memberi ruang blok baru.
    size_t to_drop = n - free_space;
    buf.tail = (buf.tail + to_drop) % g_capacity;
    buf.count -= to_drop;
    buf.drop_count += (uint32_t)to_drop;
  }
  buf.drop_count += (uint32_t)skipped_input;

  // Salin dalam maksimal 2 segmen (sebelum & sesudah wrap-around).
  size_t first = g_capacity - buf.head;
  if (first > n) first = n;
  memcpy(&buf.samples[buf.head], samples, first * sizeof(int16_t));
  if (n > first) {
    memcpy(&buf.samples[0], samples + first, (n - first) * sizeof(int16_t));
  }
  buf.head = (buf.head + n) % g_capacity;
  buf.count += n;

  platform_lock_exit(&buf.lock);
}

size_t buffer_manager_available(uint8_t channel_index) {
  if (channel_index >= g_num_channels) return 0;

  AudioRingBuffer &buf = g_buffers[channel_index];
  platform_lock_enter(&buf.lock);
  size_t count = buf.count;
  platform_lock_exit(&buf.lock);
  return count;
}

size_t buffer_manager_read_chunk(uint8_t channel_index, int16_t *out_buf, size_t max_samples,
                                 uint64_t *sample_start) {
  if (channel_index >= g_num_channels) return 0;

  AudioRingBuffer &buf = g_buffers[channel_index];

  platform_lock_enter(&buf.lock);

  if (sample_start != NULL) *sample_start = buf.samples_written - buf.count;
  size_t to_read = (buf.count < max_samples) ? buf.count : max_samples;
  size_t first = g_capacity - buf.tail;
  if (first > to_read) first = to_read;
  memcpy(out_buf, &buf.samples[buf.tail], first * sizeof(int16_t));
  if (to_read > first) {
    memcpy(out_buf + first, &buf.samples[0], (to_read - first) * sizeof(int16_t));
  }
  buf.tail = (buf.tail + to_read) % g_capacity;
  buf.count -= to_read;

  platform_lock_exit(&buf.lock);

  return to_read;
}

void buffer_manager_clear(uint8_t channel_index) {
  if (channel_index >= g_num_channels) return;

  AudioRingBuffer &buf = g_buffers[channel_index];
  platform_lock_enter(&buf.lock);
  buf.head = 0;
  buf.tail = 0;
  buf.count = 0;
  platform_lock_exit(&buf.lock);
}

uint32_t buffer_manager_get_drop_count(uint8_t channel_index) {
  if (channel_index >= g_num_channels) return 0;

  AudioRingBuffer &buf = g_buffers[channel_index];
  platform_lock_enter(&buf.lock);
  uint32_t drops = buf.drop_count;
  platform_lock_exit(&buf.lock);
  return drops;
}

void buffer_manager_skip_samples(uint8_t channel_index, size_t n) {
  if (channel_index >= g_num_channels) return;
  AudioRingBuffer &buf = g_buffers[channel_index];
  platform_lock_enter(&buf.lock);
  buf.samples_written += n;
  buf.head = buf.tail = buf.count = 0;
  platform_lock_exit(&buf.lock);
}
