#include "audio_pipeline.h"

#include <Arduino.h>
#include <config.h>

#include "audio_acquisition.h"
#include "audio_pcm.h"
#include "audio_route.h"
#include "buffer_manager.h"
#include "channel_health.h"
#include "capture_scheduler.h"
#include "noise_filter.h"

static int32_t g_raw[NUM_AUDIO_CHANNELS][AUDIO_I2S_FRAMES_PER_READ];
static float g_filtered[AUDIO_I2S_FRAMES_PER_READ];
static int16_t g_pcm[AUDIO_I2S_FRAMES_PER_READ];

static ChannelHealth g_health[NUM_AUDIO_CHANNELS];
static AudioChannelStats g_stats[NUM_AUDIO_CHANNELS];

bool audio_pipeline_init() {
  noise_filter_init(AUDIO_SAMPLE_RATE_HZ, AUDIO_HPF_CUTOFF_HZ, AUDIO_LPF_CUTOFF_HZ);

  uint32_t window = (uint32_t)AUDIO_SAMPLE_RATE_HZ * AUDIO_DEAD_CHANNEL_WINDOW_MS / 1000;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    channel_health_init(&g_health[ch], window, AUDIO_DEAD_CHANNEL_P2P_THRESHOLD);
    g_stats[ch] = AudioChannelStats{};
  }

  return audio_acquisition_init();
}

size_t audio_pipeline_process_block() {
  size_t frames = audio_acquisition_read(g_raw);
  if (AUDIO_PERIODIC_CAPTURE) {
    capture_note_source_errors(audio_acquisition_get_dma_overflows(),
                               audio_acquisition_get_read_errors());
  }
  if (frames == 0) return 0;

  size_t retain = AUDIO_PERIODIC_CAPTURE ? capture_take(frames) : frames;
  for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
    ChannelHealth &health = g_health[ch];
    AudioChannelStats &stats = g_stats[ch];

    if (channel_health_update(&health, g_raw[ch], frames)) {
      Serial.printf("[audio] channel_id %u %s (p2p=%ld).\n", AUDIO_CHANNEL_ID_MAP[ch],
                    health.faulty ? "TIDAK ADA SINYAL — mic lepas/rusak, chunk tidak dikirim"
                                  : "sinyal kembali normal",
                    (long)health.last_p2p);
      if (health.faulty && AUDIO_CHANNEL_ID_MAP[ch] != AUDIO_SIMULATED_CHANNEL_ID)
        buffer_manager_clear(ch);
    }

    for (size_t i = 0; i < frames; i++) {
      g_filtered[i] = noise_filter_apply(ch, (float)g_raw[ch][i]);
    }
    size_t clipped = audio_pcm_convert_block(g_filtered, g_pcm, frames, AUDIO_PCM_GAIN_SHIFT);

    int16_t peak = 0;
    for (size_t i = 0; i < frames; i++) {
      int16_t mag = (g_pcm[i] == INT16_MIN) ? INT16_MAX : (int16_t)abs(g_pcm[i]);
      if (mag > peak) peak = mag;
    }

    stats.faulty = health.faulty;
    stats.evaluated = health.evaluated;
    stats.last_p2p = health.last_p2p;
    stats.clip_count += clipped;
    stats.sample_count += frames;
    stats.last_peak = peak;

    if (audio_routed_source(ch, AUDIO_SIMULATED_CHANNEL_ID,
                            AUDIO_SIMULATION_SOURCE_CHANNEL_ID) == ch) {
      audio_route_write(ch, AUDIO_SIMULATED_CHANNEL_ID, AUDIO_SIMULATION_SOURCE_CHANNEL_ID,
                        g_pcm, frames, retain, !(AUDIO_DEAD_CHANNEL_SUPPRESS && health.faulty));
    }
  }

  if (AUDIO_PERIODIC_CAPTURE && retain) capture_commit(retain);
  return frames;
}

void audio_pipeline_get_stats(uint8_t channel_index, AudioChannelStats *out) {
  if (channel_index >= NUM_AUDIO_CHANNELS) {
    *out = AudioChannelStats{};
    return;
  }
  *out = g_stats[channel_index];
}
