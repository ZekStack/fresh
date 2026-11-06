#pragma once
#include <atomic>
#include <cstdint>
inline uint32_t esp_random() {
	static std::atomic<uint32_t> seed{123};
	return seed.fetch_add(0x9e3779b9u);
}
