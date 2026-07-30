#include "FreshStorage.h"

#include "Fresh.h"
#include "FreshFile.h"
#include "internal/FreshFileState.h"
#include "internal/FreshStorageContext.h"
#include "internal/FreshVFSFile.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr size_t FreshMaxLogicalPathLength = 384;

void FreshDecrementCounter(std::atomic<size_t> &counter) {
	size_t current = counter.load();
	while (current != 0 && !counter.compare_exchange_weak(current, current - 1)) {
	}
}

} // namespace

FreshStorage::FreshStorage(
    FreshStorageType type,
    const char *mountPath,
    size_t maxOpenFiles
)
    : _type(type),
      _mountPath(mountPath != nullptr ? mountPath : ""),
      _fileRegistry(new (std::nothrow) FreshStorageFileRegistry(maxOpenFiles)) {
	while (_mountPath.size() > 1 && _mountPath.back() == '/') _mountPath.pop_back();
}

FreshStorage::~FreshStorage() = default;

size_t FreshStorage::openFileCount() const {
	return _fileRegistry ? _fileRegistry->totalOpenFiles.load() : 0;
}

size_t FreshStorage::applicationOpenFileCount() const {
	return _fileRegistry ? _fileRegistry->applicationOpenFiles.load() : 0;
}

size_t FreshStorage::internalOpenFileCount() const {
	return _fileRegistry ? _fileRegistry->internalOpenFiles.load() : 0;
}

size_t FreshStorage::maxOpenFiles() const {
	return _fileRegistry ? _fileRegistry->maxOpenFiles : 0;
}

FreshResult FreshStorage::normalizeLogicalPath(
    const char *path,
    std::string &normalized
) const {
	normalized.clear();
	if (path == nullptr || *path == '\0') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "storage path is required");
	}
	if (path[0] != '/') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "storage path must be absolute");
	}

	const size_t length = strlen(path);
	if (length > FreshMaxLogicalPathLength) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "storage path is too long");
	}
	if (strchr(path, '\\') != nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "storage path contains a backslash");
	}

	normalized.reserve(length);
	const char *segment = path + 1;
	for (size_t index = 0; index < length; ++index) {
		const char value = path[index];
		if (value == '/' && index > 0 && path[index - 1] == '/') {
			normalized.clear();
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "storage path contains repeated separators"
			);
		}
		normalized.push_back(value);

		if (value == '/' || index + 1 == length) {
			const char *end = value == '/' ? path + index : path + index + 1;
			const size_t segmentLength = static_cast<size_t>(end - segment);
			if ((segmentLength == 1 && segment[0] == '.') ||
			    (segmentLength == 2 && segment[0] == '.' && segment[1] == '.')) {
				normalized.clear();
				return FreshResult::failure(
				    FreshStatus::InvalidArgument,
				    "storage path contains invalid traversal"
				);
			}
			segment = path + index + 1;
		}
	}

	while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
	return FreshResult::success("storage path normalized");
}

FreshResult FreshStorage::setProtectedPath(const std::string &path) {
	std::string normalized;
	FreshResult result = normalizeLogicalPath(path.c_str(), normalized);
	if (!result) return result;
	_protectedPath = std::move(normalized);
	return FreshResult::success("protected storage path configured");
}

FreshResult FreshStorage::validatePathAccess(const std::string &path) const {
	if (_protectedPath.empty() || FreshHasInternalStorageAccess(this)) {
		return FreshResult::success("storage path allowed");
	}
	const bool exact = path == _protectedPath;
	const bool child = _protectedPath == "/" ||
	                   (path.size() > _protectedPath.size() &&
	                    path.compare(0, _protectedPath.size(), _protectedPath) == 0 &&
	                    path[_protectedPath.size()] == '/');
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

	std::string normalized;
	FreshResult normalizedResult = normalizeLogicalPath(logicalPath, normalized);
	if (!normalizedResult) return normalizedResult;
	resolvedPath = _mountPath;
	if (normalized != "/") resolvedPath += normalized;
	return FreshResult::success("storage path resolved");
}

FreshResult FreshStorage::reserveFileHandle(FreshFileOrigin origin) {
	if (!_fileRegistry) {
		return FreshResult::failure(FreshStatus::OutOfMemory, "storage file registry is unavailable");
	}
	FreshLock lock(_fileRegistry->mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock storage file registry");
	}
	if (origin == FreshFileOrigin::Application &&
	    !_fileRegistry->acceptApplicationFiles.load()) {
		return FreshResult::failure(FreshStatus::Busy, "storage is shutting down");
	}
	const size_t total = _fileRegistry->totalOpenFiles.load();
	if (total >= _fileRegistry->maxOpenFiles) {
		return FreshResult::failure(
		    FreshStatus::Busy,
		    "storage open file limit reached",
		    total
		);
	}
	_fileRegistry->totalOpenFiles.store(total + 1);
	if (origin == FreshFileOrigin::Internal) {
		_fileRegistry->internalOpenFiles.fetch_add(1);
	} else {
		_fileRegistry->applicationOpenFiles.fetch_add(1);
	}
	return FreshResult::success("storage file handle reserved");
}

void FreshStorage::releaseReservedFileHandle(FreshFileOrigin origin) {
	if (!_fileRegistry) return;
	FreshLock lock(_fileRegistry->mutex);
	if (!lock) return;
	FreshDecrementCounter(_fileRegistry->totalOpenFiles);
	if (origin == FreshFileOrigin::Internal) {
		FreshDecrementCounter(_fileRegistry->internalOpenFiles);
	} else {
		FreshDecrementCounter(_fileRegistry->applicationOpenFiles);
	}
}

FreshResult FreshStorage::registerFileState(
    const std::shared_ptr<FreshFileState> &state
) {
	if (!_fileRegistry || !state) {
		return FreshResult::failure(FreshStatus::InternalError, "invalid storage file state");
	}
	FreshLock lock(_fileRegistry->mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock storage file registry");
	}
	for (auto iterator = _fileRegistry->files.begin(); iterator != _fileRegistry->files.end();) {
		if (iterator->expired()) {
			iterator = _fileRegistry->files.erase(iterator);
		} else {
			++iterator;
		}
	}
	_fileRegistry->files.push_back(state);
	return FreshResult::success("storage file state registered");
}

void FreshStorage::setApplicationFileAcceptance(bool accepted) {
	if (_fileRegistry) _fileRegistry->acceptApplicationFiles.store(accepted);
}

FreshResult FreshStorage::closeApplicationFiles(bool syncBeforeClose) {
	if (!_fileRegistry) return FreshResult::success("storage file registry is empty");
	std::vector<std::shared_ptr<FreshFileState>> files;
	{
		FreshLock lock(_fileRegistry->mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock storage file registry");
		}
		for (const std::weak_ptr<FreshFileState> &weak : _fileRegistry->files) {
			std::shared_ptr<FreshFileState> state = weak.lock();
			if (state && state->origin == FreshFileOrigin::Application) {
				files.push_back(std::move(state));
			}
		}
	}

	FreshResult firstFailure = FreshResult::success("application files closed");
	for (const std::shared_ptr<FreshFileState> &state : files) {
		FreshResult closed = FreshCloseFileState(state, syncBeforeClose);
		if (!closed && firstFailure) firstFailure = closed;
	}
	return firstFailure;
}

FreshResult FreshStorage::closeAllFiles(bool syncBeforeClose) {
	if (!_fileRegistry) return FreshResult::success("storage file registry is empty");
	std::vector<std::shared_ptr<FreshFileState>> files;
	{
		FreshLock lock(_fileRegistry->mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock storage file registry");
		}
		for (const std::weak_ptr<FreshFileState> &weak : _fileRegistry->files) {
			std::shared_ptr<FreshFileState> state = weak.lock();
			if (state) files.push_back(std::move(state));
		}
	}

	FreshResult firstFailure = FreshResult::success("storage files closed");
	for (const std::shared_ptr<FreshFileState> &state : files) {
		FreshResult closed = FreshCloseFileState(state, syncBeforeClose);
		if (!closed && firstFailure) firstFailure = closed;
	}
	return firstFailure;
}

FreshResult FreshStorage::open(const char *path, FreshOpenMode mode, FreshFile &file) {
	std::string normalized;
	FreshResult pathResult = normalizeLogicalPath(path, normalized);
	if (!pathResult) return pathResult;
	FreshResult accessResult = validatePathAccess(normalized);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	if (file) {
		return FreshResult::failure(FreshStatus::Busy, "destination file is already open");
	}

	const FreshFileOrigin origin = FreshHasInternalStorageAccess(this)
	                                   ? FreshFileOrigin::Internal
	                                   : FreshFileOrigin::Application;
	FreshResult reserved = reserveFileHandle(origin);
	if (!reserved) return reserved;

	std::unique_ptr<FreshFileBackend> backend;
	FreshResult result = openBackend(normalized.c_str(), mode, backend);
	if (!result) {
		releaseReservedFileHandle(origin);
		return result;
	}
	if (!backend || !backend->isOpen()) {
		releaseReservedFileHandle(origin);
		return FreshResult::failure(FreshStatus::InternalError, "storage backend returned an invalid file");
	}

	std::shared_ptr<FreshFileState> state(new (std::nothrow) FreshFileState());
	if (!state) {
		backend->close();
		releaseReservedFileHandle(origin);
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate storage file state");
	}
	state->backend = std::move(backend);
	state->registry = _fileRegistry;
	state->origin = origin;
	state->registered = true;
	FreshResult registered = registerFileState(state);
	if (!registered) {
		state->registered = false;
		state->backend->close();
		state->backend.reset();
		releaseReservedFileHandle(origin);
		return registered;
	}
	file.attach(std::move(state));
	return FreshResult::success("storage file opened");
}

FreshResult FreshStorage::exists(const char *path, bool &result) const {
	std::string normalized;
	FreshResult pathResult = normalizeLogicalPath(path, normalized);
	if (!pathResult) {
		result = false;
		return pathResult;
	}
	FreshResult accessResult = validatePathAccess(normalized);
	if (!accessResult) {
		result = false;
		return accessResult;
	}
	if (!isMounted()) {
		result = false;
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return existsBackend(normalized.c_str(), result);
}

FreshResult FreshStorage::createDirectory(const char *path) {
	std::string normalized;
	FreshResult pathResult = normalizeLogicalPath(path, normalized);
	if (!pathResult) return pathResult;
	FreshResult accessResult = validatePathAccess(normalized);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return createDirectoryBackend(normalized.c_str());
}

FreshResult FreshStorage::removeFile(const char *path) {
	std::string normalized;
	FreshResult pathResult = normalizeLogicalPath(path, normalized);
	if (!pathResult) return pathResult;
	FreshResult accessResult = validatePathAccess(normalized);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return removeFileBackend(normalized.c_str());
}

FreshResult FreshStorage::removeDirectory(const char *path) {
	std::string normalized;
	FreshResult pathResult = normalizeLogicalPath(path, normalized);
	if (!pathResult) return pathResult;
	FreshResult accessResult = validatePathAccess(normalized);
	if (!accessResult) return accessResult;
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return removeDirectoryBackend(normalized.c_str());
}

FreshResult FreshStorage::listDirectory(
    const char *path,
    std::vector<FreshDirectoryEntry> &entries
) const {
	std::string normalized;
	FreshResult pathResult = normalizeLogicalPath(path, normalized);
	if (!pathResult) {
		entries.clear();
		return pathResult;
	}
	FreshResult accessResult = validatePathAccess(normalized);
	if (!accessResult) {
		entries.clear();
		return accessResult;
	}
	if (!isMounted()) {
		entries.clear();
		return FreshResult::failure(FreshStatus::NotInitialized, "storage is not mounted");
	}
	return listDirectoryBackend(normalized.c_str(), entries);
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
		result.applicationOpenFileCount = applicationOpenFileCount();
		result.internalOpenFileCount = internalOpenFileCount();
		result.maxOpenFiles = maxOpenFiles();
		return FreshResult::failure(FreshStatus::StorageUnavailable, "storage is not mounted");
	}
	FreshResult infoResult = readInfoBackend(result);
	result.type = type();
	result.state = state();
	result.name = name() != nullptr ? name() : "";
	result.mountPath = mountPath() != nullptr ? mountPath() : "";
	result.nativeError = nativeError();
	result.openFileCount = openFileCount();
	result.applicationOpenFileCount = applicationOpenFileCount();
	result.internalOpenFileCount = internalOpenFileCount();
	result.maxOpenFiles = maxOpenFiles();
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
