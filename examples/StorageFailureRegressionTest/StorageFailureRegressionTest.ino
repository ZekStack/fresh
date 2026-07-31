#include <Arduino.h>
#include <Fresh.h>
#include <FreshFile.h>
#include <FreshStorage.h>

#include <algorithm>
#include <cerrno>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace {

struct FaultControl {
    bool shortWrite = false;
    bool syncFailure = false;
    bool closeFailure = false;
    bool readFailure = false;
    bool existsFailure = false;
};

struct FaultVolume {
    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> directories{"/"};
};

class FaultFileBackend final : public FreshFileBackend {
  public:
    FaultFileBackend(
        std::vector<uint8_t>& bytes,
        FreshOpenMode mode,
        FaultControl& control
    )
        : _bytes(bytes),
          _control(control),
          _writable(mode != FreshOpenMode::Read) {
        _position = mode == FreshOpenMode::Append ? _bytes.size() : 0;
    }

    bool isOpen() const override { return _open; }
    int available() override {
        return _open && _position < _bytes.size()
            ? static_cast<int>(_bytes.size() - _position)
            : 0;
    }
    int read() override {
        uint8_t byte = 0;
        return read(&byte, 1) == 1 ? byte : -1;
    }
    int read(uint8_t* buffer, size_t size) override {
        if (!_open || buffer == nullptr) return -1;
        if (_control.readFailure) {
            _control.readFailure = false;
            _error = EIO;
            return -1;
        }
        const size_t remaining = _position < _bytes.size()
            ? _bytes.size() - _position
            : 0;
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
    size_t write(const uint8_t* buffer, size_t size) override {
        if (!_open || !_writable || buffer == nullptr) return 0;
        size_t count = size;
        if (_control.shortWrite) {
            _control.shortWrite = false;
            count = size == 0 ? 0 : size - 1;
            _error = EIO;
        }
        if (_position + count > _bytes.size()) _bytes.resize(_position + count);
        if (count != 0) {
            std::copy_n(buffer, count, _bytes.data() + _position);
            _position += count;
        }
        return count;
    }
    bool seek(size_t position) override {
        if (!_open) return false;
        _position = position;
        return true;
    }
    size_t position() const override { return _position; }
    size_t size() const override { return _bytes.size(); }
    FreshResult sync() override {
        if (!_open) {
            return FreshResult::failure(FreshStatus::NotInitialized, "fault file is closed");
        }
        if (_control.syncFailure) {
            _control.syncFailure = false;
            _error = EIO;
            return FreshResult::failure(FreshStatus::FileSystemError, "injected sync failure");
        }
        return FreshResult::success("fault file synced");
    }
    FreshResult close() override {
        if (!_open) return FreshResult::success("fault file already closed");
        _open = false;
        if (_control.closeFailure) {
            _control.closeFailure = false;
            _error = EIO;
            return FreshResult::failure(FreshStatus::FileSystemError, "injected close failure");
        }
        return FreshResult::success("fault file closed");
    }
    int error() const override { return _error; }

  private:
    std::vector<uint8_t>& _bytes;
    FaultControl& _control;
    size_t _position = 0;
    int _error = 0;
    bool _writable = false;
    bool _open = true;
};

class FaultStorage final : public FreshStorage {
  public:
    FaultStorage(FaultVolume& volume, FaultControl& control, size_t maxOpenFiles = SIZE_MAX)
        : FreshStorage(FreshStorageType::Custom, nullptr, maxOpenFiles),
          _volume(volume),
          _control(control) {
    }

    const char* name() const override { return "FaultStorage"; }

    FreshStorageInfo info() const override {
        FreshStorageInfo result;
        result.totalBytes = 1024 * 1024;
        for (const auto& entry : _volume.files) result.usedBytes += entry.second.size();
        result.freeBytes = result.totalBytes - result.usedBytes;
        return result;
    }

  private:
    static bool validPath(const char* path) {
        if (path == nullptr || path[0] != '/') return false;
        const std::string value(path);
        return value.find("/../") == std::string::npos &&
               value.find("/./") == std::string::npos &&
               value != "/.." && value != "/.";
    }

    static std::string parentPath(const std::string& path) {
        const size_t separator = path.find_last_of('/');
        return separator == 0 ? "/" : path.substr(0, separator);
    }

    FreshResult mount() override {
        setState(FreshStorageState::Mounted);
        return FreshResult::success("fault storage mounted");
    }

    FreshResult unmount() override {
        FreshResult canUnmount = validateCanUnmount();
        if (!canUnmount) return canUnmount;
        setState(FreshStorageState::Uninitialized);
        return FreshResult::success("fault storage unmounted");
    }

    FreshResult openBackend(
        const char* logicalPath,
        FreshOpenMode mode,
        std::unique_ptr<FreshFileBackend>& backend
    ) override {
        backend.reset();
        if (!validPath(logicalPath)) {
            return FreshResult::failure(FreshStatus::InvalidArgument, "invalid fault storage path");
        }
        const std::string path(logicalPath);
        if (_volume.directories.find(parentPath(path)) == _volume.directories.end()) {
            return FreshResult::failure(FreshStatus::FileSystemError, "fault storage parent is missing");
        }
        auto found = _volume.files.find(path);
        if (mode == FreshOpenMode::Read && found == _volume.files.end()) {
            return FreshResult::failure(FreshStatus::FileSystemError, "fault storage file is missing");
        }
        if (found == _volume.files.end()) {
            found = _volume.files.emplace(path, std::vector<uint8_t>()).first;
        }
        if (mode == FreshOpenMode::Write) found->second.clear();
        backend.reset(new (std::nothrow) FaultFileBackend(found->second, mode, _control));
        return backend
            ? FreshResult::success("fault storage file opened")
            : FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate fault file");
    }

    FreshResult existsBackend(const char* path, bool& exists) const override {
        exists = false;
        if (_control.existsFailure) {
            _control.existsFailure = false;
            return FreshResult::failure(FreshStatus::FileSystemError, "injected exists failure");
        }
        if (!validPath(path)) {
            return FreshResult::failure(FreshStatus::InvalidArgument, "invalid fault storage path");
        }
        exists = _volume.files.find(path) != _volume.files.end() ||
                 _volume.directories.find(path) != _volume.directories.end();
        return FreshResult::success("fault storage path inspected");
    }

    FreshResult createDirectoryBackend(const char* path) override {
        if (!validPath(path)) {
            return FreshResult::failure(FreshStatus::InvalidArgument, "invalid fault directory");
        }
        const std::string directory(path);
        if (_volume.directories.find(parentPath(directory)) == _volume.directories.end()) {
            return FreshResult::failure(FreshStatus::FileSystemError, "fault storage parent is missing");
        }
        _volume.directories.insert(directory);
        return FreshResult::success("fault directory created");
    }

    FreshResult removeFileBackend(const char* path) override {
        _volume.files.erase(path);
        return FreshResult::success("fault file removed");
    }

    FreshResult removeDirectoryBackend(const char* path) override {
        const std::string directory(path);
        const std::string prefix = directory + "/";
        for (const auto& entry : _volume.files) {
            if (entry.first.rfind(prefix, 0) == 0) {
                return FreshResult::failure(FreshStatus::Busy, "fault directory is not empty");
            }
        }
        _volume.directories.erase(directory);
        return FreshResult::success("fault directory removed");
    }

    FreshResult listDirectoryBackend(
        const char* path,
        std::vector<FreshDirectoryEntry>& entries
    ) const override {
        entries.clear();
        const std::string directory(path);
        if (_volume.directories.find(directory) == _volume.directories.end()) {
            return FreshResult::failure(FreshStatus::FileSystemError, "fault directory is missing");
        }
        const std::string prefix = directory == "/" ? "/" : directory + "/";
        for (const auto& child : _volume.files) {
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
        return FreshResult::success("fault directory listed", entries.size());
    }

    FaultVolume& _volume;
    FaultControl& _control;
};

size_t passed = 0;
size_t failed = 0;

void check(bool condition, const char* label) {
    Serial.printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    condition ? passed++ : failed++;
}

} // namespace

void setup() {
    Serial.begin(115200);

    FaultVolume volume;
    FaultControl control;
    Fresh database;
    FreshConfig config;
    config.syncIntervalMS = 60'000;

    check(
        static_cast<bool>(database.init(
            "/fresh_failure",
            config,
            FaultStorage(volume, control)
        )),
        "initialize Fresh on owned custom fault storage"
    );
    check(
        static_cast<bool>(database.storage().ensureDirectory("/tests")),
        "create test directory"
    );

    const uint8_t payload[] = {1, 2, 3, 4};

    FreshFile shortFile;
    check(
        static_cast<bool>(database.storage().open(
            "/tests/short.bin",
            FreshOpenMode::Write,
            shortFile
        )),
        "open short-write file"
    );
    control.shortWrite = true;
    check(
        shortFile.write(payload, sizeof(payload)) != sizeof(payload) &&
            shortFile.getWriteError() != 0,
        "propagate short write"
    );
    shortFile.close();

    check(
        static_cast<bool>(database.storage().open(
            "/tests/reused.bin",
            FreshOpenMode::Write,
            shortFile
        )) && shortFile.getWriteError() == 0,
        "clear stale Print error when FreshFile is reused"
    );
    check(shortFile.write(payload, sizeof(payload)) == sizeof(payload), "write after reuse");
    shortFile.close();

    FreshFile syncFile;
    database.storage().open("/tests/sync.bin", FreshOpenMode::Write, syncFile);
    syncFile.write(payload, sizeof(payload));
    control.syncFailure = true;
    FreshResult syncFailure = syncFile.syncAndClose();
    check(
        !syncFailure && syncFailure.status == FreshStatus::FileSystemError,
        "propagate sync failure"
    );

    FreshFile closeFile;
    database.storage().open("/tests/close.bin", FreshOpenMode::Write, closeFile);
    closeFile.write(payload, sizeof(payload));
    control.closeFailure = true;
    FreshResult closeFailure = closeFile.syncAndClose();
    check(
        !closeFailure && closeFailure.status == FreshStatus::FileSystemError,
        "propagate close failure"
    );

    check(
        static_cast<bool>(database.storage().writeFile(
            "/tests/read.bin",
            payload,
            sizeof(payload)
        )),
        "prepare readable file"
    );
    FreshFile readFile;
    database.storage().open("/tests/read.bin", FreshOpenMode::Read, readFile);
    control.readFailure = true;
    uint8_t readBuffer[4] = {};
    check(
        readFile.read(readBuffer, sizeof(readBuffer)) == -1 && readFile.error() == EIO,
        "propagate unavailable read"
    );
    readFile.close();

    FreshModelResult created = database.createModel("exists_failure");
    check(static_cast<bool>(created), "create model for exists failure");
    JsonDocument document;
    document["value"] = 1;
    check(static_cast<bool>(created.model.create(document)), "create dirty document");
    control.existsFailure = true;
    FreshResult existsFailure = database.forceSync();
    check(
        !existsFailure && existsFailure.status == FreshStatus::FileSystemError,
        "propagate storage existence failure"
    );
    check(static_cast<bool>(database.forceSync()), "retry dirty sync");

    check(static_cast<bool>(database.deinit()), "deinitialize database");

    Serial.printf(
        "Storage failure regression complete: %u passed, %u failed\n",
        static_cast<unsigned>(passed),
        static_cast<unsigned>(failed)
    );
}

void loop() {
    delay(1000);
}
