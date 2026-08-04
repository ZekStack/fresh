#include <Fresh.h>
#include <FreshFile.h>
#include <FreshStorage.h>

#include <algorithm>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace {

struct FormatControl {
	bool supported = true;
	bool fail = false;
};

struct MemoryVolume {
	std::map<std::string, std::vector<uint8_t>> files;
	std::set<std::string> directories{"/"};
};

class MemoryFileBackend final : public FreshFileBackend {
  public:
	MemoryFileBackend(std::vector<uint8_t> &bytes, FreshOpenMode mode)
	    : _bytes(bytes), _writable(mode != FreshOpenMode::Read) {
		_position = mode == FreshOpenMode::Append ? _bytes.size() : 0;
	}

	bool isOpen() const override { return _open; }
	int available() override {
		return _open && _position < _bytes.size()
		           ? static_cast<int>(_bytes.size() - _position)
		           : 0;
	}
	int read() override {
		return !_open || _position >= _bytes.size() ? -1 : _bytes[_position++];
	}
	int read(uint8_t *buffer, size_t size) override {
		if (!_open || buffer == nullptr) return -1;
		const size_t remaining = _position < _bytes.size() ? _bytes.size() - _position : 0;
		const size_t count = std::min(size, remaining);
		if (count == 0) return 0;
		std::copy_n(_bytes.data() + _position, count, buffer);
		_position += count;
		return static_cast<int>(count);
	}
	int peek() override {
		return !_open || _position >= _bytes.size() ? -1 : _bytes[_position];
	}
	size_t write(uint8_t byte) override { return write(&byte, 1); }
	size_t write(const uint8_t *buffer, size_t size) override {
		if (!_open || !_writable || buffer == nullptr) return 0;
		if (_position + size > _bytes.size()) _bytes.resize(_position + size);
		std::copy_n(buffer, size, _bytes.data() + _position);
		_position += size;
		return size;
	}
	bool seek(size_t position) override {
		if (!_open) return false;
		_position = position;
		return true;
	}
	size_t position() const override { return _position; }
	size_t size() const override { return _bytes.size(); }
	FreshResult sync() override {
		return _open
		           ? FreshResult::success("memory file synced")
		           : FreshResult::failure(FreshStatus::NotInitialized, "memory file is closed");
	}
	FreshResult close() override {
		_open = false;
		return FreshResult::success("memory file closed");
	}
	int error() const override { return 0; }

  private:
	std::vector<uint8_t> &_bytes;
	size_t _position = 0;
	bool _writable = false;
	bool _open = true;
};

class MemoryStorage final : public FreshStorage {
  public:
	MemoryStorage(MemoryVolume &volume, FormatControl &control)
	    : FreshStorage(FreshStorageType::Custom), _volume(volume), _control(control) {
	}

	const char *name() const override { return "FormatMemoryStorage"; }
	FreshStorageInfo info() const override {
		FreshStorageInfo result;
		result.totalBytes = 1024 * 1024;
		for (const auto &entry : _volume.files) result.usedBytes += entry.second.size();
		result.freeBytes = result.totalBytes - result.usedBytes;
		return result;
	}

  private:
	static bool validPath(const char *path) {
		return path != nullptr && path[0] == '/';
	}

	static std::string parentPath(const std::string &path) {
		const size_t separator = path.find_last_of('/');
		return separator == 0 ? "/" : path.substr(0, separator);
	}

	FreshResult mount() override {
		setState(FreshStorageState::Mounted);
		return FreshResult::success("memory storage mounted");
	}

	FreshResult unmount() override {
		FreshResult canUnmount = validateCanUnmount();
		if (!canUnmount) return canUnmount;
		setState(FreshStorageState::Uninitialized);
		return FreshResult::success("memory storage unmounted");
	}

	bool supportsFormat() const override { return _control.supported; }

	FreshResult formatBackend() override {
		if (_control.fail) {
			_control.fail = false;
			return FreshResult::failure(FreshStatus::FileSystemError, "injected format failure");
		}
		_volume.files.clear();
		_volume.directories.clear();
		_volume.directories.insert("/");
		return FreshResult::success("memory storage formatted");
	}

	FreshResult openBackend(
	    const char *logicalPath,
	    FreshOpenMode mode,
	    std::unique_ptr<FreshFileBackend> &backend
	) override {
		backend.reset();
		if (!validPath(logicalPath)) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "invalid memory path");
		}
		const std::string path(logicalPath);
		if (_volume.directories.find(parentPath(path)) == _volume.directories.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory parent directory missing");
		}
		auto found = _volume.files.find(path);
		if (mode == FreshOpenMode::Read && found == _volume.files.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory file missing");
		}
		if (found == _volume.files.end()) {
			found = _volume.files.emplace(path, std::vector<uint8_t>()).first;
		}
		if (mode == FreshOpenMode::Write) found->second.clear();
		backend.reset(new (std::nothrow) MemoryFileBackend(found->second, mode));
		return backend
		           ? FreshResult::success("memory file opened")
		           : FreshResult::failure(FreshStatus::OutOfMemory, "memory file allocation failed");
	}

	FreshResult existsBackend(const char *logicalPath, bool &result) const override {
		result = false;
		if (!validPath(logicalPath)) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "invalid memory path");
		}
		const std::string path(logicalPath);
		result = _volume.files.find(path) != _volume.files.end() ||
		         _volume.directories.find(path) != _volume.directories.end();
		return FreshResult::success("memory path inspected");
	}

	FreshResult createDirectoryBackend(const char *logicalPath) override {
		if (!validPath(logicalPath)) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "invalid memory directory");
		}
		const std::string path(logicalPath);
		if (_volume.directories.find(parentPath(path)) == _volume.directories.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory parent directory missing");
		}
		_volume.directories.insert(path);
		return FreshResult::success("memory directory created");
	}

	FreshResult removeFileBackend(const char *logicalPath) override {
		_volume.files.erase(logicalPath);
		return FreshResult::success("memory file removed");
	}

	FreshResult removeDirectoryBackend(const char *logicalPath) override {
		const std::string directory(logicalPath);
		if (directory == "/") {
			return FreshResult::failure(FreshStatus::InvalidArgument, "cannot remove memory root");
		}
		const std::string prefix = directory + "/";
		for (const auto &entry : _volume.files) {
			if (entry.first.rfind(prefix, 0) == 0) {
				return FreshResult::failure(FreshStatus::Busy, "memory directory is not empty");
			}
		}
		for (const std::string &entry : _volume.directories) {
			if (entry != directory && entry.rfind(prefix, 0) == 0) {
				return FreshResult::failure(FreshStatus::Busy, "memory directory is not empty");
			}
		}
		_volume.directories.erase(directory);
		return FreshResult::success("memory directory removed");
	}

	FreshResult renameBackend(
	    const char *source,
	    const char *target,
	    bool replaceExisting
	) override {
		auto found = _volume.files.find(source);
		if (found == _volume.files.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory source missing");
		}
		if (!replaceExisting && _volume.files.find(target) != _volume.files.end()) {
			return FreshResult::failure(FreshStatus::Busy, "memory target exists");
		}
		_volume.files[target] = std::move(found->second);
		_volume.files.erase(found);
		return FreshResult::success("memory file renamed");
	}

	FreshResult listDirectoryBackend(
	    const char *logicalPath,
	    std::vector<FreshDirectoryEntry> &entries
	) const override {
		entries.clear();
		const std::string directory(logicalPath);
		if (_volume.directories.find(directory) == _volume.directories.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory directory missing");
		}
		const std::string prefix = directory == "/" ? "/" : directory + "/";
		for (const std::string &child : _volume.directories) {
			if (child == directory || child.rfind(prefix, 0) != 0) continue;
			const std::string name = child.substr(prefix.size());
			if (name.empty() || name.find('/') != std::string::npos) continue;
			entries.push_back({.name = name, .path = child, .isDirectory = true, .size = 0});
		}
		for (const auto &child : _volume.files) {
			if (child.first.rfind(prefix, 0) != 0) continue;
			const std::string name = child.first.substr(prefix.size());
			if (name.empty() || name.find('/') != std::string::npos) continue;
			entries.push_back({
			    .name = name,
			    .path = child.first,
			    .isDirectory = false,
			    .size = child.second.size()
			});
		}
		return FreshResult::success("memory directory listed", entries.size());
	}

	MemoryVolume &_volume;
	FormatControl &_control;
};

size_t passed = 0;
size_t failed = 0;

void check(bool condition, const char *label) {
	if (condition) {
		passed++;
		Serial.printf("[PASS] %s\n", label);
	} else {
		failed++;
		Serial.printf("[FAIL] %s\n", label);
	}
}

void checkResult(const FreshResult &result, const char *label) {
	if (!result) Serial.printf("       %s\n", result.message.c_str());
	check(static_cast<bool>(result), label);
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(100);
	Serial.println("Storage format regression starting");

	const uint8_t marker[] = {0x46, 0x52, 0x45, 0x53, 0x48};

	{
		MemoryVolume volume;
		FormatControl control{.supported = false};
		Fresh database;
		checkResult(
		    database.init("/fresh", FreshConfig(), MemoryStorage(volume, control)),
		    "initialize unsupported format backend"
		);
		FreshModelResult preservedModel = database.createModel("preserved");
		check(static_cast<bool>(preservedModel), "create model before unsupported format");
		checkResult(database.storage().ensureDirectory("/files"), "create application directory");
		checkResult(
		    database.storage().writeFile("/files/preserved.bin", marker, sizeof(marker)),
		    "write file before unsupported format"
		);
		FreshResult unsupported = database.format();
		check(
		    !unsupported && unsupported.status == FreshStatus::UnsupportedOperation,
		    "reject unsupported format without lifecycle disruption"
		);
		check(static_cast<bool>(database.model("preserved")), "preserve models after unsupported format");
		check(database.storage().exists("/files/preserved.bin"), "preserve files after unsupported format");
		checkResult(database.deinit(), "deinitialize unsupported backend");
	}

	MemoryVolume volume;
	FormatControl control;
	std::string postFormatId;

	{
		Fresh database;
		checkResult(
		    database.init("/fresh", FreshConfig(), MemoryStorage(volume, control)),
		    "initialize format-capable backend"
		);
		FreshModelResult oldModelResult = database.createModel("old-model");
		check(static_cast<bool>(oldModelResult), "create old model");
		FreshModel oldModel = oldModelResult.model;
		JsonDocument oldDocument;
		oldDocument["value"] = 1;
		checkResult(oldModel.create(oldDocument), "create old document");
		checkResult(database.forceSync(), "persist old database");

		FreshStorageAccess retainedStorage = database.storage();
		checkResult(retainedStorage.ensureDirectory("/application"), "create application directory");
		FreshFile openFile;
		checkResult(
		    retainedStorage.open("/application/open.bin", FreshOpenMode::Write, openFile),
		    "open application file before format"
		);
		check(openFile.write(marker, sizeof(marker)) == sizeof(marker), "write open application file");
		checkResult(retainedStorage.ensureDirectory("/unrelated"), "create unrelated directory");
		checkResult(
		    retainedStorage.writeFile("/unrelated/marker.bin", marker, sizeof(marker)),
		    "write unrelated file"
		);

		FreshResult formatted = database.format();
		checkResult(formatted, "format complete storage volume");
		check(formatted.affectedCount == 1, "report invalidated model count");
		check(!openFile, "invalidate open file handle");
		check(!oldModel, "invalidate old model handle");
		check(
		    oldModel.findById(oldDocument["_id"].as<std::string>()).status == FreshStatus::InvalidModel,
		    "old model operations fail as invalid"
		);
		check(retainedStorage.available(), "retain storage facade after successful format");
		check(!retainedStorage.exists("/application/open.bin"), "remove application file");
		check(!retainedStorage.exists("/unrelated/marker.bin"), "remove unrelated file");
		check(
		    volume.directories.count("/fresh") == 1 &&
		        volume.directories.count("/fresh/models") == 1,
		    "recreate empty database directories"
		);
		FreshModelListResult emptyModels = database.listModels();
		check(emptyModels && emptyModels.models.empty(), "restart with empty model registry");

		FreshModelResult postModel = database.createModel("post-format");
		check(static_cast<bool>(postModel), "create model after format");
		JsonDocument postDocument;
		postDocument["value"] = 2;
		checkResult(postModel.model.create(postDocument), "create document after format");
		postFormatId = postDocument["_id"].as<std::string>();
		checkResult(database.forceSync(), "persist post-format database");
		checkResult(database.deinit(), "deinitialize formatted database");
	}

	{
		Fresh database;
		checkResult(
		    database.init("/fresh", FreshConfig(), MemoryStorage(volume, control)),
		    "reinitialize formatted volume"
		);
		check(
		    static_cast<bool>(database.model("post-format").findById(postFormatId)),
		    "reload only post-format data"
		);
		checkResult(database.format(), "repeat format on initialized volume");
		check(database.listModels().models.empty(), "repeated format remains empty");
		checkResult(database.deinit(), "deinitialize repeated format database");
	}

	{
		MemoryVolume failedVolume;
		FormatControl failedControl;
		Fresh database;
		checkResult(
		    database.init("/fresh", FreshConfig(), MemoryStorage(failedVolume, failedControl)),
		    "initialize format failure backend"
		);
		FreshModel failedModel = database.createModel("stale").model;
		failedControl.fail = true;
		FreshResult formatFailure = database.format();
		check(
		    !formatFailure && formatFailure.status == FreshStatus::FileSystemError,
		    "propagate native format failure"
		);
		check(!failedModel, "invalidate model after native format failure");
		check(!database.storage().available(), "fail closed after native format failure");
		checkResult(
		    database.deinit(FreshDeinitOptions{.sync = false}),
		    "deinitialize failed format database"
		);
	}

	Serial.printf(
	    "Storage format regression complete: %u passed, %u failed\n",
	    static_cast<unsigned>(passed),
	    static_cast<unsigned>(failed)
	);
}

void loop() {
	delay(1000);
}
