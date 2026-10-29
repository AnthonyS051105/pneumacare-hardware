#include <unity.h>
#include <Arduino.h>
#include <Wire.h>
#include "ppg_acquisition.h"
#include <config.h>
uint32_t fake_ms=0;
FakeSerial Serial;
FakeWire Wire;
uint32_t millis(){return fake_ms;}
void delay(uint32_t ms){fake_ms+=ms;}
void setUp(){}
void tearDown(){}
static void test_absent_sensor_cannot_hold_audio_for_default_wire_timeout() {
  Wire.powered=false;
  uint32_t before=millis();
  TEST_ASSERT_FALSE(ppg_acquisition_init());
  TEST_ASSERT_FALSE(ppg_acquisition_is_ready());
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(20,millis()-before);
}
static void test_absent_sensor_retry_is_throttled_and_recovers_when_power_returns() {
  Wire.powered=false;ppg_acquisition_init();
  unsigned calls=Wire.requests;PpgSample out[1];
  for(int i=0;i<1000;i++)TEST_ASSERT_EQUAL(0,ppg_acquisition_read(out,1));
  TEST_ASSERT_EQUAL(calls,Wire.requests);
  fake_ms+=PPG_REINIT_INTERVAL_MS;
  uint32_t before=millis();ppg_acquisition_read(out,1);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(20,millis()-before);
  TEST_ASSERT_EQUAL(calls+1,Wire.requests);
  Wire.powered=true;fake_ms+=PPG_REINIT_INTERVAL_MS;
  ppg_acquisition_read(out,1);
  TEST_ASSERT_TRUE(ppg_acquisition_is_ready());
}
static void test_power_loss_read_timeout_is_bounded_and_errors_remain_visible() {
  Wire.powered=true;TEST_ASSERT_TRUE(ppg_acquisition_init());
  Wire.powered=false;PpgSample out[1];
  auto errors=ppg_acquisition_get_i2c_errors();
  for(int i=0;i<PPG_MAX_CONSECUTIVE_I2C_ERRORS;i++) {
    uint32_t before=millis();TEST_ASSERT_EQUAL(0,ppg_acquisition_read(out,1));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(20,millis()-before);
  }
  TEST_ASSERT_FALSE(ppg_acquisition_is_ready());
  TEST_ASSERT_EQUAL(errors+PPG_MAX_CONSECUTIVE_I2C_ERRORS,ppg_acquisition_get_i2c_errors());
}
int main(){UNITY_BEGIN();RUN_TEST(test_absent_sensor_cannot_hold_audio_for_default_wire_timeout);RUN_TEST(test_absent_sensor_retry_is_throttled_and_recovers_when_power_returns);RUN_TEST(test_power_loss_read_timeout_is_bounded_and_errors_remain_visible);return UNITY_END();}
