
#include <unity.h>
#include <initializer_list>
#include "audio_route.h"
#include "buffer_manager.h"
void setUp(){TEST_ASSERT_TRUE(buffer_manager_init(4,16));}
void tearDown(){buffer_manager_deinit();}
void test_route_copies_measured_pcm_without_extra_or_missing_samples(){
  int16_t pcm[4]={-32768,12,345,32767}, out[4]={};
  audio_route_write(3,3,4,pcm,4,3,true);
  for(uint8_t ch : {2,3}) {
    uint64_t start=100;
    TEST_ASSERT_EQUAL(3,buffer_manager_read_chunk(ch,out,3,&start));
    TEST_ASSERT_EQUAL(0,start);
    TEST_ASSERT_EQUAL_INT16_ARRAY(pcm,out,3);
  }
  TEST_ASSERT_EQUAL(0,buffer_manager_available(0));
  TEST_ASSERT_EQUAL(0,buffer_manager_available(1));
}
void test_failed_source_is_not_replaced_with_silence(){
  int16_t pcm[4]={1,2,3,4};
  audio_route_write(3,3,4,pcm,4,4,false);
  TEST_ASSERT_EQUAL(0,buffer_manager_available(2));
  TEST_ASSERT_EQUAL(0,buffer_manager_available(3));
  audio_route_write(3,3,4,pcm,4,4,true);
  for(uint8_t ch : {2,3}) {
    uint64_t start=0;int16_t out[4];
    TEST_ASSERT_EQUAL(4,buffer_manager_read_chunk(ch,out,4,&start));
    TEST_ASSERT_EQUAL(4,start);
  }
}
void test_default_routes_all_four_physical_channels_unchanged(){
  int16_t pcm[1];
  for(uint8_t ch=0;ch<4;ch++) {
    pcm[0]=ch+10;
    audio_route_write(ch,0,0,pcm,1,1,true);
  }
  for(uint8_t ch=0;ch<4;ch++) {
    int16_t out[1];
    TEST_ASSERT_EQUAL(1,buffer_manager_read_chunk(ch,out,1));
    TEST_ASSERT_EQUAL(ch+10,out[0]);
  }
}
int main(){UNITY_BEGIN();
RUN_TEST(test_route_copies_measured_pcm_without_extra_or_missing_samples);
RUN_TEST(test_failed_source_is_not_replaced_with_silence);
RUN_TEST(test_default_routes_all_four_physical_channels_unchanged);
return UNITY_END();}
