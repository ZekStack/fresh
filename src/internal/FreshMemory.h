#pragma once

#include <ArduinoJson.h>

#include <cstddef>

// Fresh uses one process-lifetime ArduinoJson allocator so JsonDocument values
// can safely outlive the Fresh instance that created them. Allocations prefer
// external PSRAM and fall back to internal 8-bit capable memory.
ArduinoJson::Allocator &FreshJsonAllocator();

void *FreshAllocate(size_t size);
void *FreshReallocate(void *pointer, size_t newSize);
void FreshDeallocate(void *pointer);
bool FreshHasPsram();
