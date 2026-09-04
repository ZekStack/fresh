#pragma once

#include <ArduinoJson.h>
#include <Strata.h>

#include <cstddef>
#include <cstdint>

// Allocation categories make deterministic fault-injection tests target a
// specific Fresh subsystem without changing production allocation behavior.
enum class FreshAllocationCategory : uint8_t {
	Any = 0,
	General,
	JsonDocument,
	JsonCloneBuffer,
	JournalPayload,
	DurableSlotPayload,
	BackupBuffer,
};

// Fresh uses process-lifetime ArduinoJson allocators, one for each Strata
// placement, so JsonDocument values can safely outlive the Fresh instance that
// created them while still preserving the placement chosen by that instance.
ArduinoJson::Allocator &FreshJsonAllocator(Strata::Placement placement);

// Internal compatibility path for code that has not yet threaded an owning
// Fresh instance through the helper. It preserves Fresh 0.1.x/0.2.0-rc.1's
// PSRAM-preferred allocation behavior while ownership itself is provided by
// Strata. New instance-aware code must pass FreshConfig::memory.allocation.
inline ArduinoJson::Allocator &FreshJsonAllocator() {
	return FreshJsonAllocator(Strata::Placement::PreferExternal);
}

void *FreshAllocate(
    size_t size,
    Strata::Placement placement,
    FreshAllocationCategory category = FreshAllocationCategory::General
);
void *FreshReallocate(
    void *pointer,
    size_t newSize,
    Strata::Placement placement,
    FreshAllocationCategory category = FreshAllocationCategory::General
);
void FreshDeallocate(void *pointer);

#if defined(FRESH_TESTING)
// Fails the Nth matching allocation. failOnCall is one-based. A category of
// Any matches every allocation and minimumSize=0 disables the size filter.
void FreshTestConfigureAllocationFailure(
    size_t failOnCall,
    FreshAllocationCategory category = FreshAllocationCategory::Any,
    size_t minimumSize = 0,
    bool oneShot = true
);
void FreshTestResetAllocationFailure();
size_t FreshTestMatchingAllocationCount();
#endif
