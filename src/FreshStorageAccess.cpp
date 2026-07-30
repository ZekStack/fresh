#include "Fresh.h"

#include "internal/FreshInternal.h"

FreshResult Fresh::withStorage(FreshStorageCallback callback) {
	if (!callback) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "storage callback is required"
		);
	}

	FreshLock lock(*_mutex);
	if (!lock) {
		return FreshResult::failure(
		    FreshStatus::InternalError,
		    "failed to lock database storage"
		);
	}
	if (!_initialized || !_storage || !_storage->isMounted()) {
		return FreshResult::failure(
		    FreshStatus::NotInitialized,
		    "database storage is not initialized"
		);
	}
	if (_stopping || _lifecycle != Lifecycle::Running) {
		return FreshResult::failure(
		    FreshStatus::Busy,
		    "database is stopping"
		);
	}

	return callback(*_storage);
}
