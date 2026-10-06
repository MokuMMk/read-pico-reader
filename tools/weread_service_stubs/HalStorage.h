// SPDX-License-Identifier: Apache-2.0
// 存储替身。/ Storage fake.
#pragma once
struct FakeStorage{void configure(const char*,const char*){}bool exists(const std::string&){return false;}std::string map(const std::string& s){return "/sdcard/books/"+s;}};
inline FakeStorage Storage;
