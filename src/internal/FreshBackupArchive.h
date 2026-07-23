#pragma once

#include "../Fresh.h"

#include <Stream.h>

class FreshBackupArchiveVisitor {
  public:
	virtual ~FreshBackupArchiveVisitor() = default;

	virtual FreshResult onArchiveBegin(const FreshBackupMetadata &metadata) {
		(void)metadata;
		return FreshResult::success();
	}

	virtual FreshResult onModelBegin(const FreshBackupModelMetadata &model) {
		(void)model;
		return FreshResult::success();
	}

	virtual FreshResult onRecord(FreshModelType type, JsonDocument &&document) {
		(void)type;
		(void)document;
		return FreshResult::success();
	}

	virtual FreshResult onModelEnd(const FreshBackupModelMetadata &model) {
		(void)model;
		return FreshResult::success();
	}

	virtual FreshResult onArchiveEnd(const FreshBackupMetadata &metadata) {
		(void)metadata;
		return FreshResult::success();
	}
};

FreshResult FreshReadBackupArchive(
    Stream &input,
    size_t maxDocumentBytes,
    FreshBackupArchiveVisitor &visitor,
    FreshBackupMetadata &metadata
);
