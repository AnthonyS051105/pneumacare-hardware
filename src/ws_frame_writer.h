#pragma once
#include <stddef.h>
#include <stdint.h>

enum FrameWriteStatus { FRAME_IDLE, FRAME_PENDING, FRAME_COMPLETE, FRAME_FAILED };
// Callback: positive byte count, zero would-block, negative fatal error.
using FrameWrite = int (*)(const uint8_t *,size_t,void *);
class WsFrameWriter {
 public:
  // Caller reserves 14 bytes before payload, owns buffer until completion/reset.
  bool begin(uint8_t *buffer,size_t payload_bytes,uint32_t mask,uint8_t opcode,
             uint32_t now_ms,uint32_t timeout_ms);
  FrameWriteStatus pump(FrameWrite write,void *context,uint32_t now_ms);
  bool pending() const { return data_ != nullptr; }
  void reset() { data_=nullptr; }
 private:
  uint8_t *data_ = nullptr;
  size_t size_ = 0, offset_ = 0;
  uint32_t start_ms_ = 0, timeout_ms_ = 0;
};
