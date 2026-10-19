#pragma once

#include <cstdint>

using uint8_t = std::uint8_t;
using uint32_t = std::uint32_t;
using int32_t = std::int32_t;

constexpr uint8_t LOW = 0;
constexpr uint8_t HIGH = 1;
constexpr uint8_t INPUT = 0;
constexpr uint8_t OUTPUT = 1;

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int digitalRead(uint8_t pin);

