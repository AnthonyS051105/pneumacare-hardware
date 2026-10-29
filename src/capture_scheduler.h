#pragma once
#include <stddef.h>
#include <stdint.h>
enum CaptureState { CAPTURE_IDLE, CAPTURE_RECORDING, CAPTURE_READY, CAPTURE_WAIT_ACK };
enum CaptureSourceFault { CAPTURE_SOURCE_OK, CAPTURE_SOURCE_DMA_GAP, CAPTURE_SOURCE_I2S_GAP };
struct CaptureSnapshot {
  CaptureState state;
  uint32_t id, start_ms, next_ms;
  uint64_t epoch_ms;
  size_t retained, target;
  bool in_block;
  CaptureSourceFault source_fault;
};
// Called by acquisition after every I2S read, including failed reads.
void capture_note_source_errors(uint32_t dma_overflows, uint32_t read_errors);
void capture_init(size_t samples, uint32_t interval_ms, uint32_t timeout_ms);
bool capture_begin(uint32_t now_ms, uint64_t epoch_ms);
size_t capture_take(size_t frames);
void capture_commit(size_t samples);
CaptureSnapshot capture_snapshot();
void capture_mark_sent();
bool capture_ack(uint32_t id, uint32_t now_ms);
void capture_abort(uint32_t now_ms);
bool capture_expired(uint32_t now_ms);
