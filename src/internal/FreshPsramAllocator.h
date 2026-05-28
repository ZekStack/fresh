#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <vector>

template <typename T> class FreshPsramAllocator {
  public:
	using value_type = T;

	FreshPsramAllocator() = default;

	template <typename U> FreshPsramAllocator(const FreshPsramAllocator<U> &) {
	}

	T *allocate(std::size_t count) {
		const size_t bytes = count * sizeof(T);
		void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (ptr == nullptr) {
			ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
		}
		if (ptr == nullptr) {
			ptr = malloc(bytes);
		}
		return static_cast<T *>(ptr);
	}

	void deallocate(T *ptr, std::size_t) {
		free(ptr);
	}
};

template <typename T, typename U>
bool operator==(const FreshPsramAllocator<T> &, const FreshPsramAllocator<U> &) {
	return true;
}

template <typename T, typename U>
bool operator!=(const FreshPsramAllocator<T> &, const FreshPsramAllocator<U> &) {
	return false;
}

using FreshByteVector = std::vector<uint8_t, FreshPsramAllocator<uint8_t>>;

