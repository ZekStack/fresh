#include "FreshStorage.h"

#include "Fresh.h"
#include "internal/FreshStorageContext.h"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

FreshResult FreshStorage::rename(
    const char *source,
    const char *target,
    bool replaceExisting
) {
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
	if (stat(resolvedSource.c_str(), &sourceStatus) != 0) {
		return FreshResult::failure(FreshStatus::FileSystemError, "storage rename source does not exist");
	}

	struct stat targetStatus {};
	const bool targetExists = stat(resolvedTarget.c_str(), &targetStatus) == 0;
	if (targetExists && !replaceExisting) {
		return FreshResult::failure(FreshStatus::Busy, "storage rename target already exists");
	}
	if (targetExists) {
		const int removed = S_ISDIR(targetStatus.st_mode)
		                        ? rmdir(resolvedTarget.c_str())
		                        : unlink(resolvedTarget.c_str());
		if (removed != 0) {
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to replace storage rename target");
		}
	}

	if (std::rename(resolvedSource.c_str(), resolvedTarget.c_str()) != 0) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to rename storage path");
	}
	return FreshResult::success("storage path renamed");
}
