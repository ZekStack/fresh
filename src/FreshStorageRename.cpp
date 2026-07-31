#include "FreshStorage.h"

#include "Fresh.h"
#include "internal/FreshStorageAccessState.h"
#include "internal/FreshStorageContext.h"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

FreshResult FreshStorage::rename(
    const char *source,
    const char *target,
    bool replaceExisting
) {
	std::shared_ptr<FreshStorageAccessState> accessState = _accessOwner ? _accessOwner->state() : nullptr;
	if (!accessState) {
		return FreshResult::failure(FreshStatus::OutOfMemory, "storage access state is unavailable");
	}
	FreshLock operationLock(accessState->mutex);
	if (!operationLock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock storage operation");
	}

	std::string normalizedSource;
	FreshResult sourceResult = normalizeLogicalPath(source, normalizedSource);
	if (!sourceResult) return sourceResult;
	FreshResult sourceAccess = validatePathAccess(normalizedSource);
	if (!sourceAccess) return sourceAccess;

	std::string normalizedTarget;
	FreshResult targetResult = normalizeLogicalPath(target, normalizedTarget);
	if (!targetResult) return targetResult;
	FreshResult targetAccess = validatePathAccess(normalizedTarget);
	if (!targetAccess) return targetAccess;

	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	if (normalizedSource == normalizedTarget) {
		bool exists = false;
		FreshResult existsResult = existsBackend(normalizedSource.c_str(), exists);
		if (!existsResult) return existsResult;
		return exists
		           ? FreshResult::success("storage rename source and target are identical")
		           : FreshResult::failure(
		                 FreshStatus::FileSystemError,
		                 "storage rename source does not exist"
		             );
	}
	return renameBackend(
	    normalizedSource.c_str(),
	    normalizedTarget.c_str(),
	    replaceExisting
	);
}

FreshResult FreshStorage::renameBackend(
    const char *source,
    const char *target,
    bool replaceExisting
) {
	std::string resolvedSource;
	FreshResult sourceResult = resolvePath(source, resolvedSource);
	if (!sourceResult) return sourceResult;

	std::string resolvedTarget;
	FreshResult targetResult = resolvePath(target, resolvedTarget);
	if (!targetResult) return targetResult;

	struct stat sourceStatus {};
	errno = 0;
	if (stat(resolvedSource.c_str(), &sourceStatus) != 0) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    errno == ENOENT
		        ? "storage rename source does not exist"
		        : "failed to inspect storage rename source"
		);
	}

	struct stat targetStatus {};
	bool targetExists = false;
	errno = 0;
	if (stat(resolvedTarget.c_str(), &targetStatus) == 0) {
		targetExists = true;
	} else if (errno != ENOENT) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to inspect storage rename target"
		);
	}

	if (targetExists && !replaceExisting) {
		return FreshResult::failure(FreshStatus::Busy, "storage rename target already exists");
	}
	if (targetExists && S_ISDIR(sourceStatus.st_mode) != S_ISDIR(targetStatus.st_mode)) {
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "storage rename cannot replace a different path type"
		);
	}

	// Native rename is deliberately attempted without pre-deleting the target.
	// If a backend cannot replace the target safely, it must fail while leaving
	// the previous target intact.
	if (std::rename(resolvedSource.c_str(), resolvedTarget.c_str()) != 0) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to rename storage path");
	}
	return FreshResult::success("storage path renamed");
}
