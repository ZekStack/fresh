#include "FreshMemory.h"

#if defined(FRESH_TESTING)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

namespace {

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

class FreshStrataJsonAllocator final : public ArduinoJson::Allocator {
  public:
	explicit FreshStrataJsonAllocator(Strata::Placement placement) : _placement(placement) {
	}

	void *allocate(size_t size) override {
		return FreshAllocate(size, _placement, FreshAllocationCategory::JsonDocument);
	}

	void deallocate(void *pointer) override {
		FreshDeallocate(pointer);
	}

	void *reallocate(void *pointer, size_t newSize) override {
		return FreshReallocate(
		    pointer,
		    newSize,
		    _placement,
		    FreshAllocationCategory::JsonDocument
		);
	}

  private:
	Strata::Placement _placement;
};

FreshStrataJsonAllocator defaultAllocator(Strata::Placement::Default);
FreshStrataJsonAllocator internalAllocator(Strata::Placement::Internal);
FreshStrataJsonAllocator preferExternalAllocator(Strata::Placement::PreferExternal);
FreshStrataJsonAllocator requireExternalAllocator(Strata::Placement::RequireExternal);

} // namespace

ArduinoJson::Allocator &FreshJsonAllocator(Strata::Placement placement) {
	switch (placement) {
	case Strata::Placement::Default: return defaultAllocator;
	case Strata::Placement::Internal: return internalAllocator;
	case Strata::Placement::PreferExternal: return preferExternalAllocator;
	case Strata::Placement::RequireExternal: return requireExternalAllocator;
	}
	return defaultAllocator;
}

void *FreshAllocate(
    size_t size,
    Strata::Placement placement,
    FreshAllocationCategory category
) {
	if (size == 0) {
		return nullptr;
	}
	if (shouldFailAllocation(size, category)) {
		return nullptr;
	}
	return Strata::allocate(size, placement);
}

void *FreshReallocate(
    void *pointer,
    size_t newSize,
    Strata::Placement placement,
    FreshAllocationCategory category
) {
	if (pointer == nullptr) {
		return FreshAllocate(newSize, placement, category);
	}
	if (newSize == 0) {
		FreshDeallocate(pointer);
		return nullptr;
	}
	if (shouldFailAllocation(newSize, category)) {
		return nullptr;
	}
	return Strata::reallocate(pointer, newSize, placement);
}

void FreshDeallocate(void *pointer) {
	Strata::free(pointer);
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
