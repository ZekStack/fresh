# Migrating to Fresh 0.2.0

Fresh 0.2.0 introduces pluggable storage while preserving the existing zero-configuration LittleFS initialization path.

## Existing LittleFS applications

Existing code remains valid:

```cpp
Fresh database;
FreshResult initialized = database.init("/fresh");
```

This selects the managed LittleFS backend with the compatibility defaults:

- partition label: `spiffs`
- VFS mount path: `/littlefs`
- maximum open files: `10`
- format on mount failure: disabled
- grow on mount: enabled

The database root remains the logical path supplied to `init()`. It is not prefixed with the VFS mount point in application code.

## Storage format

The storage abstraction does not introduce a new database or backup encoding. Journals, snapshots, durable manifest slots, and backup archives keep the existing format used by the branch before the storage migration.

A database copied between supported filesystems remains logically compatible as long as the complete database directory is transferred while Fresh is stopped. Fresh 0.2.0 does not perform this copy automatically.

## Explicit LittleFS configuration

Applications that need a non-default partition or mount path should configure it explicitly:

```cpp
FreshConfig config;
config.storageType = FreshStorageType::LittleFS;
config.littleFS.partitionLabel = "storage";
config.littleFS.mountPath = "/storage";

FreshResult initialized = database.init("/fresh", config);
```

`FreshConfig::eraseOnFileSystemFailure` remains available as a deprecated compatibility alias. New code should use:

```cpp
config.littleFS.formatOnMountFailure = true;
```

Formatting remains disabled by default.

## Moving the database to SD

Select `FreshStorageType::SD` and configure either SDSPI or SDMMC. Fresh does not automatically migrate existing LittleFS data.

A controlled manual migration should:

1. call `deinit()` and verify success,
2. copy the complete database root and any application backup directory,
3. configure the SD backend,
4. initialize Fresh against the copied database root, and
5. verify the expected models before deleting the original copy.

Do not operate one logical database partly from LittleFS and partly from SD.

## Application-managed files

Use `withStorage()` instead of retaining a raw backend pointer:

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

The raw `storage()` accessor remains deprecated for source compatibility, but it cannot guarantee backend lifetime during concurrent shutdown.

Fresh rejects application operations under the configured database root. Store application files in sibling paths such as `/backups`.

## Shutdown behavior

Fresh tracks all `FreshFile` handles. `deinit()` returns `FreshStatus::Busy` while an application file remains open, before stopping the sync task or unmounting storage.

Close files explicitly. Use `syncAndClose()` when the file itself is a durability boundary.

## Custom storage

Fresh-owned backend:

```cpp
std::unique_ptr<FreshStorage> storage = createCustomStorage();
FreshResult initialized = database.init("/fresh", std::move(storage));
```

Caller-owned backend:

```cpp
CustomStorage storage;
storage.attach();
FreshResult initialized = database.init("/fresh", storage);
```

Caller-owned storage must already be mounted, must outlive Fresh, and must remain attached until `deinit()` completes.

Custom file backends must report short writes, unavailable reads, synchronization failures, and close failures rather than converting them to success.

## Removed assumptions

Code must no longer assume that Fresh persistence uses the Arduino `LittleFS` singleton. Production persistence operates through `FreshStorage` and `FreshFile`.

Capacity reporting should use the result-aware overload when failure must be distinguished from zero capacity:

```cpp
FreshStorageInfo info;
FreshResult queried = database.storageInfo(info);
```

## Deferred behavior

Fresh 0.2.0 does not provide:

- automatic cross-filesystem migration,
- automatic fallback from SD to LittleFS,
- simultaneous multi-filesystem databases,
- automatic SD hot-swap recovery, or
- automatic remount after card replacement.

These behaviors are intentionally excluded because silent fallback or partial migration could split or corrupt one logical database.
