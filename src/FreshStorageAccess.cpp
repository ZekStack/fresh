#include "Fresh.h"

#include "FreshFile.h"
#include "internal/FreshFileState.h"
#include "internal/FreshInternal.h"
#include "internal/FreshStorageAccessState.h"

#include <cstring>
#include <new>
#include <utility>

FreshStorageAccessOwner::FreshStorageAccessOwner(FreshStorage *storage)
    : _state(new (std::nothrow) FreshStorageAccessState()) {
	if (_state) _state->storage.store(storage);
}

FreshStorageAccessOwner::~FreshStorageAccessOwner() {
	if (!_state) return;
	FreshLock lock(_state->mutex);
	_state->storage.store(nullptr);
}

FreshStorageAccessOwner::FreshStorageAccessOwner(FreshStorageAccessOwner &&other) noexcept
    : _state(std::move(other._state)) {
}

void FreshStorageAccessOwner::rebind(FreshStorage *storage) {
	if (!_state) return;
	FreshLock lock(_state->mutex);
	if (lock) _state->storage.store(storage);
}

#define FRESH_REQUIRE_STORAGE(storageName)                                                        \
	std::shared_ptr<FreshStorageAccessState> accessState = _state.lock();                           \
	if (!accessState) {                                                                             \
		return FreshResult::failure(FreshStatus::NotInitialized, "storage access is detached");     \
	}                                                                                              \
	FreshLock accessLock(accessState->mutex);                                                       \
	if (!accessLock) {                                                                              \
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock storage access");   \
	}                                                                                              \
	FreshStorage *storagePointer = accessState->storage.load();                                     \
	if (storagePointer == nullptr) {                                                                \
		return FreshResult::failure(FreshStatus::NotInitialized, "storage access is detached");     \
	}                                                                                              \
	if (!storagePointer->_fileRegistry ||                                                           \
	    !storagePointer->_fileRegistry->acceptApplicationFiles.load()) {                            \
		return FreshResult::failure(FreshStatus::Busy, "database storage is stopping");             \
	}                                                                                              \
	if (!storagePointer->isMounted()) {                                                             \
		return FreshResult::failure(                                                                 \
		    FreshStatus::NotInitialized,                                                             \
		    "database storage is not initialized"                                                   \
		);                                                                                           \
	}                                                                                              \
	FreshStorage &storageName = *storagePointer

bool FreshStorageAccess::available() const {
	std::shared_ptr<FreshStorageAccessState> accessState = _state.lock();
	if (!accessState) return false;
	FreshLock accessLock(accessState->mutex);
	if (!accessLock) return false;
	FreshStorage *storage = accessState->storage.load();
	return storage != nullptr && storage->_fileRegistry &&
	       storage->_fileRegistry->acceptApplicationFiles.load() && storage->isMounted();
}

FreshResult FreshStorageAccess::open(
    const char *path,
    FreshOpenMode mode,
    FreshFile &file
) const {
	FRESH_REQUIRE_STORAGE(storage);
	return storage.open(path, mode, file);
}

bool FreshStorageAccess::exists(const char *path) const {
	bool result = false;
	return exists(path, result) && result;
}

FreshResult FreshStorageAccess::exists(const char *path, bool &result) const {
	result = false;
	FRESH_REQUIRE_STORAGE(storage);
	return storage.exists(path, result);
}

FreshResult FreshStorageAccess::fileSize(const char *path, size_t &size) const {
	size = 0;
	FreshFile file;
	FreshResult opened = open(path, FreshOpenMode::Read, file);
	if (!opened) return opened;
	size = file.size();
	return file.close();
}

FreshResult FreshStorageAccess::ensureDirectory(const char *path) const {
	if (path == nullptr || path[0] != '/' || path[1] == '\0') {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "directory path must be a non-root absolute path"
		);
	}

	std::string current;
	const size_t length = std::strlen(path);
	for (size_t index = 1; index <= length; ++index) {
		if (index != length && path[index] != '/') continue;
		if (index == 1) continue;
		current.assign(path, index);
		FreshResult created = createDirectory(current.c_str());
		if (!created) return created;
	}
	return FreshResult::success("storage directory ensured");
}

FreshResult FreshStorageAccess::createDirectory(const char *path) const {
	FRESH_REQUIRE_STORAGE(storage);
	return storage.createDirectory(path);
}

FreshResult FreshStorageAccess::removeFile(const char *path) const {
	FRESH_REQUIRE_STORAGE(storage);
	return storage.removeFile(path);
}

FreshResult FreshStorageAccess::removeDirectory(const char *path) const {
	FRESH_REQUIRE_STORAGE(storage);
	return storage.removeDirectory(path);
}

FreshResult FreshStorageAccess::rename(
    const char *source,
    const char *target,
    bool replaceExisting
) const {
	FRESH_REQUIRE_STORAGE(storage);
	return storage.rename(source, target, replaceExisting);
}

FreshResult FreshStorageAccess::listDirectory(
    const char *path,
    std::vector<FreshDirectoryEntry> &entries
) const {
	FRESH_REQUIRE_STORAGE(storage);
	FreshResult listed = storage.listDirectory(path, entries);
	if (!listed) return listed;

	const std::string directory(path == nullptr ? "" : path);
	for (FreshDirectoryEntry &entry : entries) {
		if (!entry.path.empty()) continue;
		entry.path = directory == "/" ? std::string() : directory;
		if (!entry.path.empty() && entry.path.back() != '/') entry.path.push_back('/');
		entry.path += entry.name;
		if (entry.path.empty() || entry.path.front() != '/') entry.path.insert(entry.path.begin(), '/');
	}
	return listed;
}

FreshResult FreshStorageAccess::writeFile(
    const char *path,
    const uint8_t *data,
    size_t length
) const {
	if (data == nullptr && length != 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "file data is required");
	}

	FreshFile file;
	FreshResult opened = open(path, FreshOpenMode::Write, file);
	if (!opened) return opened;

	const size_t written = length == 0 ? 0 : file.write(data, length);
	const bool writeFailed = written != length || file.getWriteError() != 0;
	FreshResult closed = file.syncAndClose();
	if (writeFailed) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write storage file");
	}
	if (!closed) return closed;
	return FreshResult::success("storage file written", written);
}

FreshResult FreshStorageAccess::readFile(
    const char *path,
    uint8_t *buffer,
    size_t capacity,
    size_t &bytesRead
) const {
	bytesRead = 0;
	if (buffer == nullptr && capacity != 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "file buffer is required");
	}

	FreshFile file;
	FreshResult opened = open(path, FreshOpenMode::Read, file);
	if (!opened) return opened;

	const size_t expectedSize = file.size();
	if (expectedSize > capacity) {
		FreshResult closed = file.close();
		if (!closed) return closed;
		return FreshResult::failure(
		    FreshStatus::SizeLimitExceeded,
		    "file buffer is too small",
		    expectedSize
		);
	}

	while (bytesRead < expectedSize) {
		const int read = file.read(buffer + bytesRead, expectedSize - bytesRead);
		if (read < 0) {
			file.close();
			bytesRead = 0;
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to read storage file");
		}
		if (read == 0) {
			file.close();
			bytesRead = 0;
			return FreshResult::failure(FreshStatus::FileSystemError, "storage file ended unexpectedly");
		}
		bytesRead += static_cast<size_t>(read);
	}
	if (file.size() != expectedSize) {
		file.close();
		bytesRead = 0;
		return FreshResult::failure(FreshStatus::Busy, "storage file changed while reading");
	}
	FreshResult closed = file.close();
	if (!closed) {
		bytesRead = 0;
		return closed;
	}
	return FreshResult::success("storage file read", bytesRead);
}

FreshResult FreshStorageAccess::info(FreshStorageInfo &result) const {
	FRESH_REQUIRE_STORAGE(storage);
	return storage.readInfo(result);
}

FreshStorageInfo FreshStorageAccess::info() const {
	FreshStorageInfo result;
	(void)info(result);
	return result;
}

FreshStorageAccess Fresh::storage() {
	FreshLock lock(*_mutex);
	if (!lock || !_storage || !_storage->_accessOwner) return FreshStorageAccess();
	return FreshStorageAccess(_storage->_accessOwner->state());
}

#undef FRESH_REQUIRE_STORAGE
