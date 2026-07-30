# Storage

Fresh 0.2.0 routes every database operation through a `FreshStorage` backend. The selected backend owns the database root, model storage, journals, snapshots, durable manifest slots, garbage collection, restore staging, and application-managed backup archives.

Supported built-in backends:

- LittleFS through ESP-IDF VFS
- SD over SPI through ESP-IDF SDSPI and FATFS
- SD over SDMMC through ESP-IDF SDMMC and FATFS where supported

Applications may also provide custom storage and file implementations. Custom storage does not need to use ESP-IDF VFS or POSIX files.

## Default LittleFS

The existing initialization API remains valid and selects Fresh-managed LittleFS:

```cpp
Fresh database;
FreshResult initialized = database.init("/fresh");
```

Equivalent explicit configuration:

```cpp
FreshConfig config;
config.storageType = FreshStorageType::LittleFS;
config.littleFS.partitionLabel = "spiffs";
config.littleFS.mountPath = "/littlefs";
config.littleFS.maxOpenFiles = 10;
config.littleFS.formatOnMountFailure = false;
config.littleFS.growOnMount = true;

FreshResult initialized = database.init("/fresh", config);
```

Fresh mounts the partition during `init()` and unmounts it after the sync task exits during `deinit()`.

`maxOpenFiles` is enforced by Fresh for every file opened through this storage instance, including internal persistence files and application files. It is not presented as a runtime LittleFS VFS registration option.

`formatOnMountFailure` is disabled by default. Enabling it allows Fresh to erase and format the selected LittleFS partition after a mount failure.

The deprecated `FreshConfig::eraseOnFileSystemFailure` field remains as a compatibility alias for `littleFS.formatOnMountFailure` during the 0.2.0 transition.

## SD over SPI

```cpp
FreshConfig config;
config.storageType = FreshStorageType::SD;
config.sd.interface = FreshSDInterface::SPI;
config.sd.mountPath = "/fresh-sd";
config.sd.maxOpenFiles = 8;
config.sd.allocationUnitSize = 16 * 1024;
config.sd.formatOnMountFailure = false;

config.sd.spi.host = SPI2_HOST;
config.sd.spi.chipSelectPin = GPIO_NUM_10;
config.sd.spi.clockPin = GPIO_NUM_12;
config.sd.spi.mosiPin = GPIO_NUM_11;
config.sd.spi.misoPin = GPIO_NUM_13;
config.sd.spi.frequencyHz = 20'000'000;
config.sd.spi.busOwnership = FreshSPIBusOwnership::Managed;

Fresh database;
FreshResult initialized = database.init("/fresh", config);
```

### Managed SPI bus

With `FreshSPIBusOwnership::Managed`, Fresh:

1. initializes the configured SPI host,
2. mounts the SD card,
3. unmounts the card during shutdown, and
4. releases the SPI host after all Fresh files are closed.

Clock, MOSI, MISO, and chip-select pins are required.

### External SPI bus

Use `FreshSPIBusOwnership::External` when another component initializes or shares the SPI host:

```cpp
config.sd.spi.busOwnership = FreshSPIBusOwnership::External;
```

Fresh mounts and unmounts its SD card device but never initializes or frees the SPI bus. The application must keep the bus available for the complete Fresh lifetime.

SD formatting is always disabled unless `config.sd.formatOnMountFailure` is explicitly enabled. Fresh never silently formats removable media.

## SDMMC

```cpp
FreshConfig config;
config.storageType = FreshStorageType::SD;
config.sd.interface = FreshSDInterface::SDMMC;
config.sd.mountPath = "/fresh-sd";
config.sd.sdmmc.slot = 0;
config.sd.sdmmc.oneBitMode = false;

Fresh database;
FreshResult initialized = database.init("/fresh", config);
```

Board-default SDMMC pins are used when no pin values are supplied.

Targets with SDMMC GPIO-matrix support may provide custom pins:

```cpp
config.sd.sdmmc.clockPin = GPIO_NUM_43;
config.sd.sdmmc.commandPin = GPIO_NUM_44;
config.sd.sdmmc.data0Pin = GPIO_NUM_39;
config.sd.sdmmc.data1Pin = GPIO_NUM_40;
config.sd.sdmmc.data2Pin = GPIO_NUM_41;
config.sd.sdmmc.data3Pin = GPIO_NUM_42;
```

Fresh rejects custom routing on targets that only support fixed SDMMC pins. Four-bit mode requires data lines 0 through 3. One-bit mode requires only data line 0.

## Fresh-owned custom storage

An application can transfer ownership of a custom backend to Fresh:

```cpp
std::unique_ptr<FreshStorage> storage = createCustomStorage();

Fresh database;
FreshResult initialized = database.init(
    "/fresh",
    std::move(storage)
);
```

Fresh calls the backend's protected `mount()` operation, owns it for the complete database lifetime, calls `unmount()` after the sync task exits, and destroys it during cleanup.

This mode is appropriate when the custom backend exists only for this Fresh instance.

## Caller-owned custom storage

A shared or externally managed backend can be passed by reference:

```cpp
CustomStorage storage;
FreshResult mounted = storage.attachApplicationStorage();

Fresh database;
FreshResult initialized = database.init(
    "/fresh",
    storage
);
```

Requirements:

- The storage must already report `FreshStorageState::Mounted`.
- The storage object must outlive the `Fresh` instance.
- The application must not detach or destroy the storage while Fresh is initialized.
- Fresh never calls the underlying backend's `unmount()` operation.
- Fresh detaches only its internal reference adapter during `deinit()`.

This mode is appropriate for shared filesystems, application-managed buses, encrypted volume managers, or multiple services using one mounted storage device.

## Custom storage contract

A custom backend derives from `FreshStorage` and implements:

```cpp
class CustomStorage final : public FreshStorage {
public:
    CustomStorage()
        : FreshStorage(FreshStorageType::Custom) {}

    const char* name() const override;
    FreshStorageInfo info() const override;

private:
    FreshResult mount() override;
    FreshResult unmount() override;

    FreshResult openBackend(
        const char* logicalPath,
        FreshOpenMode mode,
        std::unique_ptr<FreshFileBackend>& backend
    ) override;

    FreshResult existsBackend(
        const char* logicalPath,
        bool& exists
    ) const override;

    FreshResult createDirectoryBackend(
        const char* logicalPath
    ) override;

    FreshResult removeFileBackend(
        const char* logicalPath
    ) override;

    FreshResult removeDirectoryBackend(
        const char* logicalPath
    ) override;

    FreshResult listDirectoryBackend(
        const char* logicalPath,
        std::vector<FreshDirectoryEntry>& entries
    ) const override;
};
```

A VFS-mounted custom backend may inherit the default file and directory implementations by supplying a mount path to the `FreshStorage` constructor. A non-VFS backend overrides the primitives and returns its own `FreshFileBackend` implementation.

Custom backends may pass a third constructor argument to enforce a backend-independent concurrent file limit:

```cpp
FreshStorage(FreshStorageType::Custom, nullptr, 8)
```

### File backend contract

`FreshFileBackend` must implement:

- open-state reporting,
- byte and buffered reads,
- byte and buffered writes,
- `peek()`,
- seek, position, and size,
- durable `sync()`,
- explicit `close()`, and
- native error reporting.

`sync()` is a durability boundary. It must not report success until data and required metadata have reached the backend's durable medium according to that storage technology's guarantees.

Fresh performs the following sequence for durable journals, snapshots, and manifests:

1. write the complete binary header and payload,
2. reject short writes or `Print` write errors,
3. call `sync()`,
4. call `close()`, and
5. reopen and verify durable slot data where the format requires verification.

A custom backend must return failures honestly. It must not convert unavailable media, failed existence queries, short writes, failed synchronization, or failed close operations into success.

## Logical paths

Fresh passes storage-root-relative logical paths beginning with `/`:

```text
/fresh
/fresh/manifest.a.msgpack
/fresh/models/0123456789abcdef/journal.log
/backups/configuration.fresh
```

Built-in VFS backends prepend their configured mount point internally. Applications and custom backends must not expose the VFS mount point as part of logical paths.

Every public storage operation canonicalizes and validates its logical path before access control or backend dispatch. Fresh rejects:

- paths without a leading `/`,
- repeated separators such as `//fresh`,
- `.` and `..` traversal segments,
- backslashes, and
- paths beyond the implementation limit.

A trailing separator is normalized away except for `/`. The configured database root is normalized through the same path boundary before it is protected. Custom backends receive the already validated logical path and should preserve equivalent path isolation for any backend-specific aliases such as case folding.

## Application files and backup archives

Use `Fresh::withStorage()` for short storage operations such as creating a directory or opening a file:

```cpp
FreshFile archive;
FreshResult opened = database.withStorage(
    [&](FreshStorage& storage) -> FreshResult {
        FreshResult directory = storage.createDirectory("/backups");
        if (!directory) return directory;

        return storage.open(
            "/backups/configuration.fresh",
            FreshOpenMode::Write,
            archive
        );
    }
);
```

`withStorage()` holds the Fresh lifecycle lock while the callback runs. This prevents `deinit()` from detaching or destroying the backend between storage lookup and file open. Keep the callback short and do not call database methods from inside it.

Once a `FreshFile` is open, the callback may return. The file owns a shared state object rather than a raw pointer back to `FreshStorage`. Explicit `deinit()` returns `FreshStatus::Busy` until every application file is closed.

Recommended layout:

```text
/fresh/       Fresh-owned database files
/backups/     application-owned backup archives
```

Fresh rejects application operations targeting the configured database root or any child below it. Internal persistence operations run through a scoped internal storage context and remain authorized.

The raw `Fresh::storage()` pointer remains deprecated for source compatibility. New code must use `withStorage()` because a raw pointer cannot provide a lifetime guarantee across concurrent shutdown.

Close every `FreshFile` before calling `deinit()`. Use `syncAndClose()` when the application file is a durability boundary.

When a `Fresh` object is destroyed with an application file still alive, destruction performs best-effort file synchronization, closes and invalidates the shared file state, completes the database shutdown barrier, and then releases storage. The surviving `FreshFile` object is safe to inspect or close but behaves as a closed file. This emergency cleanup prevents use-after-free; it is not a replacement for an explicit application durability boundary.

A `FreshFile` reused for a later successful open starts with a cleared inherited `Print` write-error state. Moving a file transfers its current write-error state to the destination and clears the moved-from object.

## Storage information

Convenience form:

```cpp
FreshStorageInfo info = database.storageInfo();
```

Result-aware form:

```cpp
FreshStorageInfo info;
FreshResult queried = database.storageInfo(info);
if (!queried) {
    Serial.println(queried.message.c_str());
}
```

The result-aware form distinguishes a real zero-capacity value from a failed capacity query.

`FreshStorageInfo` contains:

- backend type,
- backend state,
- backend name,
- mount path,
- backend-native error code,
- total active Fresh file count,
- application file count,
- internal persistence file count,
- configured concurrent file limit,
- total bytes,
- used bytes, and
- free bytes.

Custom backends may override result-aware information retrieval when capacity queries can fail independently.

## Shutdown and open handles

Explicit `deinit()` performs:

1. reject shutdown with `FreshStatus::Busy` if application files remain open,
2. reject new application file opens,
3. cancel or finish backup activity,
4. perform the requested final sync,
5. stop and join the sync task,
6. verify or invalidate any remaining internal handle,
7. unmount or detach storage according to ownership mode, and
8. destroy Fresh-owned backend state.

Application and internal files are counted separately. A transient internal persistence handle no longer causes explicit shutdown to return `Busy`; shutdown waits for the sync boundary instead.

A bounded `deinit()` failure leaves the object in a retryable lifecycle state where possible. Once a call enters final-sync mode, later retries cannot weaken that pending durability decision by passing `sync = false`; the final sync is retried before stop is committed.

The destructor is an unbounded lifetime barrier. It first stops accepting new application files, closes surviving application file states, and then performs normal shutdown. If the durability attempt itself fails, it still stops the task and invalidates remaining handles before member state is destroyed.

## Open-file limits

`littleFS.maxOpenFiles`, `sd.maxOpenFiles`, and the optional custom-storage constructor limit are enforced at `FreshStorage::open()` before the backend is invoked.

The limit includes:

- application files opened through `withStorage()`,
- journals,
- snapshots,
- manifests,
- restore staging files, and
- backup archives opened through the same storage instance.

When the limit is reached, `open()` returns `FreshStatus::Busy`. A successful or failed close releases the reserved slot exactly once. `FreshStorageInfo` exposes the current total, application, and internal counts to help diagnose constrained devices.

## SD card removal

Automatic hot-swap recovery is not included in 0.2.0.

If SD media disappears during operation:

- the current operation returns a storage or filesystem error,
- failed existence probes abort the current sync instead of spinning during model-ID allocation,
- dirty RAM state remains dirty,
- unverified writes are not marked committed,
- restore staging is retained when manifest commit state is uncertain, and
- the application should deinitialize without discarding unsynchronized state only when safe, remount, and reinitialize, or reboot.

Fresh does not automatically fall back from SD to LittleFS because doing so could silently split one logical database across two filesystems.

## Formatting policy

- LittleFS formatting is opt-in.
- SD formatting is opt-in and disabled by default.
- Custom storage formatting is entirely backend-defined.
- Fresh never changes from one storage backend to another after initialization.
- Fresh 0.2.0 does not migrate databases between filesystems.

## Conformance examples

`examples/CustomStorage` contains an in-memory non-VFS backend that verifies:

- caller-owned storage attachment,
- directory and file primitives,
- model creation,
- durable synchronization,
- result-query error propagation,
- database-root protection,
- open-file shutdown blocking,
- deinitialization,
- reinitialization against the same storage instance, and
- persisted document reload.

`examples/StorageLifecycleRegressionTest` exercises path authorization, per-origin handle accounting, explicit Busy shutdown, leaked-file destructor cleanup, and repeated LittleFS lifecycle behavior.

`examples/StorageFailureRegressionTest` exercises configured handle limits, failed existence probes, short writes, read/sync/close failures, and `FreshFile` reuse after a write error.

Production backends also need failure-path and power-loss testing appropriate to their storage medium.
