// SPDX-License-Identifier: Apache-2.0
// 信号量替身。/ Semaphore fake.
#pragma once
typedef int* SemaphoreHandle_t;
inline int fake_mutex=1,fake_finished;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return &fake_mutex;}
inline SemaphoreHandle_t xSemaphoreCreateBinary(){return &fake_finished;}
inline int xSemaphoreTake(int* s,unsigned){if(s==&fake_mutex)return 1;int n=*s;*s=0;return n;}
inline int xSemaphoreGive(int* s){*s=1;return 1;}
