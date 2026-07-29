#include "FreshStorage.h"

#include "Fresh.h"
#include "FreshFile.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

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

const char *FreshOpenModeString(FreshOpenMode mode) {
	switch (mode) {
	case FreshOpenMode::Read: return "rb";
	case FreshOpenMode::Write: return "wb";
	case FreshOpenMode::Append: return "ab";
	}
	return nullptr;
}

} // namespace

FreshStorage::FreshStorage(FreshStorageType type, const char *mountPath)
    : _type(type), _mountPath(mountPath != nullptr ? mountPath : "") {
	while (_mountPath.size() > 1 && _mountPath.back() == '/') _mountPath.pop_back();
}

FreshResult FreshStorage::resolvePath(const char *logicalPath, std::string &resolvedPath) const {
	resolvedPath.clear();
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	if (_mountPath.empty() || _mountPath.front() != '/') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "storage mount path is invalid");
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
	if (file) {
		return FreshResult::failure(FreshStatus::Busy, "destination file is already open");
	}
	std::string resolved;
	FreshResult pathResult = resolvePath(path, resolved);
	if (!pathResult) return pathResult;
	const char *modeString = FreshOpenModeString(mode);
	if (modeString == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "unsupported file mode");
	}
	FILE *handle = fopen(resolved.c_str(), modeString);
	if (handle == nullptr) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open storage file");
	}
	_openFileCount.fetch_add(1, std::memory_order_relaxed);
	file.attach(handle, this);
	return FreshResult::success("storage file opened");
}

FreshResult FreshStorage::exists(const char *path, bool &result) const {
	result = false;
	std::string resolved;
	FreshResult pathResult = resolvePath(path, resolved);
	if (!pathResult) return pathResult;
	struct stat status {};
	if (stat(resolved.c_str(), &status) == 0) {
		result = true;
		return FreshResult::success("storage path exists");
	}
	if (errno == ENOENT) return FreshResult::success("storage path does not exist");
	return FreshResult::failure(FreshStatus::FileSystemError, "failed to inspect storage path");
}

FreshResult FreshStorage::createDirectory(const char *path) {
	std::string resolved;
	FreshResult pathResult = resolvePath(path, resolved);
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

FreshResult FreshStorage::removeFile(const char *path) {
	std::string resolved;
	FreshResult pathResult = resolvePath(path, resolved);
	if (!pathResult) return pathResult;
	if (unlink(resolved.c_str()) == 0) {
		return FreshResult::success("storage file removed");
	}
	if (errno == ENOENT) return FreshResult::success("storage file not found");
	return FreshResult::failure(FreshStatus::FileSystemError, "failed to remove storage file");
}

FreshResult FreshStorage::removeDirectory(const char *path) {
	std::string resolved;
	FreshResult pathResult = resolvePath(path, resolved);
	if (!pathResult) return pathResult;
	if (rmdir(resolved.c_str()) == 0) {
		return FreshResult::success("storage directory removed");
	}
	if (errno == ENOENT) return FreshResult::success("storage directory not found");
	return FreshResult::failure(FreshStatus::FileSystemError, "failed to remove storage directory");
}

FreshResult FreshStorage::listDirectory(
    const char *path,
    std::vector<FreshDirectoryEntry> &entries
) const {
	entries.clear();
	std::string resolved;
	FreshResult pathResult = resolvePath(path, resolved);
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
		FreshDirectoryEntry result;
		result.name = entry->d_name;
		result.isDirectory = S_ISDIR(status.st_mode);
		result.size = status.st_size > 0 ? static_cast<size_t>(status.st_size) : 0;
		entries.push_back(std::move(result));
	}
	const int readError = errno;
	const int closeError = closedir(directory);
	if (readError != 0 || closeError != 0) {
		entries.clear();
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to read storage directory");
	}
	return FreshResult::success("storage directory listed", entries.size());
}

FreshResult FreshStorage::validateCanUnmount() const {
	const size_t count = openFileCount();
	if (count != 0) {
		return FreshResult::failure(FreshStatus::Busy, "storage still has open files", count);
	}
	return FreshResult::success("storage can unmount");
}

void FreshStorage::releaseFileHandle() {
	_openFileCount.fetch_sub(1, std::memory_order_relaxed);
}
