#include "ws_frame_writer.h"

bool WsFrameWriter::begin(uint8_t *buffer,size_t n,uint32_t mask,uint8_t opcode,
                          uint32_t now,uint32_t timeout) {
  if(pending() || !buffer || !n || n>65535 || (opcode!=1 && opcode!=2))return false;
  size_t h=n<126?6:8;
  uint8_t *out=buffer+14-h;
  out[0]=0x80|opcode;
  size_t key=2;
  if(n<126)out[1]=0x80|n;
  else {out[1]=0xfe;out[2]=n>>8;out[3]=n&0xff;key=4;}
  for(int i=0;i<4;i++)out[key+i]=(uint8_t)(mask>>(8*i));
  for(size_t i=0;i<n;i++)buffer[14+i]^=out[key+i%4];
  data_=out;size_=h+n;offset_=0;start_ms_=now;timeout_ms_=timeout;
  return true;
}
FrameWriteStatus WsFrameWriter::pump(FrameWrite write,void *context,uint32_t now) {
  if(!pending())return FRAME_IDLE;
  if(now-start_ms_>timeout_ms_) {reset();return FRAME_FAILED;}
  int n=write(data_+offset_,size_-offset_,context);
  if(n<0 || (size_t)n>size_-offset_) {reset();return FRAME_FAILED;}
  offset_+=n;
  if(offset_==size_) {reset();return FRAME_COMPLETE;}
  return FRAME_PENDING;
}
