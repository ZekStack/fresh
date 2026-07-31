# Migrating to Fresh 0.2.0

Fresh 0.2.0 intentionally breaks the pre-release storage API. The branch does not provide deprecated aliases, factory adapters, borrowed backends, or Arduino filesystem compatibility shims.

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

`FreshConfig` now contains database settings only.

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

It is equivalent to initializing an owned `FreshLittleFSStorage` with default settings.
