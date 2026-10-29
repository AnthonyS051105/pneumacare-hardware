#pragma once
#include <stddef.h>
#include <stdint.h>
#include "buffer_manager.h"

// IDs are 1-based; the normal profile passes simulated_channel=0.
inline uint8_t audio_routed_source(uint8_t channel, uint8_t simulated_channel,
                                   uint8_t source_channel) {
  return channel + 1 == simulated_channel ? source_channel - 1 : channel;
}

// Copy the already filtered PCM into the logical duplicate's existing ring.
// A failed source suppresses both routes; no fabricated silence or extra gain.
inline void audio_route_write(uint8_t source, uint8_t simulated_channel,
                              uint8_t source_channel, const int16_t* pcm,
                              size_t frames, size_t retain, bool available) {
  for (uint8_t destination=0;destination<4;destination++) {
    if(audio_routed_source(destination,simulated_channel,source_channel)!=source) continue;
    if(!available) buffer_manager_skip_samples(destination,frames);
    else {
      if(retain) buffer_manager_write_block(destination,pcm,retain);
      if(frames>retain) buffer_manager_advance_samples(destination,frames-retain);
    }
  }
}
