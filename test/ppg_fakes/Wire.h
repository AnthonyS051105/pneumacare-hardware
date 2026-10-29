#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint32_t fake_ms;
struct FakeWire {
  bool powered=false;
  uint16_t timeout_ms=50;
  unsigned requests=0;
  uint8_t reg=0,pos=0;
  void begin(int,int,uint32_t) {}
  void setTimeOut(uint16_t n) {timeout_ms=n;}
  void beginTransmission(uint8_t) {pos=0;}
  void write(uint8_t b) {if(pos++==0)reg=b;}
  uint8_t endTransmission(bool stop=true) {
    if(!powered && stop){fake_ms+=timeout_ms;return 5;}
    return 0;
  }
  size_t requestFrom(uint16_t,uint8_t n) {
    requests++;
    if(!powered){fake_ms+=timeout_ms;return 0;}
    return n;
  }
  int available() {return 0;}
  int read() {return reg==0xff?0x15:reg==0xfe?1:0;}
};
extern FakeWire Wire;
