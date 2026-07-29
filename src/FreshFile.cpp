#include "FreshFile.h"

#include "Fresh.h"
#include "FreshStorage.h"
#include "internal/FreshVFSFile.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class FreshVFSFileBackend final : public FreshFileBackend {
  public:
	explicit FreshVFSFileBackend(FILE *file) : _file(file) {
	}

	~FreshVFSFileBackend() override {
		if (_file != nullptr) fclose(_file);
	}

	bool isOpen() const override {
		return _file != nullptr;
	}

	int available() override {
		if (_file == nullptr) return 0;
		const size_t current = position();
		const size_t total = size();
		if (total <= current) return 0;
		const size_t remaining = total - current;
		return remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
	}

	int read() override {
		if (_file == nullptr) return -1;
		const int value = fgetc(_file);
		if (value == EOF && ferror(_file)) _lastError = errno;
		return value;
	}

	int read(uint8_t *buffer, size_t length) override {
		if (_file == nullptr || buffer == nullptr) return -1;
		if (length == 0) return 0;
		const size_t readBytes = fread(buffer, 1, length, _file);
		if (readBytes == 0 && ferror(_file)) {
			_lastError = errno;
			return -1;
		}
		return readBytes > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(readBytes);
	}

	int peek() override {
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

	size_t write(uint8_t byte) override {
		return write(&byte, 1);
	}

	size_t write(const uint8_t *buffer, size_t length) override {
		if (_file == nullptr || buffer == nullptr || length == 0) return 0;
		const size_t written = fwrite(buffer, 1, length, _file);
		if (written != length) _lastError = errno == 0 ? EIO : errno;
		return written;
	}

	bool seek(size_t target) override {
		if (_file == nullptr || target > static_cast<size_t>(LONG_MAX)) return false;
		if (fseek(_file, static_cast<long>(target), SEEK_SET) != 0) {
			_lastError = errno;
			return false;
		}
		return true;
	}

	size_t position() const override {
		if (_file == nullptr) return 0;
		const long value = ftell(_file);
		return value < 0 ? 0 : static_cast<size_t>(value);
	}

	size_t size() const override {
		if (_file == nullptr) return 0;
		struct stat status {};
		const int descriptor = fileno(_file);
		if (descriptor < 0 || fstat(descriptor, &status) != 0 || status.st_size < 0) return 0;
		return static_cast<size_t>(status.st_size);
	}

	FreshResult sync() override {
		if (_file == nullptr) {
			return FreshResult::failure(FreshStatus::NotInitialized, "file is not open");
		}
		if (fflush(_file) != 0) {
			_lastError = errno;
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to flush file");
		}
		const int descriptor = fileno(_file);
		if (descriptor < 0) {
			_lastError = errno == 0 ? EBADF : errno;
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to resolve file descriptor");
		}
		if (fsync(descriptor) != 0) {
			_lastError = errno;
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to sync file");
		}
		return FreshResult::success("file synced");
	}

	FreshResult close() override {
		if (_file == nullptr) return FreshResult::success("file already closed");
		FILE *file = _file;
		_file = nullptr;
		if (fclose(file) != 0) {
			_lastError = errno;
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to close file");
		}
		return FreshResult::success("file closed");
	}

	int error() const override {
		return _lastError;
	}

  private:
	FILE *_file = nullptr;
	int _lastError = 0;
};

const char *FreshVFSOpenMode(FreshOpenMode mode) {
	switch (mode) {
	case FreshOpenMode::Read: return "rb";
	case FreshOpenMode::Write: return "wb";
	case FreshOpenMode::Append: return "ab";
	}
	return nullptr;
}

} // namespace

FreshResult FreshOpenVFSFile(
    const char *resolvedPath,
    FreshOpenMode mode,
    std::unique_ptr<FreshFileBackend> &backend
) {
	backend.reset();
	const char *modeString = FreshVFSOpenMode(mode);
	if (resolvedPath == nullptr || *resolvedPath == '\0' || modeString == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "invalid VFS file open request");
	}
	FILE *file = fopen(resolvedPath, modeString);
	if (file == nullptr) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open storage file");
	}
	backend.reset(new (std::nothrow) FreshVFSFileBackend(file));
	if (!backend) {
		fclose(file);
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate file backend");
	}
	return FreshResult::success("storage file opened");
}

FreshFile::~FreshFile() {
	close();
}

FreshFile::FreshFile(FreshFile &&other) noexcept
    : _backend(std::move(other._backend)), _storage(other._storage) {
	other._storage = nullptr;
}

FreshFile &FreshFile::operator=(FreshFile &&other) noexcept {
	if (this == &other) return *this;
	close();
	_backend = std::move(other._backend);
	_storage = other._storage;
	other._storage = nullptr;
	return *this;
}

FreshFile::operator bool() const {
	return _backend != nullptr && _backend->isOpen();
}

void FreshFile::attach(std::unique_ptr<FreshFileBackend> backend, FreshStorage *storage) {
	_backend = std::move(backend);
	_storage = storage;
}

int FreshFile::available() {
	return _backend != nullptr ? _backend->available() : 0;
}

int FreshFile::read() {
	return _backend != nullptr ? _backend->read() : -1;
}

int FreshFile::read(uint8_t *buffer, size_t length) {
	return _backend != nullptr ? _backend->read(buffer, length) : -1;
}

int FreshFile::peek() {
	return _backend != nullptr ? _backend->peek() : -1;
}

size_t FreshFile::write(uint8_t byte) {
	const size_t written = _backend != nullptr ? _backend->write(byte) : 0;
	if (written != 1) setWriteError(error() == 0 ? EIO : error());
	return written;
}

size_t FreshFile::write(const uint8_t *buffer, size_t length) {
	const size_t written = _backend != nullptr ? _backend->write(buffer, length) : 0;
	if (written != length) setWriteError(error() == 0 ? EIO : error());
	return written;
}

void FreshFile::flush() {
	FreshResult result = sync();
	if (!result) setWriteError(error() == 0 ? EIO : error());
}

bool FreshFile::seek(size_t target) {
	return _backend != nullptr && _backend->seek(target);
}

size_t FreshFile::position() const {
	return _backend != nullptr ? _backend->position() : 0;
}

size_t FreshFile::size() const {
	return _backend != nullptr ? _backend->size() : 0;
}

int FreshFile::error() const {
	return _backend != nullptr ? _backend->error() : 0;
}

FreshResult FreshFile::sync() {
	if (_backend == nullptr) {
		return FreshResult::failure(FreshStatus::NotInitialized, "file is not open");
	}
	return _backend->sync();
}

FreshResult FreshFile::close() {
	if (_backend == nullptr) {
		_storage = nullptr;
		return FreshResult::success("file already closed");
	}

	FreshStorage *storage = _storage;
	FreshResult result = _backend->close();
	_backend.reset();
	_storage = nullptr;
	if (storage != nullptr) storage->releaseFileHandle();
	return result;
}
