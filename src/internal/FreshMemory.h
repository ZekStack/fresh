#pragma once

#include <ArduinoJson.h>

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

// Fresh uses one process-lifetime ArduinoJson allocator so JsonDocument values
// can safely outlive the Fresh instance that created them. Allocations prefer
// external PSRAM and fall back to internal 8-bit capable memory.
ArduinoJson::Allocator &FreshJsonAllocator();

void *FreshAllocate(
    size_t size,
    FreshAllocationCategory category = FreshAllocationCategory::General
);
void *FreshReallocate(
    void *pointer,
    size_t newSize,
    FreshAllocationCategory category = FreshAllocationCategory::General
);
void FreshDeallocate(void *pointer);
bool FreshHasPsram();

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
