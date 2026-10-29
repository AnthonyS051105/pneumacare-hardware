#include "capture_scheduler.h"
#include "platform_lock.h"
static platform_lock_t g_lock = PLATFORM_LOCK_INITIALIZER;
static CaptureSnapshot g;
static uint32_t g_interval, g_timeout;
static uint32_t g_last_dma, g_last_read_errors;
static const uint32_t kMinimumRestMs = 1000; // operational pause after ACK/abort
void capture_note_source_errors(uint32_t dma, uint32_t errors) {
  platform_lock_enter(&g_lock);
  if(g.state==CAPTURE_RECORDING && g.source_fault==CAPTURE_SOURCE_OK) {
    if(dma!=g_last_dma)g.source_fault=CAPTURE_SOURCE_DMA_GAP;
    else if(errors!=g_last_read_errors)g.source_fault=CAPTURE_SOURCE_I2S_GAP;
  }
  g_last_dma=dma;g_last_read_errors=errors;
  platform_lock_exit(&g_lock);
}
void capture_init(size_t samples, uint32_t interval_ms, uint32_t timeout_ms) {
  platform_lock_enter(&g_lock);
  g = {};g_last_dma=0;g_last_read_errors=0;g.target=samples;g_interval=interval_ms;g_timeout=timeout_ms;
  platform_lock_exit(&g_lock);
}
CaptureSnapshot capture_snapshot() {
  platform_lock_enter(&g_lock);CaptureSnapshot s=g;platform_lock_exit(&g_lock);return s;
}
bool capture_begin(uint32_t now, uint64_t epoch) {
  platform_lock_enter(&g_lock);
  bool ok=g.state==CAPTURE_IDLE && !g.in_block && (int32_t)(now-g.next_ms)>=0;
  if(ok){g.state=CAPTURE_RECORDING;g.source_fault=CAPTURE_SOURCE_OK;g.id++;g.retained=0;g.start_ms=now;g.epoch_ms=epoch;}
  platform_lock_exit(&g_lock);return ok;
}
size_t capture_take(size_t frames) {
  platform_lock_enter(&g_lock);
  size_t n=0;
  if(g.state==CAPTURE_RECORDING && g.source_fault==CAPTURE_SOURCE_OK && !g.in_block){n=g.target-g.retained;if(n>frames)n=frames;g.in_block=true;}
  platform_lock_exit(&g_lock);return n;
}
void capture_commit(size_t n) {
  platform_lock_enter(&g_lock);
  if(g.state==CAPTURE_RECORDING && g.source_fault==CAPTURE_SOURCE_OK){g.retained+=n;if(g.retained==g.target)g.state=CAPTURE_READY;}
  g.in_block=false;platform_lock_exit(&g_lock);
}
void capture_mark_sent() {
  platform_lock_enter(&g_lock);if(g.state==CAPTURE_READY)g.state=CAPTURE_WAIT_ACK;platform_lock_exit(&g_lock);
}
static void rest(uint32_t now) {
  g.state=CAPTURE_IDLE;
  uint32_t scheduled=g.start_ms+g_interval;
  g.next_ms=(int32_t)(scheduled-(now+kMinimumRestMs))>0?scheduled:now+kMinimumRestMs;
}
bool capture_ack(uint32_t id,uint32_t now) {
  platform_lock_enter(&g_lock);bool ok=g.state==CAPTURE_WAIT_ACK&&g.id==id;
  if (ok) rest(now);
  platform_lock_exit(&g_lock);
  return ok;
}
void capture_abort(uint32_t now) {
  platform_lock_enter(&g_lock);rest(now);platform_lock_exit(&g_lock);
}
bool capture_expired(uint32_t now) {
  auto s=capture_snapshot();return s.state!=CAPTURE_IDLE && now-s.start_ms>g_timeout;
}
