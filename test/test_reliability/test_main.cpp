// Exponential backoff (FR-HW-024, INTEGRATION_CONTRACT.md §2.4) dan estimasi
// level baterai (FR-HW-030).

#include <unity.h>

#include <config.h>

#include "battery_curve.h"
#include "audio_transport_policy.h"
#include "reconnect_manager.h"

void setUp() {}
void tearDown() {}

void test_backoff_sequence_doubles_until_cap() {
  ReconnectBackoff b;
  reconnect_backoff_init(&b, 1000, 30000);
  const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
  uint32_t now = 100000;
  for (uint32_t delay : expected) {
    TEST_ASSERT_EQUAL_UINT32(delay, b.current_delay_ms);
    reconnect_backoff_notify_attempt(&b, now);
    now += delay;
  }
}

void test_backoff_waits_for_delay() {
  ReconnectBackoff b;
  reconnect_backoff_init(&b, 1000, 30000);
  reconnect_backoff_notify_attempt(&b, 50000); // delay berikutnya 2000
  TEST_ASSERT_FALSE(reconnect_backoff_should_attempt(&b, 51999));
  TEST_ASSERT_TRUE(reconnect_backoff_should_attempt(&b, 52000));
}

void test_backoff_reset_after_success() {
  ReconnectBackoff b;
  reconnect_backoff_init(&b, 1000, 30000);
  for (int i = 0; i < 5; i++) reconnect_backoff_notify_attempt(&b, 1000 * i);
  reconnect_backoff_reset(&b);
  TEST_ASSERT_EQUAL_UINT32(1000, b.current_delay_ms);
}

void test_backoff_survives_millis_overflow() {
  ReconnectBackoff b;
  reconnect_backoff_init(&b, 1000, 30000);
  reconnect_backoff_notify_attempt(&b, 0xFFFFFF00UL); // ~49 hari uptime
  TEST_ASSERT_FALSE(reconnect_backoff_should_attempt(&b, 0x00000100UL)); // baru 512 ms
  TEST_ASSERT_TRUE(reconnect_backoff_should_attempt(&b, 0x00000800UL));  // 2304 ms
}

void test_contract_backoff_parameters() {
  // §2.4: mulai 1 s, cap 30 s.
  TEST_ASSERT_EQUAL_UINT32(1000, RECONNECT_BACKOFF_INITIAL_MS);
  TEST_ASSERT_EQUAL_UINT32(30000, RECONNECT_BACKOFF_MAX_MS);
}

void test_battery_curve_endpoints_and_clamping() {
  TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4200));
  TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4500));
  TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(3270));
  TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(2500));
}

void test_battery_curve_interpolates_and_is_monotonic() {
  TEST_ASSERT_EQUAL_INT(50, battery_percent_from_mv(3840));
  TEST_ASSERT_EQUAL_INT(83, battery_percent_from_mv(4050)); // antara 4020(80) & 4080(85)
  int previous = -1;
  for (uint32_t mv = 3000; mv <= 4300; mv += 5) {
    int pct = battery_percent_from_mv(mv);
    TEST_ASSERT_TRUE(pct >= previous);
    TEST_ASSERT_TRUE(pct >= 0 && pct <= 100);
    previous = pct;
  }
}


void test_websocket_control_never_interleaves_partial_audio_frame() {
  TEST_ASSERT_FALSE(ws_may_poll(true,true,CAPTURE_RECORDING));
  TEST_ASSERT_FALSE(ws_may_poll(true,true,CAPTURE_READY));
  TEST_ASSERT_FALSE(ws_may_poll(true,true,CAPTURE_WAIT_ACK));
}
void test_connected_recording_does_not_enter_blocking_control_reader() {
  TEST_ASSERT_FALSE(ws_may_poll(false,true,CAPTURE_RECORDING));
  TEST_ASSERT_FALSE(ws_may_poll(false,true,CAPTURE_READY));
}
void test_poll_resumes_for_ack_keepalive_and_reconnection() {
  TEST_ASSERT_TRUE(ws_may_poll(false,true,CAPTURE_WAIT_ACK));
  TEST_ASSERT_TRUE(ws_may_poll(false,true,CAPTURE_IDLE));
  TEST_ASSERT_TRUE(ws_may_poll(false,false,CAPTURE_RECORDING));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_websocket_control_never_interleaves_partial_audio_frame);
  RUN_TEST(test_connected_recording_does_not_enter_blocking_control_reader);
  RUN_TEST(test_poll_resumes_for_ack_keepalive_and_reconnection);
  RUN_TEST(test_backoff_sequence_doubles_until_cap);
  RUN_TEST(test_backoff_waits_for_delay);
  RUN_TEST(test_backoff_reset_after_success);
  RUN_TEST(test_backoff_survives_millis_overflow);
  RUN_TEST(test_contract_backoff_parameters);
  RUN_TEST(test_battery_curve_endpoints_and_clamping);
  RUN_TEST(test_battery_curve_interpolates_and_is_monotonic);
  return UNITY_END();
}
