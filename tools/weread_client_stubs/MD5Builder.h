// SPDX-License-Identifier: Apache-2.0
// 不验证摘要；真实协议摘要另有向量测试。/ Digest fake; real protocol hashes have separate vector tests.
#pragma once
class MD5Builder{public:void begin(){}void add(const uint8_t*,size_t){}void calculate(){}std::string toString(){return std::string(32,'0');}};
