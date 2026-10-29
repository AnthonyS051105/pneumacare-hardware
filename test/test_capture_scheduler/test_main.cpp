#include <unity.h>
#include "capture_scheduler.h"
#include "buffer_manager.h"
#include "capture_delivery.h"
#include "ws_frame_writer.h"
#include <string.h>
void setUp() {}
void tearDown() { buffer_manager_deinit(); }
static void test_exact_window_waits_for_ack_and_interval() {
  capture_init(8000, 10000, 30000);
  TEST_ASSERT_TRUE(capture_begin(100, 1700000000000ULL));
  size_t recorded=0;
  for (int i=0;i<16;i++) {
    size_t n=capture_take(512); recorded+=n; capture_commit(n);
  }
  TEST_ASSERT_EQUAL(8000, recorded);
  TEST_ASSERT_EQUAL(CAPTURE_READY, capture_snapshot().state);
  TEST_ASSERT_EQUAL(0,capture_take(512));
  capture_mark_sent();
  TEST_ASSERT_FALSE(capture_begin(20000,0));
  TEST_ASSERT_FALSE(capture_ack(999,20000));
  TEST_ASSERT_TRUE(capture_ack(1,20000));
  TEST_ASSERT_FALSE(capture_begin(20001,0));
  TEST_ASSERT_TRUE(capture_begin(21000,0));
}
static void test_pause_preserves_pcm_and_sample_timeline() {
  TEST_ASSERT_TRUE(buffer_manager_init(1,8));
  int16_t pcm[]={1,2,3,4},out[4];
  buffer_manager_write_block(0,pcm,4);
  buffer_manager_advance_samples(0,100);
  uint64_t start;
  TEST_ASSERT_EQUAL(4,buffer_manager_read_chunk(0,out,4,&start));
  TEST_ASSERT_EQUAL(0,start);
  TEST_ASSERT_EQUAL_MEMORY(pcm,out,sizeof(pcm));
  buffer_manager_write_block(0,pcm,4);
  TEST_ASSERT_EQUAL(4,buffer_manager_read_chunk(0,out,4,&start));
  TEST_ASSERT_EQUAL(104,start);
  TEST_ASSERT_EQUAL(0,buffer_manager_get_drop_count(0));
}
static void test_abort_during_producer_block_and_ack_timeout() {
  capture_init(8000,10000,30000);
  TEST_ASSERT_TRUE(capture_begin(100,0));
  TEST_ASSERT_EQUAL(512,capture_take(512));
  capture_abort(200);
  TEST_ASSERT_FALSE(capture_begin(10100,0));
  capture_commit(512);
  TEST_ASSERT_TRUE(capture_begin(10100,0));
  TEST_ASSERT_FALSE(capture_ack(1,10101));
  TEST_ASSERT_TRUE(capture_expired(40101));
  TEST_ASSERT_FALSE(capture_begin(50000,0));
  capture_abort(50000);
  TEST_ASSERT_FALSE(capture_begin(50001,0));
  TEST_ASSERT_TRUE(capture_begin(51000,0));
}
static void test_oldest_sample_index_after_oversized_write() {
  TEST_ASSERT_TRUE(buffer_manager_init(1,8));
  int16_t data[12]={},out[8];uint64_t start;
  buffer_manager_write_block(0,data,4);
  buffer_manager_write_block(0,data,12);
  TEST_ASSERT_EQUAL(8,buffer_manager_read_chunk(0,out,8,&start));
  TEST_ASSERT_EQUAL(8,start);
}
static void test_seven_second_period_delays_if_ack_late() {
  capture_init(8000,7000,30000);
  TEST_ASSERT_TRUE(capture_begin(100,0));
  size_t n=capture_take(8000);capture_commit(n);capture_mark_sent();
  TEST_ASSERT_TRUE(capture_ack(1,5000));
  TEST_ASSERT_FALSE(capture_begin(7099,0));
  TEST_ASSERT_TRUE(capture_begin(7100,0));
  n=capture_take(8000);capture_commit(n);capture_mark_sent();
  TEST_ASSERT_TRUE(capture_ack(2,15000));
  TEST_ASSERT_FALSE(capture_begin(15999,0));
  TEST_ASSERT_TRUE(capture_begin(16000,0));
}

static void test_two_second_streams_before_ready_with_bounded_ring() {
  capture_init(32000,20000,30000);
  TEST_ASSERT_TRUE(buffer_manager_init(4,8000));
  TEST_ASSERT_TRUE(capture_begin(100,1700000000000ULL));
  CaptureDelivery delivery;
  uint32_t drops[4]={};
  delivery.begin(1,32000,drops);
  int16_t block[256],out[4000];
  bool early=false;
  for(size_t produced=0;produced<32000;produced+=256) {
    size_t n=capture_take(256);
    for(size_t i=0;i<n;i++) block[i]=(int16_t)(produced+i);
    for(int ch=0;ch<4;ch++)buffer_manager_write_block(ch,block,n);
    capture_commit(n);
    for(int ch=0;ch<4;ch++) {
      if(buffer_manager_available(ch)<4000)continue;
      uint64_t start;
      TEST_ASSERT_EQUAL(4000,buffer_manager_read_chunk(ch,out,4000,&start));
      if(capture_snapshot().state==CAPTURE_RECORDING)early=true;
      for(int i=0;i<4000;i++)TEST_ASSERT_EQUAL_INT16((int16_t)(start+i),out[i]);
      TEST_ASSERT_TRUE(delivery.consume(ch,start,4000));
      TEST_ASSERT_TRUE(buffer_manager_available(ch)<=8000);
    }
  }
  TEST_ASSERT_TRUE(early);
  TEST_ASSERT_TRUE(delivery.complete());
  TEST_ASSERT_EQUAL(CAPTURE_READY,capture_snapshot().state);
  capture_mark_sent();
  TEST_ASSERT_TRUE(capture_ack(1,2600));
  TEST_ASSERT_FALSE(capture_begin(20099,0));
  TEST_ASSERT_TRUE(capture_begin(20100,0));
}
static void test_streaming_delivery_rejects_overflow_and_sample_gap() {
  CaptureDelivery delivery;uint32_t drops[4]={};
  delivery.begin(1,32000,drops);
  TEST_ASSERT_TRUE(delivery.consume(0,1000,4000));
  TEST_ASSERT_FALSE(delivery.consume(0,5256,4000));
  // Even a loss before a channel's first transmitted chunk must abort.
  TEST_ASSERT_TRUE(buffer_manager_init(4,8000));
  int16_t block[256]={};
  for(int i=0;i<33;i++)buffer_manager_write_block(3,block,256);
  for(int ch=0;ch<4;ch++)drops[ch]=buffer_manager_get_drop_count(ch);
  TEST_ASSERT_TRUE(delivery.overflowed(drops));
  TEST_ASSERT_FALSE(delivery.complete());
  delivery.begin(2,32000,drops);
  TEST_ASSERT_FALSE(delivery.overflowed(drops));
  TEST_ASSERT_EQUAL(0,delivery.sent(0));
  TEST_ASSERT_TRUE(delivery.consume(0,100000,4000));
  TEST_ASSERT_FALSE(delivery.consume(0,104000,32000));
}


struct FakeSocket { uint8_t bytes[256]; size_t used=0; int limit=3; };
static int fake_write(const uint8_t *data,size_t n,void *context) {
  auto *s=(FakeSocket *)context;
  if(s->limit<=0)return s->limit;
  if(n>(size_t)s->limit)n=s->limit;
  memcpy(s->bytes+s->used,data,n);s->used+=n;return n;
}
static void test_frame_writer_partial_writes_and_random_mask() {
  uint8_t buffer[14+8]={};memcpy(buffer+14,"PCA1data",8);
  WsFrameWriter writer;FakeSocket socket;
  TEST_ASSERT_TRUE(writer.begin(buffer,8,0x04030201,2,100,500));
  TEST_ASSERT_FALSE(writer.begin(buffer,8,0,2,101,500)); // never replace in-flight PCM
  socket.limit=0;
  TEST_ASSERT_EQUAL(FRAME_PENDING,writer.pump(fake_write,&socket,110));
  socket.limit=3;
  while(writer.pending())writer.pump(fake_write,&socket,120);
  TEST_ASSERT_EQUAL(14,socket.used);
  TEST_ASSERT_EQUAL_HEX8(0x82,socket.bytes[0]);
  TEST_ASSERT_EQUAL_HEX8(0x88,socket.bytes[1]);
  for(int i=0;i<8;i++)TEST_ASSERT_EQUAL_INT8("PCA1data"[i],socket.bytes[6+i]^socket.bytes[2+i%4]);
}
static void test_frame_writer_timeout_and_error_are_bounded() {
  uint8_t buffer[22]={};WsFrameWriter writer;FakeSocket socket;
  TEST_ASSERT_TRUE(writer.begin(buffer,8,1,2,0xfffffff0,500));
  socket.limit=0;
  TEST_ASSERT_EQUAL(FRAME_PENDING,writer.pump(fake_write,&socket,0x10));
  TEST_ASSERT_EQUAL(FRAME_FAILED,writer.pump(fake_write,&socket,0x200));
  TEST_ASSERT_FALSE(writer.pending());
  TEST_ASSERT_TRUE(writer.begin(buffer,8,1,2,1000,500));
  socket.limit=-1;
  TEST_ASSERT_EQUAL(FRAME_FAILED,writer.pump(fake_write,&socket,1001));
  TEST_ASSERT_FALSE(writer.pending());
}
static void test_frame_writer_extended_length_is_big_endian_and_masked() {
  uint8_t buffer[14+130]={};
  for(int i=0;i<130;i++)buffer[14+i]=(uint8_t)i;
  WsFrameWriter writer;FakeSocket socket;socket.limit=31;
  TEST_ASSERT_TRUE(writer.begin(buffer,130,0xa1b2c3d4,1,0,500));
  while(writer.pending())writer.pump(fake_write,&socket,1);
  TEST_ASSERT_EQUAL(138,socket.used);
  TEST_ASSERT_EQUAL_HEX8(0x81,socket.bytes[0]);
  TEST_ASSERT_EQUAL_HEX8(0xfe,socket.bytes[1]);
  TEST_ASSERT_EQUAL(0,socket.bytes[2]);TEST_ASSERT_EQUAL(130,socket.bytes[3]);
  for(int i=0;i<130;i++)TEST_ASSERT_EQUAL_UINT8(i,socket.bytes[8+i]^socket.bytes[4+i%4]);
}

static void test_source_dma_gap_stops_capture_until_transport_aborts() {
  capture_init(32000,20000,30000);
  capture_note_source_errors(5,0); // an idle gap is not part of the next window
  TEST_ASSERT_TRUE(capture_begin(100,0));
  capture_note_source_errors(5,0);
  size_t n=capture_take(256);capture_commit(n);
  TEST_ASSERT_EQUAL(CAPTURE_SOURCE_OK,capture_snapshot().source_fault);
  capture_note_source_errors(6,0);
  TEST_ASSERT_EQUAL(CAPTURE_SOURCE_DMA_GAP,capture_snapshot().source_fault);
  TEST_ASSERT_EQUAL(0,capture_take(256));
  capture_mark_sent();
  TEST_ASSERT_FALSE(capture_ack(1,3000));
  capture_abort(3000);
  TEST_ASSERT_TRUE(capture_begin(20100,0));
  TEST_ASSERT_EQUAL(CAPTURE_SOURCE_OK,capture_snapshot().source_fault);
}
static void test_read_error_invalidates_inflight_block_and_cannot_complete() {
  capture_init(256,20000,30000);
  capture_note_source_errors(0,0);
  TEST_ASSERT_TRUE(capture_begin(100,0));
  TEST_ASSERT_EQUAL(256,capture_take(256));
  capture_note_source_errors(0,1);
  capture_commit(256);
  TEST_ASSERT_EQUAL(CAPTURE_SOURCE_I2S_GAP,capture_snapshot().source_fault);
  TEST_ASSERT_FALSE(capture_snapshot().in_block);
  TEST_ASSERT_EQUAL(0,capture_snapshot().retained);
  TEST_ASSERT_EQUAL(CAPTURE_RECORDING,capture_snapshot().state);
}
static void test_source_gap_after_final_committed_block_does_not_cancel_valid_pcm() {
  capture_init(256,20000,30000);
  capture_note_source_errors(0,0);
  TEST_ASSERT_TRUE(capture_begin(100,0));
  capture_commit(capture_take(256));
  TEST_ASSERT_EQUAL(CAPTURE_READY,capture_snapshot().state);
  capture_note_source_errors(1,0); // next acquisition is outside finished recording
  TEST_ASSERT_EQUAL(CAPTURE_SOURCE_OK,capture_snapshot().source_fault);
  capture_mark_sent();TEST_ASSERT_TRUE(capture_ack(1,1000));
}

int main(){UNITY_BEGIN();RUN_TEST(test_source_dma_gap_stops_capture_until_transport_aborts);RUN_TEST(test_read_error_invalidates_inflight_block_and_cannot_complete);RUN_TEST(test_source_gap_after_final_committed_block_does_not_cancel_valid_pcm);RUN_TEST(test_frame_writer_extended_length_is_big_endian_and_masked);RUN_TEST(test_frame_writer_partial_writes_and_random_mask);RUN_TEST(test_frame_writer_timeout_and_error_are_bounded);RUN_TEST(test_two_second_streams_before_ready_with_bounded_ring);RUN_TEST(test_streaming_delivery_rejects_overflow_and_sample_gap);RUN_TEST(test_seven_second_period_delays_if_ack_late);RUN_TEST(test_abort_during_producer_block_and_ack_timeout);RUN_TEST(test_oldest_sample_index_after_oversized_write);RUN_TEST(test_exact_window_waits_for_ack_and_interval);RUN_TEST(test_pause_preserves_pcm_and_sample_timeline);return UNITY_END();}
