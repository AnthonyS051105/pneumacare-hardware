#pragma once
#include <stddef.h>
#include <stdint.h>

// Single-owner bookkeeping for one four-channel recording. No PCM allocation.
// A gap or a change in ANY ring drop counter invalidates the entire recording.
class CaptureDelivery {
 public:
  void begin(uint32_t id, size_t target, const uint32_t drops[4]);
  bool overflowed(const uint32_t drops[4]) const;
  bool consume(uint8_t channel, uint64_t sample_start, size_t count);
  size_t sent(uint8_t channel) const { return channel < 4 ? sent_[channel] : 0; }
  bool complete() const;
 private:
  uint32_t id_ = 0;
  size_t target_ = 0, sent_[4] = {};
  uint64_t next_sample_[4] = {};
  uint32_t initial_drops_[4] = {};
};
