# API reference

## Fresh lifecycle

```cpp
FreshInitResult init(
    const char* databasePath,
    const FreshConfig& config = FreshConfig()
);
```

Creates the default LittleFS backend.

```cpp
template <typename Storage>
FreshInitResult init(
    const char* databasePath,
    const FreshConfig& config,
    Storage&& storage
);
```

Moves an explicit `FreshStorage` backend into Fresh, mounts it, loads the database, and starts the sync task.

```cpp
FreshResult deinit(
    const FreshDeinitOptions& options = FreshDeinitOptions()
);
```

Performs final persistence by default, stops the sync task, unmounts the backend, and destroys it. Returns `FreshStatus::Busy` while an application `FreshFile` is open.

## Storage access

```cpp
FreshStorageAccess storage();
```

Returns a lightweight facade for the active backend. It does not expose mount or unmount operations.

### Availability and information

```cpp
bool available() const;
bool exists(const char* path) const;
FreshResult exists(const char* path, bool& exists) const;
FreshResult fileSize(const char* path, size_t& size) const;
FreshResult info(FreshStorageInfo& info) const;
FreshStorageInfo info() const;
```

The overloads returning `FreshResult` distinguish a missing file from a storage failure.

### Directories

```cpp
FreshResult ensureDirectory(const char* path) const;
FreshResult createDirectory(const char* path) const;
FreshResult removeDirectory(const char* path) const;
FreshResult listDirectory(
    const char* path,
    std::vector<FreshDirectoryEntry>& entries
) const;
```

`ensureDirectory()` creates missing parent directories. `createDirectory()` creates one level and succeeds when that directory already exists.

### Files

```cpp
FreshResult open(
    const char* path,
    FreshOpenMode mode,
    FreshFile& file
) const;

FreshResult writeFile(
    const char* path,
    const uint8_t* data,
    size_t length
) const;

FreshResult readFile(
    const char* path,
    uint8_t* buffer,
    size_t capacity,
    size_t& bytesRead
) const;

FreshResult removeFile(const char* path) const;
FreshResult rename(
    const char* source,
    const char* target,
    bool replaceExisting = false
) const;
```

Use `writeFile()` and `readFile()` for bounded complete-file operations. Use `open()` for streaming or large files.

## FreshFile

`FreshFile` is move-only and derives from Arduino `Stream`/`Print` for integration with existing streaming APIs.

```cpp
explicit operator bool() const;
int available();
int read();
int read(uint8_t* buffer, size_t length);
int peek();
size_t write(uint8_t byte);
size_t write(const uint8_t* buffer, size_t length);
bool seek(size_t position);
size_t position() const;
size_t size() const;
int error() const;
FreshResult sync();
FreshResult close();
FreshResult syncAndClose();
```

Every operation is serialized by the file state's mutex. `syncAndClose()` is the normal durability boundary for application output.

## Models

```cpp
FreshModelResult createModel(const char* name);
FreshModelResult createModel(const char* name, FreshModelType type);
FreshModel model(const char* name);
FreshModelListResult listModels() const;
FreshResult renameModel(const char* oldName, const char* newName);
FreshResult dropModel(const char* name);
FreshResult dropAllModels();
```

### General-model records

```cpp
FreshResult create(JsonDocument& document);
FreshResult findById(const char* id) const;
FreshResult find(FreshPredicate predicate, bool stopAtFirst = false) const;
FreshResult listRecords(const FreshRecordRetrieveOptions& options = {}) const;
FreshResult replaceById(const char* id, const JsonDocument& replacement);
FreshResult updateById(const char* id, const JsonDocument& patch, FreshReturn mode = FreshReturn::None);
FreshResult update(FreshPredicate predicate, const JsonDocument& patch, FreshReturn mode = FreshReturn::None);
FreshResult deleteById(const char* id);
FreshResult deleteMany(FreshPredicate predicate);
```

### Stream-model records

```cpp
FreshResult append(JsonDocument& document);
FreshResult append(JsonDocument& document, const FreshStreamAppendOptions& options);
FreshResult retrieve(const FreshStreamRetrieveOptions& options = {}) const;
FreshResult streamTo(Print& output) const;
```

## Persistence

```cpp
FreshResult flush();
FreshResult forceSyncAsync();
FreshResult forceSync();
FreshResult collectGarbage(FreshGarbageCollectionResult& result);
FreshDiagnostics diagnostics() const;
```

`flush()` writes captured pending journal data. `forceSync()` also forces checkpoint processing.

## Backup

```cpp
FreshResult estimateBackup(const FreshBackupOptions&, FreshBackupEstimate&) const;
FreshResult startBackup(const FreshBackupOptions& options = {});
size_t readBackup(uint8_t* buffer, size_t length, uint32_t timeoutMS = 0);
FreshBackupStatus backupStatus() const;
FreshResult cancelBackup();
FreshResult inspectBackup(Stream& input, FreshBackupMetadata& metadata) const;
FreshResult restoreBackup(Stream& input, const FreshRestoreOptions& options);
```

A backup archive can be written to the selected backend with `db.storage().open()`; the archive path must be outside the protected database root.

## Callbacks

```cpp
void onSync(FreshSyncCallback callback);
void onEvent(FreshEventCallback callback);
void onTimeGet(FreshTimeCallback callback);
void onBackupStart(FreshBackupCallback callback);
void onBackupProgress(FreshBackupCallback callback);
void onBackupEnd(FreshBackupCallback callback);
void onBackupError(FreshBackupCallback callback);
```

Callbacks are notification hooks. Do not call blocking lifecycle, persistence, backup, or storage operations directly from a callback. Schedule work on another task instead.
