// Ring buffer per-channel (FR-HW-011, SDD_HARDWARE.md §4): urutan FIFO,
// wrap-around, kebijakan drop-oldest, dan isolasi antar channel.

#include <unity.h>

#include "buffer_manager.h"

static const size_t kCapacity = 8;

void setUp() {
  TEST_ASSERT_TRUE(buffer_manager_init(4, kCapacity));
}

void tearDown() {
  buffer_manager_deinit();
}

static void fill_sequence(int16_t *buf, size_t n, int16_t start) {
  for (size_t i = 0; i < n; i++) buf[i] = (int16_t)(start + i);
}

void test_init_rejects_invalid_args() {
  TEST_ASSERT_FALSE(buffer_manager_init(0, 8));
  TEST_ASSERT_FALSE(buffer_manager_init(5, 8));
  TEST_ASSERT_FALSE(buffer_manager_init(1, 0));
}

void test_write_then_read_preserves_order() {
  int16_t in[5], out[5];
  fill_sequence(in, 5, 100);
  buffer_manager_write_block(0, in, 5);
  TEST_ASSERT_EQUAL_UINT32(5, buffer_manager_available(0));

  TEST_ASSERT_EQUAL_UINT32(5, buffer_manager_read_chunk(0, out, 5));
  TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 5);
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_available(0));
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_get_drop_count(0));
}

void test_wrap_around_keeps_order() {
  int16_t in[6], out[8];
  fill_sequence(in, 6, 0);
  buffer_manager_write_block(0, in, 6);
  TEST_ASSERT_EQUAL_UINT32(4, buffer_manager_read_chunk(0, out, 4)); // tail di index 4

  fill_sequence(in, 6, 6); // 6..11, melewati batas array (wrap)
  buffer_manager_write_block(0, in, 6);
  TEST_ASSERT_EQUAL_UINT32(8, buffer_manager_available(0));

  TEST_ASSERT_EQUAL_UINT32(8, buffer_manager_read_chunk(0, out, 8));
  int16_t expected[8];
  fill_sequence(expected, 8, 4);
  TEST_ASSERT_EQUAL_INT16_ARRAY(expected, out, 8);
}

void test_overflow_drops_oldest_samples() {
  int16_t in[6], out[8];
  fill_sequence(in, 6, 0);
  buffer_manager_write_block(0, in, 6);
  fill_sequence(in, 6, 6); // total 12 > kapasitas 8 -> 4 tertua hilang
  buffer_manager_write_block(0, in, 6);

  TEST_ASSERT_EQUAL_UINT32(8, buffer_manager_available(0));
  TEST_ASSERT_EQUAL_UINT32(4, buffer_manager_get_drop_count(0));

  buffer_manager_read_chunk(0, out, 8);
  int16_t expected[8];
  fill_sequence(expected, 8, 4); // 4..11
  TEST_ASSERT_EQUAL_INT16_ARRAY(expected, out, 8);
}

void test_block_larger_than_capacity_keeps_newest() {
  int16_t in[20], out[8];
  fill_sequence(in, 20, 0);
  buffer_manager_write_block(0, in, 20);

  TEST_ASSERT_EQUAL_UINT32(8, buffer_manager_available(0));
  TEST_ASSERT_EQUAL_UINT32(12, buffer_manager_get_drop_count(0));
  buffer_manager_read_chunk(0, out, 8);
  int16_t expected[8];
  fill_sequence(expected, 8, 12);
  TEST_ASSERT_EQUAL_INT16_ARRAY(expected, out, 8);
}

void test_partial_read_returns_available_only() {
  int16_t in[3], out[8];
  fill_sequence(in, 3, 7);
  buffer_manager_write_block(1, in, 3);
  TEST_ASSERT_EQUAL_UINT32(3, buffer_manager_read_chunk(1, out, 8));
  TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 3);
}

void test_channels_are_isolated() {
  int16_t a[4] = {1, 2, 3, 4};
  int16_t b[2] = {-1, -2};
  buffer_manager_write_block(0, a, 4);
  buffer_manager_write_block(3, b, 2);

  TEST_ASSERT_EQUAL_UINT32(4, buffer_manager_available(0));
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_available(1));
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_available(2));
  TEST_ASSERT_EQUAL_UINT32(2, buffer_manager_available(3));
}

void test_clear_empties_channel_without_counting_drops() {
  int16_t in[5];
  fill_sequence(in, 5, 0);
  buffer_manager_write_block(2, in, 5);
  buffer_manager_clear(2);
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_available(2));
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_get_drop_count(2));
}

void test_out_of_range_channel_is_ignored() {
  int16_t in[2] = {1, 2}, out[2];
  buffer_manager_write_block(4, in, 2);
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_available(4));
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_read_chunk(4, out, 2));
}

void test_sample_cursor_exposes_loss_and_clear() {
  int16_t in[20] = {0}, out[8];
  uint64_t start = UINT64_MAX;
  buffer_manager_write_block(0, in, 6);
  TEST_ASSERT_EQUAL_UINT32(4, buffer_manager_read_chunk(0, out, 4, &start));
  TEST_ASSERT_TRUE(start == 0);
  buffer_manager_write_block(0, in, 6); // remaining indices 4..11
  buffer_manager_write_block(0, in, 3); // drop indices 4..6
  buffer_manager_read_chunk(0, out, 4, &start);
  TEST_ASSERT_TRUE(start == 7);
  buffer_manager_clear(0); // discard indices 11..14, but cursor must not restart
  buffer_manager_write_block(0, in, 20); // indices 15..34, keep 27..34
  buffer_manager_read_chunk(0, out, 8, &start);
  TEST_ASSERT_TRUE(start == 27);
}

void test_suppressed_frames_advance_cursor_and_isolate_channels() {
  int16_t in[4] = {0}, out[4];
  uint64_t start = UINT64_MAX;
  buffer_manager_write_block(0, in, 4);
  buffer_manager_skip_samples(0, 16000);
  TEST_ASSERT_EQUAL_UINT32(0, buffer_manager_available(0));
  buffer_manager_write_block(0, in, 4);
  buffer_manager_read_chunk(0, out, 4, &start);
  TEST_ASSERT_TRUE(start == 16004);
  buffer_manager_write_block(1, in, 4);
  buffer_manager_read_chunk(1, out, 4, &start);
  TEST_ASSERT_TRUE(start == 0);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sample_cursor_exposes_loss_and_clear);
  RUN_TEST(test_suppressed_frames_advance_cursor_and_isolate_channels);
  RUN_TEST(test_init_rejects_invalid_args);
  RUN_TEST(test_write_then_read_preserves_order);
  RUN_TEST(test_wrap_around_keeps_order);
  RUN_TEST(test_overflow_drops_oldest_samples);
  RUN_TEST(test_block_larger_than_capacity_keeps_newest);
  RUN_TEST(test_partial_read_returns_available_only);
  RUN_TEST(test_channels_are_isolated);
  RUN_TEST(test_clear_empties_channel_without_counting_drops);
  RUN_TEST(test_out_of_range_channel_is_ignored);
  return UNITY_END();
}
