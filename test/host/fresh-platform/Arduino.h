#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
inline uint32_t millis() {
	static const auto start = std::chrono::steady_clock::now();
	return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                 std::chrono::steady_clock::now() - start
	)
	                                 .count());
}
inline void delay(uint32_t ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
