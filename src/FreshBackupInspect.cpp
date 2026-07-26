#include "Fresh.h"
#include "internal/FreshBackupArchive.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshCooperativeStream.h"
#include "internal/FreshInternal.h"

#include <utility>

FreshResult Fresh::inspectBackup(Stream &input, FreshBackupMetadata &metadata) const {
	size_t maxDocumentBytes = 0;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			metadata = FreshBackupMetadata();
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized) {
			metadata = FreshBackupMetadata();
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping || _lifecycle != Lifecycle::Running) {
			metadata = FreshBackupMetadata();
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		maxDocumentBytes = _config.maxDocumentBytes;
	}

	FreshBackupArchiveVisitor visitor;
	FreshBackupMetadata parsed;
	FreshCooperativeStream cooperativeInput(input);
	FreshResult result = FreshReadBackupArchive(
	    cooperativeInput,
	    maxDocumentBytes,
	    visitor,
	    parsed
	);
	if (!result) {
		metadata = FreshBackupMetadata();
		return result;
	}
	metadata = std::move(parsed);
	return FreshResult::success("backup inspected", metadata.modelCount);
}

FreshResult Fresh::inspectBackup(
    const uint8_t *data,
    size_t length,
    FreshBackupMetadata &metadata
) const {
	if (data == nullptr || length == 0) {
		metadata = FreshBackupMetadata();
		return FreshResult::failure(FreshStatus::InvalidArgument, "backup buffer is required");
	}
	fresh_backup_v2::MemoryStream input(data, length);
	return inspectBackup(input, metadata);
}
