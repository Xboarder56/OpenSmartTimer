#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
class String {
  std::string value_;
 public:
  String(const char* value = "") : value_(value) {}
  String(std::string value) : value_(value) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  char operator[](size_t i) const { return value_[i]; }
  void operator+=(char c) { value_ += c; }
  bool operator==(const char* s) const { return value_ == s; }
  bool endsWith(const char* s) const {
    const size_t n = strlen(s);
    return value_.size() >= n && value_.compare(value_.size() - n, n, s) == 0;
  }
  void remove(size_t start) { value_.erase(start); }
  void trim() {
    const size_t a = value_.find_first_not_of(" \r\n\t"), b = value_.find_last_not_of(" \r\n\t");
    value_ = a == std::string::npos ? "" : value_.substr(a, b - a + 1);
  }
  String substring(size_t a, size_t b) const { return value_.substr(a, b - a); }
  void toCharArray(char* dest, size_t size) const {
    const auto n = std::min(size - 1, value_.size());
    memcpy(dest, value_.data(), n); dest[n] = 0;
  }
};
