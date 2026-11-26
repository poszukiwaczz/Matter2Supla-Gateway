#pragma once
#include <cstdint>
namespace m2s::reporting {
inline bool alive(bool active,uint32_t now,uint32_t report,uint16_t maximum) {
  return active && uint32_t(now-report)<=uint32_t(maximum)*1000+90000;
}
inline bool retry_due(bool attempted,uint32_t now,uint32_t last) {
  return !attempted || uint32_t(now-last)>=30000;
}
inline bool poll_due(bool polled,uint32_t now,uint32_t last,uint32_t seconds) {
  return !polled || uint32_t(now-last)>=seconds*1000;
}
}
