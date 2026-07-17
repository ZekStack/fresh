#include "FreshMemory.h"

#include <esp_heap_caps.h>

namespace {

void *allocateWithCaps(size_t size, uint32_t caps) {
	return size == 0 ? nullptr : heap_caps_malloc(size, caps);
}

void *reallocateWithCaps(void *pointer, size_t newSize, uint32_t caps) {
	if (newSize == 0) {
		heap_caps_free(pointer);
		return nullptr;
	}
	return heap_caps_realloc(pointer, newSize, caps);
}

class FreshPreferPsramJsonAllocator final : public ArduinoJson::Allocator {
  public:
	void *allocate(size_t size) override {
		return FreshAllocate(size);
	}

	void deallocate(void *pointer) override {
		FreshDeallocate(pointer);
	}

	void *reallocate(void *pointer, size_t newSize) override {
		return FreshReallocate(pointer, newSize);
	}
};

FreshPreferPsramJsonAllocator allocator;

} // namespace

ArduinoJson::Allocator &FreshJsonAllocator() {
	return allocator;
}

bool FreshHasPsram() {
	return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

void *FreshAllocate(size_t size) {
	if (size == 0) {
		return nullptr;
	}

	if (FreshHasPsram()) {
		void *pointer = allocateWithCaps(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (pointer != nullptr) {
			return pointer;
		}
	}

	void *pointer = allocateWithCaps(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (pointer != nullptr) {
		return pointer;
	}

	return allocateWithCaps(size, MALLOC_CAP_8BIT);
}

void *FreshReallocate(void *pointer, size_t newSize) {
	if (pointer == nullptr) {
		return FreshAllocate(newSize);
	}
	if (newSize == 0) {
		FreshDeallocate(pointer);
		return nullptr;
	}

	if (FreshHasPsram()) {
		void *resized = reallocateWithCaps(
		    pointer,
		    newSize,
		    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
		);
		if (resized != nullptr) {
			return resized;
		}
	}

	void *resized = reallocateWithCaps(
	    pointer,
	    newSize,
	    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
	);
	if (resized != nullptr) {
		return resized;
	}

	return reallocateWithCaps(pointer, newSize, MALLOC_CAP_8BIT);
}

void FreshDeallocate(void *pointer) {
	if (pointer != nullptr) {
		heap_caps_free(pointer);
	}
}
