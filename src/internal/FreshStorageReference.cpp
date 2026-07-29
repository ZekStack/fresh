#include "FreshStorageReference.h"

#include "../Fresh.h"
#include "../FreshFile.h"

#include <new>
#include <utility>

namespace {

class FreshForwardFileBackend final : public FreshFileBackend {
  public:
	explicit FreshForwardFileBackend(FreshFile &&file) : _file(std::move(file)) {
	}

	bool isOpen() const override {
		return static_cast<bool>(_file);
	}

	int available() override {
		return _file.available();
	}

	int read() override {
		return _file.read();
	}

	int read(uint8_t *buffer, size_t size) override {
		return _file.read(buffer, size);
	}

	int peek() override {
		return _file.peek();
	}

	size_t write(uint8_t byte) override {
		return _file.write(byte);
	}

	size_t write(const uint8_t *buffer, size_t size) override {
		return _file.write(buffer, size);
	}

	bool seek(size_t position) override {
		return _file.seek(position);
	}

	size_t position() const override {
		return _file.position();
	}

	size_t size() const override {
		return _file.size();
	}

	FreshResult sync() override {
		return _file.sync();
	}

	FreshResult close() override {
		return _file.close();
	}

	int error() const override {
		return _file.error();
	}

  private:
	FreshFile _file;
};

} // namespace

FreshStorageReference::FreshStorageReference(FreshStorage &target)
    : FreshStorage(FreshStorageType::Custom, target.mountPath()), _target(target) {
}

const char *FreshStorageReference::name() const {
	return _target.name();
}

FreshStorageInfo FreshStorageReference::info() const {
	return _target.info();
}

int FreshStorageReference::nativeError() const {
	return _target.nativeError();
}

FreshResult FreshStorageReference::mount() {
	if (!_target.isMounted()) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "caller-owned custom storage must be mounted before Fresh initialization"
		);
	}
	setState(FreshStorageState::Mounted);
	return FreshResult::success("caller-owned custom storage attached");
}

FreshResult FreshStorageReference::unmount() {
	FreshResult canUnmount = validateCanUnmount();
	if (!canUnmount) return canUnmount;
	setState(FreshStorageState::Uninitialized);
	return FreshResult::success("caller-owned custom storage detached");
}

FreshResult FreshStorageReference::openBackend(
    const char *logicalPath,
    FreshOpenMode mode,
    std::unique_ptr<FreshFileBackend> &backend
) {
	backend.reset();
	FreshFile file;
	FreshResult opened = _target.open(logicalPath, mode, file);
	if (!opened) return opened;

	backend.reset(new (std::nothrow) FreshForwardFileBackend(std::move(file)));
	if (!backend) {
		file.close();
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate custom file adapter");
	}
	return FreshResult::success("custom storage file opened");
}

FreshResult FreshStorageReference::existsBackend(const char *logicalPath, bool &result) const {
	return _target.exists(logicalPath, result);
}

FreshResult FreshStorageReference::createDirectoryBackend(const char *logicalPath) {
	return _target.createDirectory(logicalPath);
}

FreshResult FreshStorageReference::removeFileBackend(const char *logicalPath) {
	return _target.removeFile(logicalPath);
}

FreshResult FreshStorageReference::removeDirectoryBackend(const char *logicalPath) {
	return _target.removeDirectory(logicalPath);
}

FreshResult FreshStorageReference::listDirectoryBackend(
    const char *logicalPath,
    std::vector<FreshDirectoryEntry> &entries
) const {
	return _target.listDirectory(logicalPath, entries);
}
