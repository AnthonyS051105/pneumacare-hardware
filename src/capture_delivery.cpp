#include "capture_delivery.h"

void CaptureDelivery::begin(uint32_t id, size_t target, const uint32_t drops[4]) {
  id_ = id; target_ = target;
  for (int ch = 0; ch < 4; ch++) {
    sent_[ch] = 0; next_sample_[ch] = 0; initial_drops_[ch] = drops[ch];
  }
}
bool CaptureDelivery::overflowed(const uint32_t drops[4]) const {
  for (int ch = 0; ch < 4; ch++) if (drops[ch] != initial_drops_[ch]) return true;
  return false;
}
bool CaptureDelivery::consume(uint8_t ch, uint64_t start, size_t n) {
  if (!id_ || ch >= 4 || !n || n > target_ - sent_[ch] ||
      (sent_[ch] && start != next_sample_[ch])) return false;
  sent_[ch] += n; next_sample_[ch] = start + n;
  return true;
}
bool CaptureDelivery::complete() const {
  if (!id_ || !target_) return false;
  for (int ch = 0; ch < 4; ch++) if (sent_[ch] != target_) return false;
  return true;
}
