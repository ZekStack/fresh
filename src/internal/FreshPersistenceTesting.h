#pragma once

// Test-only hook. The next durable-slot serialization simulates a payload
// measurement differing from the preflight measurement. No data is modified.
#if defined(FRESH_TESTING)
void FreshTestInjectPersistedSizeMismatch();
// Fail verification once after a durable slot has been written and synced.
void FreshTestInjectDurableVerificationFailure();
#endif
