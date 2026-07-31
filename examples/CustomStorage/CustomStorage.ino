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
		if (!_open || _position >= _bytes.size()) return -1;
		return _bytes[_position++];
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
	explicit MemoryStorage(MemoryVolume &volume)
	    : FreshStorage(FreshStorageType::Custom), _volume(volume) {
	}

	const char *name() const override { return "MemoryStorage"; }

	FreshStorageInfo info() const override {
		FreshStorageInfo result;
		result.totalBytes = 1024 * 1024;
		for (const auto &entry : _volume.files) result.usedBytes += entry.second.size();
		result.freeBytes = result.totalBytes - result.usedBytes;
		return result;
	}

  private:
	static bool validPath(const char *path) {
		if (path == nullptr || path[0] != '/') return false;
		const std::string value(path);
		return value.find("/../") == std::string::npos &&
		       value.find("/./") == std::string::npos &&
		       value != "/.." && value != "/.";
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
			return FreshResult::failure(FreshStatus::FileSystemError, "memory parent directory is missing");
		}
		auto found = _volume.files.find(path);
		if (mode == FreshOpenMode::Read && found == _volume.files.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory file does not exist");
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
			return FreshResult::failure(FreshStatus::FileSystemError, "memory parent directory is missing");
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
			return FreshResult::failure(FreshStatus::FileSystemError, "memory source file is missing");
		}
		if (!replaceExisting && _volume.files.find(target) != _volume.files.end()) {
			return FreshResult::failure(FreshStatus::Busy, "memory target already exists");
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
			return FreshResult::failure(FreshStatus::FileSystemError, "memory directory does not exist");
		}
		const std::string prefix = directory == "/" ? "/" : directory + "/";
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
};

void require(bool condition, const char *message) {
	if (condition) return;
	Serial.print("FAILED: ");
	Serial.println(message);
	while (true) delay(1000);
}

void require(const FreshResult &result, const char *message) {
	require(static_cast<bool>(result), message);
}

void require(const FreshModelResult &result, const char *message) {
	require(static_cast<bool>(result), message);
}

} // namespace

void setup() {
	Serial.begin(115200);

	MemoryVolume volume;
	std::string documentId;

	{
		Fresh database;
		require(
		    database.init("/fresh", FreshConfig(), MemoryStorage(volume)),
		    "initialize owned custom storage"
		);
		FreshModelResult createdModel = database.createModel("settings");
		require(createdModel, "create settings model");

		JsonDocument document;
		document["name"] = "custom-storage";
		require(createdModel.model.create(document), "create document");
		documentId = document["_id"].as<std::string>();
		require(database.forceSync(), "persist custom storage data");
		require(database.deinit(), "deinitialize first database");
	}

	{
		Fresh database;
		require(
		    database.init("/fresh", FreshConfig(), MemoryStorage(volume)),
		    "reinitialize custom storage"
		);
		FreshResult found = database.model("settings").findById(documentId);
		require(
		    found && std::string(found.doc["name"] | "") == "custom-storage",
		    "reload document"
		);

		require(database.storage().ensureDirectory("/backups"), "create backups directory");
		const uint8_t marker[] = {0x46, 0x52, 0x45, 0x53, 0x48};
		require(
		    database.storage().writeFile("/backups/custom.bin", marker, sizeof(marker)),
		    "write application file through db.storage()"
		);
		require(
		    database.storage().exists("/backups/custom.bin"),
		    "find application file through db.storage()"
		);
		require(database.deinit(), "deinitialize second database");
	}

	Serial.println("Custom storage conformance passed");
}

void loop() {
	delay(1000);
}
