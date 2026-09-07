# Migrating to Fresh 0.2.0

Fresh 0.2.0 intentionally breaks the pre-release storage API. The branch does not provide deprecated aliases, factory adapters, borrowed backends, or Arduino filesystem compatibility shims.

Fresh 0.2.0 also depends on Strata v0.1.2 for placement-aware memory ownership and Fresh-owned FreeRTOS primitives.

## Storage selection

Before:

```cpp
FreshConfig config;
config.storageType = FreshStorageType::LittleFS;
config.littleFS.maxOpenFiles = 12;

db.init("/fresh", config);
```

After:

```cpp
FreshConfig config;

FreshLittleFSConfig storageConfig;
storageConfig.maxOpenFiles = 12;

db.init(
    "/fresh",
    config,
    FreshLittleFSStorage(storageConfig)
);
```

`FreshConfig` now contains database and memory-policy settings, while storage-specific settings live with the selected backend.

## Memory policy

Fresh now exposes `FreshConfig::memory`:

```cpp
FreshConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

The allocation policy defaults to `PreferExternal`; the sync-task stack defaults to `Internal`.

Fresh/FreshModel-owned JSON clones and results inherit `config.memory.allocation`, as does the backup buffer. The process-lifetime ArduinoJson allocator remains safe for returned documents that outlive the originating `Fresh` instance.

The task-stack setting distinguishes preferences from hard requirements. The configured storage may impose a stricter safety requirement. `FreshLittleFSStorage` constrains `PreferExternal` to an internal sync-task stack. `FreshSDStorage` and `FreshEMMCStorage` are unconstrained and therefore honor the configured stack placement.

A custom backend may override `FreshStorage::syncTaskStackRequirement()` and return `FreshTaskStackRequirement::Internal` when its I/O path cannot safely execute from a PSRAM-backed task stack.

`RequireExternal` is not silently weakened. If a backend declares `FreshTaskStackRequirement::Internal`, configuring `taskStack = Strata::Placement::RequireExternal` causes `init()` to fail with `FreshStatus::InvalidArgument`. Use `PreferExternal` when external RAM is desirable but storage safety may require an internal stack.

Inspect `Fresh::diagnostics()` when the distinction between requested, effective, and observed placement matters.

## Storage ownership

Before, custom storage could be borrowed by reference. That mode has been removed.

After, pass a temporary or move a named backend:

```cpp
CustomStorage storage(customConfig);

db.init(
    "/fresh",
    config,
    std::move(storage)
);
```

Fresh owns, mounts, unmounts, and destroys the backend. Put persistent medium state outside the backend object when a custom implementation must survive creation of a new backend instance.

The public storage object remains standard polymorphic ownership; Strata integration does not change the `std::unique_ptr<FreshStorage>` boundary.

## Application files

Before:

```cpp
FreshFile file;
db.withStorage([&](FreshStorage& storage) {
    return storage.open("/backups/archive.fresh", FreshOpenMode::Write, file);
});
```

After:

```cpp
FreshFile file;
FreshResult opened = db.storage().open(
    "/backups/archive.fresh",
    FreshOpenMode::Write,
    file
);
```

`withStorage()` and the raw storage pointer accessor have been removed.

## Arduino filesystem objects

Fresh no longer includes or binds:

```cpp
LittleFS
SD
SD_MMC
```

Fresh mounts built-in storage through ESP-IDF and exposes application files through `db.storage()`. Code that used an Arduino filesystem singleton for the same partition must migrate to the Fresh storage facade or mount a separate filesystem independently.

## Removed compatibility code

The 0.2.0 redesign removes:

- storage selection from `FreshConfig`;
- `FreshStorageFactory`;
- borrowed `FreshStorageReference` initialization;
- `Fresh::withStorage()`;
- the raw backend pointer accessor;
- the Arduino LittleFS bridge;
- `eraseOnFileSystemFailure`;
- the legacy-database P4 fixture tied to the old compatibility path.

## Default LittleFS convenience

This remains supported because it is a useful default, not a compatibility shim:

```cpp
db.init("/fresh");
```

It is equivalent to initializing an owned `FreshLittleFSStorage` with default settings. The convenience backend carries the same internal sync-task stack constraint as an explicitly constructed `FreshLittleFSStorage`.
