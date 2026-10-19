#pragma once

#include <cstdint>

using esp_err_t = std::int32_t;
using gpio_num_t = std::int32_t;

constexpr esp_err_t ESP_OK = 0;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gpio_set_level(gpio_num_t gpioNumber, std::uint32_t level);

#ifdef __cplusplus
}
#endif
