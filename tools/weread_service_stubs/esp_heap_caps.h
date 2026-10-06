// SPDX-License-Identifier: Apache-2.0
// 扩展内存失败注入。/ Allocation fault injection.
#pragma once
#include <cstdlib>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
inline bool fake_alloc_failure;
inline void* heap_caps_malloc(size_t n,int){return fake_alloc_failure?nullptr:malloc(n);}
inline void* heap_caps_calloc(size_t n,size_t z,int){return fake_alloc_failure?nullptr:calloc(n,z);}
inline void heap_caps_free(void* p){free(p);}
inline size_t heap_caps_get_free_size(int){return 1000000;}inline size_t heap_caps_get_largest_free_block(int){return 1000000;}
