// SPDX-License-Identifier: Apache-2.0
// 离线封面与打包测试的时钟替身。/ Offline cover/package clock fake.
#pragma once
#include <string>
#include <cstdint>
using String=std::string;
inline unsigned long millis(){static unsigned long now=100000;now+=2000;return now;}
inline void delay(unsigned long){}
inline long random(long a,long){return a;}
struct TestHeap{size_t getFreeHeap(){return 1000000;}size_t getMaxAllocHeap(){return 1000000;}};
inline TestHeap ESP;
inline unsigned uxTaskGetStackHighWaterMark(void*){return 16000;}
