// SPDX-License-Identifier: Apache-2.0
// 任务失败和取消注入。/ Task failure and cancel injection.
#pragma once
inline void (*fake_worker)(void*);
inline bool fake_task_failure;
inline int xTaskCreate(void(*fn)(void*),const char*,unsigned stack,void*,int,void*){assert(stack==16384);if(fake_task_failure)return 0;fake_worker=fn;return 1;}
inline unsigned uxTaskGetStackHighWaterMark(void*){return 8192;}
inline TickType_t xTaskGetTickCount(){return 0;}
inline void vTaskDelay(unsigned){}
inline void vTaskDelete(void*){}
