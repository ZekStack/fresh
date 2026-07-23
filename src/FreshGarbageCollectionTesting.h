#pragma once

#include <cstdint>

enum class FreshGarbageCollectionTestFailurePoint : uint8_t {
	None = 0,
	BeforeCandidateRemoval,
};

#if defined(FRESH_TESTING)
// Configures a one-shot failure used by the garbage-collection regression sketch.
void FreshTestConfigureGarbageCollectionFailure(
    FreshGarbageCollectionTestFailurePoint point
);
void FreshTestResetGarbageCollectionFailure();
#endif
