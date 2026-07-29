#include "FreshFile.h"

#include "Fresh.h"
#include "FreshStorage.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

FreshFile::~FreshFile() {
	close();
}

FreshFile::FreshFile(FreshFile &&other) noexcept
    : _file(other._file), _storage(other._storage), _lastError(other._lastError) {
	other._file = nullptr;
	other._storage = nullptr;
	other._lastError = 0;
}

FreshFile &FreshFile::operator=(FreshFile &&other) noexcept {
	if (this == &other) return *this;
	close();
	_file = other._file;
	_storage = other._storage;
	_lastError = other._lastError;
	other._file = nullptr;
	other._storage = nullptr;
	other._lastError = 0;
	return *this;
}

void FreshFile::attach(FILE *file, FreshStorage *storage) {
	_file = file;
	_storage = storage;
	_lastError = 0;
}

int FreshFile::available() {
	if (_file == nullptr) return 0;
	const size_t current = position();
	const size_t total = size();
	if (total <= current) return 0;
	const size_t remaining = total - current;
	return remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
}

int FreshFile::read() {
	if (_file == nullptr) return -1;
	const int value = fgetc(_file);
	if (value == EOF && ferror(_file)) _lastError = errno;
	return value;
}

int FreshFile::read(uint8_t *buffer, size_t size) {
	if (_file == nullptr || buffer == nullptr) return -1;
	if (size == 0) return 0;
	const size_t readBytes = fread(buffer, 1, size, _file);
	if (readBytes == 0 && ferror(_file)) {
		_lastError = errno;
		return -1;
	}
	return readBytes > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(readBytes);
}

int FreshFile::peek() {
	if (_file == nullptr) return -1;
	const int value = fgetc(_file);
	if (value == EOF) {
		if (ferror(_file)) _lastError = errno;
		return -1;
	}
	if (ungetc(value, _file) == EOF) {
		_lastError = errno;
		return -1;
	}
	return value;
}

size_t FreshFile::write(uint8_t byte) {
	return write(&byte, 1);
}

size_t FreshFile::write(const uint8_t *buffer, size_t size) {
	if (_file == nullptr || buffer == nullptr || size == 0) return 0;
	const size_t written = fwrite(buffer, 1, size, _file);
	if (written != size) {
		_lastError = errno;
		setWriteError(_lastError == 0 ? EIO : _lastError);
	}
	return written;
}

void FreshFile::flush() {
	FreshResult result = sync();
	if (!result) setWriteError(_lastError == 0 ? EIO : _lastError);
}

bool FreshFile::seek(size_t position) {
	if (_file == nullptr || position > static_cast<size_t>(LONG_MAX)) return false;
	if (fseek(_file, static_cast<long>(position), SEEK_SET) != 0) {
		_lastError = errno;
		return false;
	}
	return true;
}

size_t FreshFile::position() const {
	if (_file == nullptr) return 0;
	const long value = ftell(_file);
	return value < 0 ? 0 : static_cast<size_t>(value);
}

size_t FreshFile::size() const {
	if (_file == nullptr) return 0;
	struct stat status {};
	const int descriptor = fileno(_file);
	if (descriptor < 0 || fstat(descriptor, &status) != 0 || status.st_size < 0) return 0;
	return static_cast<size_t>(status.st_size);
}

FreshResult FreshFile::sync() {
	if (_file == nullptr) {
		return FreshResult::failure(FreshStatus::NotInitialized, "file is not open");
	}
	if (fflush(_file) != 0) {
		_lastError = errno;
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to flush file");
	}
	const int descriptor = fileno(_file);
	if (descriptor < 0) {
		_lastError = errno;
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to resolve file descriptor");
	}
	if (fsync(descriptor) != 0) {
		_lastError = errno;
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to sync file");
	}
	return FreshResult::success("file synced");
}

FreshResult FreshFile::close() {
	if (_file == nullptr) {
		_storage = nullptr;
		return FreshResult::success("file already closed");
	}

	FILE *file = _file;
	FreshStorage *storage = _storage;
	_file = nullptr;
	_storage = nullptr;
	const int closeResult = fclose(file);
	if (storage != nullptr) storage->releaseFileHandle();
	if (closeResult != 0) {
		_lastError = errno;
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to close file");
	}
	return FreshResult::success("file closed");
}
