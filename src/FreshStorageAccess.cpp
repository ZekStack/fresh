#include "Fresh.h"

#include "FreshFile.h"
#include "internal/FreshInternal.h"

#include <cstring>

#define FRESH_REQUIRE_STORAGE(storageName)                                                        \
	if (_owner == nullptr) {                                                                       \
		return FreshResult::failure(FreshStatus::NotInitialized, "storage access is detached");      \
	}                                                                                              \
	FreshLock storageLock(*_owner->_mutex);                                                         \
	if (!storageLock) {                                                                             \
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database storage");  \
	}                                                                                              \
	if (!_owner->_initialized || !_owner->_storage || !_owner->_storage->isMounted()) {             \
		return FreshResult::failure(                                                                 \
		    FreshStatus::NotInitialized,                                                             \
		    "database storage is not initialized"                                                   \
		);                                                                                           \
	}                                                                                              \
	if (_owner->_stopping || _owner->_lifecycle != Fresh::Lifecycle::Running) {                     \
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");                      \
	}                                                                                              \
	FreshStorage &storageName = *_owner->_storage

bool FreshStorageAccess::available() const {
	if (_owner == nullptr) return false;
	FreshLock storageLock(*_owner->_mutex);
	return storageLock && _owner->_initialized && !_owner->_stopping &&
	       _owner->_lifecycle == Fresh::Lifecycle::Running && _owner->_storage &&
	       _owner->_storage->isMounted();
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

	while (bytesRead < capacity) {
		const int read = file.read(buffer + bytesRead, capacity - bytesRead);
		if (read < 0) {
			file.close();
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to read storage file");
		}
		if (read == 0) break;
		bytesRead += static_cast<size_t>(read);
	}
	FreshResult closed = file.close();
	if (!closed) return closed;
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
	return FreshStorageAccess(this);
}

#undef FRESH_REQUIRE_STORAGE
