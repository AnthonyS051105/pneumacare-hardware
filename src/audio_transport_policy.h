#pragma once
#include "capture_scheduler.h"
// Never insert control bytes into a partial frame. During an active recording,
// defer the library's blocking control reader until all audio bytes are sent.
// The <=30s window deadline and <=500ms frame deadline still bound a stalled link.
inline bool ws_may_poll(bool pending, bool connected, CaptureState state) {
  return !pending && (!connected || state == CAPTURE_IDLE || state == CAPTURE_WAIT_ACK);
}
