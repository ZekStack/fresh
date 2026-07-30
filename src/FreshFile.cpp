#include "FreshFile.h"

#include "Fresh.h"
#include "internal/FreshFileState.h"
#include "internal/FreshVFSFile.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

class FreshVFSFileBackend final : public FreshFileBackend {
  public:
	FreshVFSFileBackend(FILE *file, size_t initialSize)
	    : _file(file), _size(initialSize) {
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
		if (written > 0) {
			const long current = ftell(_file);
			if (current >= 0) {
				const size_t end = static_cast<size_t>(current);
				if (end > _size) _size = end;
			}
		}
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
		return _file != nullptr ? _size : 0;
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
	size_t _size = 0;
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

void FreshDecrement(std::atomic<size_t> &counter) {
	size_t current = counter.load();
	while (current != 0 && !counter.compare_exchange_weak(current, current - 1)) {
	}
}

void FreshReleaseFileRegistration(const std::shared_ptr<FreshFileState> &state) {
	if (!state || !state->registered) return;
	std::shared_ptr<FreshStorageFileRegistry> registry = state->registry.lock();
	if (!registry) {
		state->registered = false;
		return;
	}
	FreshLock registryLock(registry->mutex);
	if (!registryLock || !state->registered) return;
	state->registered = false;
	FreshDecrement(registry->totalOpenFiles);
	if (state->origin == FreshFileOrigin::Internal) {
		FreshDecrement(registry->internalOpenFiles);
	} else {
		FreshDecrement(registry->applicationOpenFiles);
	}
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

	struct stat status {};
	const int descriptor = fileno(file);
	if (descriptor < 0 || fstat(descriptor, &status) != 0 || status.st_size < 0) {
		fclose(file);
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to inspect storage file");
	}
	const size_t initialSize = static_cast<size_t>(status.st_size);
	backend.reset(new (std::nothrow) FreshVFSFileBackend(file, initialSize));
	if (!backend) {
		fclose(file);
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate file backend");
	}
	return FreshResult::success("storage file opened");
}

FreshResult FreshCloseFileState(
    const std::shared_ptr<FreshFileState> &state,
    bool syncBeforeClose
) {
	if (!state) return FreshResult::success("file already closed");
	FreshLock lock(state->mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock file state");
	}

	FreshResult syncResult = FreshResult::success("file sync not requested");
	FreshResult closeResult = FreshResult::success("file already closed");
	if (state->backend) {
		if (syncBeforeClose && state->backend->isOpen()) {
			syncResult = state->backend->sync();
		}
		closeResult = state->backend->close();
		state->lastError = state->backend->error();
		state->backend.reset();
	}
	FreshReleaseFileRegistration(state);
	return syncResult ? closeResult : syncResult;
}

FreshFile::~FreshFile() {
	close();
}

FreshFile::FreshFile(FreshFile &&other) noexcept : _state(std::move(other._state)) {
	const int writeError = other.getWriteError();
	clearWriteError();
	if (writeError != 0) setWriteError(writeError);
	other.clearWriteError();
}

FreshFile &FreshFile::operator=(FreshFile &&other) noexcept {
	if (this == &other) return *this;
	close();
	const int writeError = other.getWriteError();
	_state = std::move(other._state);
	clearWriteError();
	if (writeError != 0) setWriteError(writeError);
	other.clearWriteError();
	return *this;
}

FreshFile::operator bool() const {
	if (!_state) return false;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr && _state->backend->isOpen();
}

void FreshFile::attach(std::shared_ptr<FreshFileState> state) {
	if (_state) close();
	_state = std::move(state);
	clearWriteError();
}

int FreshFile::available() {
	if (!_state) return 0;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr ? _state->backend->available() : 0;
}

int FreshFile::read() {
	if (!_state) return -1;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr ? _state->backend->read() : -1;
}

int FreshFile::read(uint8_t *buffer, size_t length) {
	if (!_state) return -1;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr ? _state->backend->read(buffer, length) : -1;
}

int FreshFile::peek() {
	if (!_state) return -1;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr ? _state->backend->peek() : -1;
}

size_t FreshFile::write(uint8_t byte) {
	if (!_state) {
		setWriteError(EBADF);
		return 0;
	}
	FreshLock lock(_state->mutex);
	if (!lock || _state->backend == nullptr) {
		setWriteError(EBADF);
		return 0;
	}
	const size_t written = _state->backend->write(byte);
	if (written != 1) {
		const int backendError = _state->backend->error();
		setWriteError(backendError == 0 ? EIO : backendError);
	}
	return written;
}

size_t FreshFile::write(const uint8_t *buffer, size_t length) {
	if (!_state) {
		if (length != 0) setWriteError(EBADF);
		return 0;
	}
	FreshLock lock(_state->mutex);
	if (!lock || _state->backend == nullptr) {
		if (length != 0) setWriteError(EBADF);
		return 0;
	}
	const size_t written = _state->backend->write(buffer, length);
	if (written != length) {
		const int backendError = _state->backend->error();
		setWriteError(backendError == 0 ? EIO : backendError);
	}
	return written;
}

void FreshFile::flush() {
	FreshResult result = sync();
	if (!result) setWriteError(error() == 0 ? EIO : error());
}

bool FreshFile::seek(size_t target) {
	if (!_state) return false;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr && _state->backend->seek(target);
}

size_t FreshFile::position() const {
	if (!_state) return 0;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr ? _state->backend->position() : 0;
}

size_t FreshFile::size() const {
	if (!_state) return 0;
	FreshLock lock(_state->mutex);
	return lock && _state->backend != nullptr ? _state->backend->size() : 0;
}

int FreshFile::error() const {
	if (!_state) return 0;
	FreshLock lock(_state->mutex);
	if (!lock) return EBUSY;
	return _state->backend != nullptr ? _state->backend->error() : _state->lastError;
}

FreshResult FreshFile::sync() {
	if (!_state) {
		return FreshResult::failure(FreshStatus::NotInitialized, "file is not open");
	}
	FreshLock lock(_state->mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock file state");
	}
	if (_state->backend == nullptr) {
		return FreshResult::failure(FreshStatus::NotInitialized, "file is not open");
	}
	return _state->backend->sync();
}

FreshResult FreshFile::close() {
	if (!_state) return FreshResult::success("file already closed");
	FreshResult result = FreshCloseFileState(_state, false);
	_state.reset();
	return result;
}

FreshResult FreshFile::syncAndClose() {
	if (!_state) {
		return FreshResult::failure(FreshStatus::NotInitialized, "file is not open");
	}
	FreshResult result = FreshCloseFileState(_state, true);
	_state.reset();
	return result;
}
