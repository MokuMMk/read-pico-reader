// SPDX-License-Identifier: Apache-2.0
// 封面与打包测试不使用密码运算。/ Cover/package fixtures do not exercise cryptography.
#pragma once
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
inline int psa_crypto_init(){return 0;}
inline int psa_hash_compute(int,const uint8_t*,size_t,uint8_t* out,size_t n,size_t* used){memset(out,0,n);*used=n;return 0;}
