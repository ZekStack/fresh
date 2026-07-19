#include "FreshMemory.h"

#include <esp_heap_caps.h>

#if defined(FRESH_TESTING)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

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

#if defined(FRESH_TESTING)
struct FreshAllocationFaultState {
	size_t failOnCall = 0;
	size_t matchingCalls = 0;
	size_t minimumSize = 0;
	FreshAllocationCategory category = FreshAllocationCategory::Any;
	bool oneShot = true;
	bool armed = false;
};

FreshAllocationFaultState allocationFault;
portMUX_TYPE allocationFaultMux = portMUX_INITIALIZER_UNLOCKED;

bool shouldFailAllocation(size_t size, FreshAllocationCategory category) {
	bool fail = false;
	portENTER_CRITICAL(&allocationFaultMux);
	const bool categoryMatches = allocationFault.category == FreshAllocationCategory::Any ||
	                             allocationFault.category == category;
	if (allocationFault.armed && categoryMatches && size >= allocationFault.minimumSize) {
		allocationFault.matchingCalls++;
		if (allocationFault.failOnCall > 0 &&
		    allocationFault.matchingCalls == allocationFault.failOnCall) {
			fail = true;
			if (allocationFault.oneShot) {
				allocationFault.armed = false;
			}
		}
	}
	portEXIT_CRITICAL(&allocationFaultMux);
	return fail;
}
#else
bool shouldFailAllocation(size_t, FreshAllocationCategory) {
	return false;
}
#endif

class FreshPreferPsramJsonAllocator final : public ArduinoJson::Allocator {
  public:
	void *allocate(size_t size) override {
		return FreshAllocate(size, FreshAllocationCategory::JsonDocument);
	}

	void deallocate(void *pointer) override {
		FreshDeallocate(pointer);
	}

	void *reallocate(void *pointer, size_t newSize) override {
		return FreshReallocate(pointer, newSize, FreshAllocationCategory::JsonDocument);
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

void *FreshAllocate(size_t size, FreshAllocationCategory category) {
	if (size == 0) {
		return nullptr;
	}
	if (shouldFailAllocation(size, category)) {
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

void *FreshReallocate(void *pointer, size_t newSize, FreshAllocationCategory category) {
	if (pointer == nullptr) {
		return FreshAllocate(newSize, category);
	}
	if (newSize == 0) {
		FreshDeallocate(pointer);
		return nullptr;
	}
	if (shouldFailAllocation(newSize, category)) {
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

#if defined(FRESH_TESTING)
void FreshTestConfigureAllocationFailure(
    size_t failOnCall,
    FreshAllocationCategory category,
    size_t minimumSize,
    bool oneShot
) {
	portENTER_CRITICAL(&allocationFaultMux);
	allocationFault.failOnCall = failOnCall;
	allocationFault.matchingCalls = 0;
	allocationFault.minimumSize = minimumSize;
	allocationFault.category = category;
	allocationFault.oneShot = oneShot;
	allocationFault.armed = failOnCall > 0;
	portEXIT_CRITICAL(&allocationFaultMux);
}

void FreshTestResetAllocationFailure() {
	portENTER_CRITICAL(&allocationFaultMux);
	allocationFault = FreshAllocationFaultState();
	portEXIT_CRITICAL(&allocationFaultMux);
}

size_t FreshTestMatchingAllocationCount() {
	portENTER_CRITICAL(&allocationFaultMux);
	const size_t count = allocationFault.matchingCalls;
	portEXIT_CRITICAL(&allocationFaultMux);
	return count;
}
#endif
