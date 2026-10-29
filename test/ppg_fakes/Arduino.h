#pragma once
#include <stdint.h>
#include <stddef.h>
uint32_t millis();
void delay(uint32_t ms);
struct FakeSerial {
  template<typename... Args> void printf(const char *,Args...) {}
  void println(const char *) {}
};
extern FakeSerial Serial;
