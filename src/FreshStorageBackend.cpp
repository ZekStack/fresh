#include "FreshStorage.h"

#include "Fresh.h"
#include "FreshFile.h"
#include "internal/FreshVFSFile.h"
#include "internal/FreshStorageContext.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr size_t FreshMaxLogicalPathLength = 384;

bool FreshPathContainsTraversal(const char *path) {
	const char *segment = path;
	while (*segment != '\0') {
		while (*segment == '/') ++segment;
		const char *end = segment;
		while (*end != '\0' && *end != '/') ++end;
		const size_t length = static_cast<size_t>(end - segment);
		if ((length == 1 && segment[0] == '.') ||
		    (length == 2 && segment[0] == '.' && segment[1] == '.')) {
			return true;
		}
		segment = end;
	}
	return false;
}

} // namespace

FreshStorage::FreshStorage(FreshStorageType type, const char *mountPath)
    : _type(type), _mountPath(mountPath != nullptr ? mountPath : "") {
	while (_mountPath.size() > 1 && _mountPath.back() == '/') _mountPath.pop_back();
}

void FreshStorage::setProtectedPath(const std::string &path) {
	_protectedPath = path;
	while (_protectedPath.size() > 1 && _protectedPath.back() == '/') {
		_protectedPath.pop_back();
	}
}

FreshResult FreshStorage::validatePathAccess(const char *path) const {
	if (path == nullptr || *path == '\0') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "storage path is required");
	}
	if (_protectedPath.empty() || FreshHasInternalStorageAccess(this)) {
		return FreshResult::success("storage path allowed");
	}
	const std::string logicalPath(path);
	const bool exact = logicalPath == _protectedPath;
	const bool child = _protectedPath == "/" ||
	                   (logicalPath.size() > _protectedPath.size() &&
	                    logicalPath.compare(0, _protectedPath.size(), _protectedPath) == 0 &&
	                    logicalPath[_protectedPath.size()] == '/');
	if (exact || child) {
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "application access to the Fresh database root is forbidden"
		);
	}
	return FreshResult::success("storage path allowed");
}

FreshResult FreshStorage::resolvePath(const char *logicalPath, std::string &resolvedPath) const {
	resolvedPath.clear();
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	if (_mountPath.empty() || _mountPath.front() != '/') {
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "storage backend does not provide default VFS path operations"
		);
	}
	if (logicalPath == nullptr || *logicalPath == '\0' || logicalPath[0] != '/') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "storage path must be absolute");
	}
	const size_t logicalLength = strlen(logicalPath);
	if (logicalLength > FreshMaxLogicalPathLength) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "storage path is too long");
	}
	if (strchr(logicalPath, '\\') != nullptr || FreshPathContainsTraversal(logicalPath)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "storage path contains invalid traversal");
	}

	resolvedPath = _mountPath;
	if (strcmp(logicalPath, "/") != 0) resolvedPath += logicalPath;
	return FreshResult::success("storage path resolved");
}

FreshResult FreshStorage::open(const char *path, FreshOpenMode mode, FreshFile &file) {
	FreshResult accessResult = validatePathAccess(path);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	if (file) {
		return FreshResult::failure(FreshStatus::Busy, "destination file is already open");
	}
	std::unique_ptr<FreshFileBackend> backend;
	FreshResult result = openBackend(path, mode, backend);
	if (!result) return result;
	if (!backend || !backend->isOpen()) {
		return FreshResult::failure(FreshStatus::InternalError, "storage backend returned an invalid file");
	}
	_openFileCount.fetch_add(1);
	file.attach(std::move(backend), this);
	return FreshResult::success("storage file opened");
}

FreshResult FreshStorage::exists(const char *path, bool &result) const {
	FreshResult accessResult = validatePathAccess(path);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		result = false;
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return existsBackend(path, result);
}

FreshResult FreshStorage::createDirectory(const char *path) {
	FreshResult accessResult = validatePathAccess(path);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return createDirectoryBackend(path);
}

FreshResult FreshStorage::removeFile(const char *path) {
	FreshResult accessResult = validatePathAccess(path);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return removeFileBackend(path);
}

FreshResult FreshStorage::removeDirectory(const char *path) {
	FreshResult accessResult = validatePathAccess(path);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return removeDirectoryBackend(path);
}

FreshResult FreshStorage::listDirectory(
    const char *path,
    std::vector<FreshDirectoryEntry> &entries
) const {
	FreshResult accessResult = validatePathAccess(path);
	if (!accessResult) {
		entries.clear();
		return accessResult;
	}
	if (!isMounted()) {
		entries.clear();
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return listDirectoryBackend(path, entries);
}

FreshResult FreshStorage::openBackend(
    const char *logicalPath,
    FreshOpenMode mode,
    std::unique_ptr<FreshFileBackend> &backend
) {
	std::string resolved;
	FreshResult pathResult = resolvePath(logicalPath, resolved);
	if (!pathResult) return pathResult;
	return FreshOpenVFSFile(resolved.c_str(), mode, backend);
}

FreshResult FreshStorage::existsBackend(const char *logicalPath, bool &result) const {
	result = false;
	std::string resolved;
	FreshResult pathResult = resolvePath(logicalPath, resolved);
	if (!pathResult) return pathResult;
	struct stat status {};
	if (stat(resolved.c_str(), &status) == 0) {
		result = true;
		return FreshResult::success("storage path exists");
	}
	if (errno == ENOENT) return FreshResult::success("storage path does not exist");
	return FreshResult::failure(FreshStatus::FileSystemError, "failed to inspect storage path");
}

FreshResult FreshStorage::createDirectoryBackend(const char *logicalPath) {
	std::string resolved;
	FreshResult pathResult = resolvePath(logicalPath, resolved);
	if (!pathResult) return pathResult;
	if (mkdir(resolved.c_str(), 0755) == 0) {
		return FreshResult::success("storage directory created");
	}
	if (errno == EEXIST) {
		struct stat status {};
		if (stat(resolved.c_str(), &status) == 0 && S_ISDIR(status.st_mode)) {
			return FreshResult::success("storage directory already exists");
		}
	}
	return FreshResult::failure(FreshStatus::FileSystemError, "failed to create storage directory");
}

FreshResult FreshStorage::removeFileBackend(const char *logicalPath) {
	std::string resolved;
	FreshResult pathResult = resolvePath(logicalPath, resolved);
	if (!pathResult) return pathResult;
	if (unlink(resolved.c_str()) == 0 || errno == ENOENT) {
		return FreshResult::success("storage file removed");
	}
	return FreshResult::failure(FreshStatus::FileSystemError, "failed to remove storage file");
}

FreshResult FreshStorage::removeDirectoryBackend(const char *logicalPath) {
	std::string resolved;
	FreshResult pathResult = resolvePath(logicalPath, resolved);
	if (!pathResult) return pathResult;
	if (rmdir(resolved.c_str()) == 0 || errno == ENOENT) {
		return FreshResult::success("storage directory removed");
	}
	return FreshResult::failure(FreshStatus::FileSystemError, "failed to remove storage directory");
}

FreshResult FreshStorage::listDirectoryBackend(
    const char *logicalPath,
    std::vector<FreshDirectoryEntry> &entries
) const {
	entries.clear();
	std::string resolved;
	FreshResult pathResult = resolvePath(logicalPath, resolved);
	if (!pathResult) return pathResult;
	DIR *directory = opendir(resolved.c_str());
	if (directory == nullptr) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open storage directory");
	}

	errno = 0;
	while (dirent *entry = readdir(directory)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		std::string childPath = resolved;
		if (childPath.back() != '/') childPath.push_back('/');
		childPath += entry->d_name;
		struct stat status {};
		if (stat(childPath.c_str(), &status) != 0) {
			closedir(directory);
			entries.clear();
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to inspect directory entry");
		}
		FreshDirectoryEntry item;
		item.name = entry->d_name;
		item.isDirectory = S_ISDIR(status.st_mode);
		item.size = status.st_size > 0 ? static_cast<size_t>(status.st_size) : 0;
		entries.push_back(std::move(item));
	}
	const int readError = errno;
	const int closeError = closedir(directory);
	if (readError != 0 || closeError != 0) {
		entries.clear();
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to read storage directory");
	}
	return FreshResult::success("storage directory listed", entries.size());
}

FreshResult FreshStorage::readInfo(FreshStorageInfo &result) const {
	result = FreshStorageInfo();
	if (!isMounted()) {
		result.type = type();
		result.state = state();
		result.name = name() != nullptr ? name() : "";
		result.mountPath = mountPath() != nullptr ? mountPath() : "";
		result.nativeError = nativeError();
		result.openFileCount = openFileCount();
		return FreshResult::failure(FreshStatus::StorageUnavailable, "storage is not mounted");
	}
	FreshResult infoResult = readInfoBackend(result);
	result.type = type();
	result.state = state();
	result.name = name() != nullptr ? name() : "";
	result.mountPath = mountPath() != nullptr ? mountPath() : "";
	result.nativeError = nativeError();
	result.openFileCount = openFileCount();
	return infoResult;
}

FreshResult FreshStorage::readInfoBackend(FreshStorageInfo &result) const {
	result = info();
	return FreshResult::success("storage information read");
}

FreshResult FreshStorage::validateCanUnmount() const {
	if (openFileCount() != 0) {
		return FreshResult::failure(FreshStatus::Busy, "storage still has open files", openFileCount());
	}
	return FreshResult::success("storage can unmount");
}

void FreshStorage::releaseFileHandle() {
	size_t current = _openFileCount.load();
	while (current != 0 && !_openFileCount.compare_exchange_weak(current, current - 1)) {
	}
}
