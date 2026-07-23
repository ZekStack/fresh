#pragma once

#include <cstdint>

enum class FreshRestoreTestFailurePoint : uint8_t {
	None = 0,
	BeforeSnapshotWrite,
	AfterSnapshotWrite,
	BeforeManifestCommit,
	AfterManifestCommit,
	BeforeCleanup,
};

#if defined(FRESH_TESTING)
// Configures a one-shot failure used by the durable-restore regression sketch.
// AfterManifestCommit intentionally simulates a reset after the durable commit
// but before the in-memory registry switch.
void FreshTestConfigureRestoreFailure(FreshRestoreTestFailurePoint point);
void FreshTestResetRestoreFailure();
#endif
