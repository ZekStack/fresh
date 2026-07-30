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

class MemoryFileBackend final : public FreshFileBackend {
  public:
	MemoryFileBackend(std::vector<uint8_t> &bytes, FreshOpenMode mode)
	    : _bytes(bytes), _writable(mode != FreshOpenMode::Read) {
		_position = mode == FreshOpenMode::Append ? _bytes.size() : 0;
	}

	bool isOpen() const override {
		return _open;
	}

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
		const size_t count = std::min(size, _bytes.size() - std::min(_position, _bytes.size()));
		if (count == 0) return 0;
		std::copy_n(_bytes.data() + _position, count, buffer);
		_position += count;
		return static_cast<int>(count);
	}

	int peek() override {
		return !_open || _position >= _bytes.size() ? -1 : _bytes[_position];
	}

	size_t write(uint8_t byte) override {
		return write(&byte, 1);
	}

	size_t write(const uint8_t *buffer, size_t size) override {
		if (!_open || !_writable || buffer == nullptr) return 0;
		if (_position > _bytes.size()) _bytes.resize(_position);
		if (size > _bytes.max_size() - _position) return 0;
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

	size_t position() const override {
		return _position;
	}

	size_t size() const override {
		return _bytes.size();
	}

	FreshResult sync() override {
		return _open ? FreshResult::success("memory file synced")
		             : FreshResult::failure(FreshStatus::NotInitialized, "memory file is closed");
	}

	FreshResult close() override {
		_open = false;
		return FreshResult::success("memory file closed");
	}

	int error() const override {
		return 0;
	}

  private:
	std::vector<uint8_t> &_bytes;
	size_t _position = 0;
	bool _writable = false;
	bool _open = true;
};

class MemoryStorage final : public FreshStorage {
  public:
	MemoryStorage() : FreshStorage(FreshStorageType::Custom) {
		_directories.insert("/");
	}

	FreshResult attach() {
		setState(FreshStorageState::Mounted);
		return FreshResult::success("memory storage attached");
	}

	FreshResult detach() {
		FreshResult canDetach = validateCanUnmount();
		if (!canDetach) return canDetach;
		setState(FreshStorageState::Uninitialized);
		return FreshResult::success("memory storage detached");
	}

	void failNextInfoQuery() {
		_failNextInfoQuery = true;
	}

	const char *name() const override {
		return "MemoryStorage";
	}

	FreshStorageInfo info() const override {
		FreshStorageInfo info;
		info.type = FreshStorageType::Custom;
		info.state = state();
		info.name = name();
		info.totalBytes = 1024 * 1024;
		for (const auto &entry : _files) info.usedBytes += entry.second.size();
		info.freeBytes = info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
		return info;
	}

  private:
	static bool validPath(const char *path) {
		if (path == nullptr || path[0] != '/') return false;
		const std::string value(path);
		return value.find("/../") == std::string::npos && value != "/.." &&
		       value.find("/./") == std::string::npos && value != "/.";
	}

	static std::string parentPath(const std::string &path) {
		const size_t separator = path.find_last_of('/');
		return separator == 0 ? "/" : path.substr(0, separator);
	}

	FreshResult mount() override {
		return attach();
	}

	FreshResult unmount() override {
		return detach();
	}

	FreshResult readInfoBackend(FreshStorageInfo &result) const override {
		if (_failNextInfoQuery) {
			_failNextInfoQuery = false;
			result = FreshStorageInfo();
			return FreshResult::failure(
			    FreshStatus::FileSystemError,
			    "injected memory storage information failure"
			);
		}
		result = info();
		return FreshResult::success("memory storage information read");
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
		if (_directories.find(parentPath(path)) == _directories.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory parent directory is missing");
		}
		auto found = _files.find(path);
		if (mode == FreshOpenMode::Read && found == _files.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory file does not exist");
		}
		if (found == _files.end()) found = _files.emplace(path, std::vector<uint8_t>()).first;
		if (mode == FreshOpenMode::Write) found->second.clear();
		backend.reset(new (std::nothrow) MemoryFileBackend(found->second, mode));
		return backend ? FreshResult::success("memory file opened")
		               : FreshResult::failure(FreshStatus::OutOfMemory, "memory file allocation failed");
	}

	FreshResult existsBackend(const char *logicalPath, bool &result) const override {
		result = false;
		if (!validPath(logicalPath)) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "invalid memory path");
		}
		const std::string path(logicalPath);
		result = _files.find(path) != _files.end() || _directories.find(path) != _directories.end();
		return FreshResult::success("memory path inspected");
	}

	FreshResult createDirectoryBackend(const char *logicalPath) override {
		if (!validPath(logicalPath)) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "invalid memory directory");
		}
		const std::string path(logicalPath);
		if (_directories.find(parentPath(path)) == _directories.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory parent directory is missing");
		}
		_directories.insert(path);
		return FreshResult::success("memory directory created");
	}

	FreshResult removeFileBackend(const char *logicalPath) override {
		if (!validPath(logicalPath)) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "invalid memory file");
		}
		_files.erase(logicalPath);
		return FreshResult::success("memory file removed");
	}

	FreshResult removeDirectoryBackend(const char *logicalPath) override {
		if (!validPath(logicalPath) || std::string(logicalPath) == "/") {
			return FreshResult::failure(FreshStatus::InvalidArgument, "invalid memory directory");
		}
		const std::string prefix = std::string(logicalPath) + "/";
		for (const auto &entry : _files) {
			if (entry.first.rfind(prefix, 0) == 0) {
				return FreshResult::failure(FreshStatus::Busy, "memory directory is not empty");
			}
		}
		for (const std::string &directory : _directories) {
			if (directory != logicalPath && directory.rfind(prefix, 0) == 0) {
				return FreshResult::failure(FreshStatus::Busy, "memory directory is not empty");
			}
		}
		_directories.erase(logicalPath);
		return FreshResult::success("memory directory removed");
	}

	FreshResult listDirectoryBackend(
	    const char *logicalPath,
	    std::vector<FreshDirectoryEntry> &entries
	) const override {
		entries.clear();
		if (!validPath(logicalPath) || _directories.find(logicalPath) == _directories.end()) {
			return FreshResult::failure(FreshStatus::FileSystemError, "memory directory does not exist");
		}
		const std::string directory(logicalPath);
		const std::string prefix = directory == "/" ? "/" : directory + "/";
		for (const std::string &child : _directories) {
			if (child == directory || child.rfind(prefix, 0) != 0) continue;
			const std::string name = child.substr(prefix.size());
			if (name.empty() || name.find('/') != std::string::npos) continue;
			entries.push_back({.name = name, .isDirectory = true, .size = 0});
		}
		for (const auto &child : _files) {
			if (child.first.rfind(prefix, 0) != 0) continue;
			const std::string name = child.first.substr(prefix.size());
			if (name.empty() || name.find('/') != std::string::npos) continue;
			entries.push_back({.name = name, .isDirectory = false, .size = child.second.size()});
		}
		return FreshResult::success("memory directory listed", entries.size());
	}

	std::map<std::string, std::vector<uint8_t>> _files;
	std::set<std::string> _directories;
	mutable bool _failNextInfoQuery = false;
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

	MemoryStorage storage;
	require(storage.attach(), "attach custom storage");

	std::string documentId;
	{
		Fresh database;
		require(database.init("/fresh", storage), "initialize with caller-owned storage");
		FreshModelResult createdModel = database.createModel("settings");
		require(createdModel, "create settings model");

		JsonDocument document;
		document["name"] = "custom-storage";
		FreshResult created = createdModel.model.create(document);
		require(created, "create document");
		documentId = document["_id"].as<std::string>();
		require(!documentId.empty(), "capture document id");
		require(database.forceSync(), "persist custom storage data");
		require(database.deinit(), "deinitialize first database");
	}

	{
		Fresh database;
		require(database.init("/fresh", storage), "reinitialize custom storage");
		FreshResult found = database.model("settings").findById(documentId);
		require(found && std::string(found.doc["name"] | "") == "custom-storage", "reload document");

		FreshStorageInfo storageInfo;
		storage.failNextInfoQuery();
		FreshResult infoFailure = database.storageInfo(storageInfo);
		require(
		    !infoFailure && infoFailure.status == FreshStatus::FileSystemError,
		    "propagate custom storage information failure"
		);
		require(database.storageInfo(storageInfo), "retry custom storage information");

		FreshFile archive;
		FreshResult opened = database.withStorage(
		    [&](FreshStorage &activeStorage) -> FreshResult {
			    FreshFile forbidden;
			    FreshResult forbiddenOpen = activeStorage.open(
			        "/fresh/forbidden.bin",
			        FreshOpenMode::Write,
			        forbidden
			    );
			    require(
			        !forbiddenOpen &&
			            forbiddenOpen.status == FreshStatus::UnsupportedOperation,
			        "protect custom database root"
			    );

			    FreshResult directory = activeStorage.createDirectory("/backups");
			    if (!directory) return directory;
			    return activeStorage.open(
			        "/backups/custom.bin",
			        FreshOpenMode::Write,
			        archive
			    );
		    }
		);
		require(opened, "open custom application file");

		const uint8_t marker[] = {0x46, 0x52, 0x45, 0x53, 0x48};
		require(archive.write(marker, sizeof(marker)) == sizeof(marker), "write custom application file");

		FreshResult busy = database.deinit();
		require(
		    !busy && busy.status == FreshStatus::Busy,
		    "custom storage deinit rejects open file"
		);
		require(archive.syncAndClose(), "close custom application file");
		require(database.deinit(), "deinitialize second database");
	}

	require(storage.detach(), "detach custom storage");
	Serial.println("Custom storage conformance passed");
}

void loop() {
	delay(1000);
}
