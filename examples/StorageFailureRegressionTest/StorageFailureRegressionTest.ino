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

struct FaultState {
    bool shortWrite = false;
    bool syncFailure = false;
    bool closeFailure = false;
    bool readFailure = false;
};

class FaultFileBackend final : public FreshFileBackend {
  public:
    FaultFileBackend(
        std::vector<uint8_t>& bytes,
        FreshOpenMode mode,
        FaultState& faults
    )
        : _bytes(bytes),
          _faults(faults),
          _writable(mode != FreshOpenMode::Read) {
        _position = mode == FreshOpenMode::Append ? _bytes.size() : 0;
    }

    bool isOpen() const override {
        return _open;
    }

    int available() override {
        if (!_open || _position >= _bytes.size()) return 0;
        return static_cast<int>(_bytes.size() - _position);
    }

    int read() override {
        uint8_t byte = 0;
        return read(&byte, 1) == 1 ? byte : -1;
    }

    int read(uint8_t* buffer, size_t size) override {
        if (!_open || buffer == nullptr) return -1;
        if (_faults.readFailure) {
            _faults.readFailure = false;
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
        if (!_open || _position >= _bytes.size()) return -1;
        return _bytes[_position];
    }

    size_t write(uint8_t byte) override {
        return write(&byte, 1);
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        if (!_open || !_writable || buffer == nullptr) return 0;
        size_t count = size;
        if (_faults.shortWrite) {
            _faults.shortWrite = false;
            count = size == 0 ? 0 : size - 1;
            _error = EIO;
        }
        if (_position > _bytes.size()) _bytes.resize(_position);
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

    size_t position() const override {
        return _position;
    }

    size_t size() const override {
        return _bytes.size();
    }

    FreshResult sync() override {
        if (!_open) {
            return FreshResult::failure(
                FreshStatus::NotInitialized,
                "fault file is closed"
            );
        }
        if (_faults.syncFailure) {
            _faults.syncFailure = false;
            _error = EIO;
            return FreshResult::failure(
                FreshStatus::FileSystemError,
                "injected sync failure"
            );
        }
        return FreshResult::success("fault file synced");
    }

    FreshResult close() override {
        if (!_open) return FreshResult::success("fault file already closed");
        _open = false;
        if (_faults.closeFailure) {
            _faults.closeFailure = false;
            _error = EIO;
            return FreshResult::failure(
                FreshStatus::FileSystemError,
                "injected close failure"
            );
        }
        return FreshResult::success("fault file closed");
    }

    int error() const override {
        return _error;
    }

  private:
    std::vector<uint8_t>& _bytes;
    FaultState& _faults;
    size_t _position = 0;
    int _error = 0;
    bool _writable = false;
    bool _open = true;
};

class FaultStorage final : public FreshStorage {
  public:
    FaultStorage() : FreshStorage(FreshStorageType::Custom) {
        _directories.insert("/");
    }

    FreshResult attach() {
        setState(FreshStorageState::Mounted);
        return FreshResult::success("fault storage attached");
    }

    FreshResult detach() {
        FreshResult canDetach = validateCanUnmount();
        if (!canDetach) return canDetach;
        setState(FreshStorageState::Uninitialized);
        return FreshResult::success("fault storage detached");
    }

    void failNextWrite() {
        _faults.shortWrite = true;
    }

    void failNextSync() {
        _faults.syncFailure = true;
    }

    void failNextClose() {
        _faults.closeFailure = true;
    }

    void failNextRead() {
        _faults.readFailure = true;
    }

    const char* name() const override {
        return "FaultStorage";
    }

    FreshStorageInfo info() const override {
        FreshStorageInfo result;
        result.totalBytes = 1024 * 1024;
        for (const auto& entry : _files) result.usedBytes += entry.second.size();
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
        return attach();
    }

    FreshResult unmount() override {
        return detach();
    }

    FreshResult openBackend(
        const char* logicalPath,
        FreshOpenMode mode,
        std::unique_ptr<FreshFileBackend>& backend
    ) override {
        backend.reset();
        if (!validPath(logicalPath)) {
            return FreshResult::failure(
                FreshStatus::InvalidArgument,
                "invalid fault storage path"
            );
        }
        const std::string path(logicalPath);
        if (_directories.find(parentPath(path)) == _directories.end()) {
            return FreshResult::failure(
                FreshStatus::FileSystemError,
                "fault storage parent directory is missing"
            );
        }
        auto found = _files.find(path);
        if (mode == FreshOpenMode::Read && found == _files.end()) {
            return FreshResult::failure(
                FreshStatus::FileSystemError,
                "fault storage file does not exist"
            );
        }
        if (found == _files.end()) {
            found = _files.emplace(path, std::vector<uint8_t>()).first;
        }
        if (mode == FreshOpenMode::Write) found->second.clear();
        backend.reset(new (std::nothrow) FaultFileBackend(
            found->second,
            mode,
            _faults
        ));
        return backend
            ? FreshResult::success("fault storage file opened")
            : FreshResult::failure(
                FreshStatus::OutOfMemory,
                "failed to allocate fault file"
            );
    }

    FreshResult existsBackend(const char* path, bool& exists) const override {
        exists = false;
        if (!validPath(path)) {
            return FreshResult::failure(
                FreshStatus::InvalidArgument,
                "invalid fault storage path"
            );
        }
        exists = _files.find(path) != _files.end() ||
                 _directories.find(path) != _directories.end();
        return FreshResult::success("fault storage path inspected");
    }

    FreshResult createDirectoryBackend(const char* path) override {
        if (!validPath(path)) {
            return FreshResult::failure(
                FreshStatus::InvalidArgument,
                "invalid fault storage directory"
            );
        }
        const std::string directory(path);
        if (_directories.find(parentPath(directory)) == _directories.end()) {
            return FreshResult::failure(
                FreshStatus::FileSystemError,
                "fault storage parent directory is missing"
            );
        }
        _directories.insert(directory);
        return FreshResult::success("fault storage directory created");
    }

    FreshResult removeFileBackend(const char* path) override {
        _files.erase(path);
        return FreshResult::success("fault storage file removed");
    }

    FreshResult removeDirectoryBackend(const char* path) override {
        const std::string directory(path);
        const std::string prefix = directory + "/";
        for (const auto& entry : _files) {
            if (entry.first.rfind(prefix, 0) == 0) {
                return FreshResult::failure(
                    FreshStatus::Busy,
                    "fault storage directory is not empty"
                );
            }
        }
        for (const std::string& entry : _directories) {
            if (entry != directory && entry.rfind(prefix, 0) == 0) {
                return FreshResult::failure(
                    FreshStatus::Busy,
                    "fault storage directory is not empty"
                );
            }
        }
        _directories.erase(directory);
        return FreshResult::success("fault storage directory removed");
    }

    FreshResult listDirectoryBackend(
        const char* path,
        std::vector<FreshDirectoryEntry>& entries
    ) const override {
        entries.clear();
        const std::string directory(path);
        if (_directories.find(directory) == _directories.end()) {
            return FreshResult::failure(
                FreshStatus::FileSystemError,
                "fault storage directory does not exist"
            );
        }
        const std::string prefix = directory == "/" ? "/" : directory + "/";
        for (const std::string& child : _directories) {
            if (child == directory || child.rfind(prefix, 0) != 0) continue;
            const std::string name = child.substr(prefix.size());
            if (!name.empty() && name.find('/') == std::string::npos) {
                entries.push_back({.name = name, .isDirectory = true, .size = 0});
            }
        }
        for (const auto& child : _files) {
            if (child.first.rfind(prefix, 0) != 0) continue;
            const std::string name = child.first.substr(prefix.size());
            if (!name.empty() && name.find('/') == std::string::npos) {
                entries.push_back({
                    .name = name,
                    .isDirectory = false,
                    .size = child.second.size()
                });
            }
        }
        return FreshResult::success("fault storage directory listed");
    }

    FaultState _faults;
    std::map<std::string, std::vector<uint8_t>> _files;
    std::set<std::string> _directories;
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

    FaultStorage storage;
    check(static_cast<bool>(storage.attach()), "attach fault storage");

    Fresh database;
    check(
        static_cast<bool>(database.init("/fresh_failure", storage)),
        "initialize Fresh on custom fault storage"
    );

    FreshResult directory = database.withStorage(
        [](FreshStorage& activeStorage) {
            return activeStorage.createDirectory("/tests");
        }
    );
    check(static_cast<bool>(directory), "create test directory");

    FreshFile shortFile;
    check(
        static_cast<bool>(database.withStorage(
            [&](FreshStorage& activeStorage) {
                return activeStorage.open(
                    "/tests/short.bin",
                    FreshOpenMode::Write,
                    shortFile
                );
            }
        )),
        "open short-write file"
    );
    storage.failNextWrite();
    const uint8_t payload[] = {1, 2, 3, 4};
    check(
        shortFile.write(payload, sizeof(payload)) != sizeof(payload) &&
            shortFile.getWriteError() != 0,
        "propagate short write"
    );
    shortFile.close();

    FreshFile syncFile;
    database.withStorage([&](FreshStorage& activeStorage) {
        return activeStorage.open(
            "/tests/sync.bin",
            FreshOpenMode::Write,
            syncFile
        );
    });
    syncFile.write(payload, sizeof(payload));
    storage.failNextSync();
    FreshResult syncFailure = syncFile.syncAndClose();
    check(
        !syncFailure && syncFailure.status == FreshStatus::FileSystemError,
        "propagate sync failure"
    );

    FreshFile closeFile;
    database.withStorage([&](FreshStorage& activeStorage) {
        return activeStorage.open(
            "/tests/close.bin",
            FreshOpenMode::Write,
            closeFile
        );
    });
    closeFile.write(payload, sizeof(payload));
    storage.failNextClose();
    FreshResult closeFailure = closeFile.syncAndClose();
    check(
        !closeFailure && closeFailure.status == FreshStatus::FileSystemError,
        "propagate close failure"
    );

    FreshFile writeReadFile;
    database.withStorage([&](FreshStorage& activeStorage) {
        return activeStorage.open(
            "/tests/read.bin",
            FreshOpenMode::Write,
            writeReadFile
        );
    });
    writeReadFile.write(payload, sizeof(payload));
    check(
        static_cast<bool>(writeReadFile.syncAndClose()),
        "prepare readable file"
    );

    FreshFile readFile;
    database.withStorage([&](FreshStorage& activeStorage) {
        return activeStorage.open(
            "/tests/read.bin",
            FreshOpenMode::Read,
            readFile
        );
    });
    storage.failNextRead();
    uint8_t readBuffer[4] = {};
    check(
        readFile.read(readBuffer, sizeof(readBuffer)) == -1 &&
            readFile.error() == EIO,
        "propagate unavailable read"
    );
    readFile.close();

    check(static_cast<bool>(database.deinit()), "deinitialize database");
    check(static_cast<bool>(storage.detach()), "detach fault storage");

    Serial.printf(
        "Storage failure regression complete: %u passed, %u failed\n",
        static_cast<unsigned>(passed),
        static_cast<unsigned>(failed)
    );
}

void loop() {
    delay(1000);
}
