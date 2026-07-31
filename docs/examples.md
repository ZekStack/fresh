# Examples

All examples are under [`../examples`](../examples).

| Example | Purpose |
| --- | --- |
| `Basic` | General model creation and document operations. |
| `StreamModel` | Append-only stream model usage. |
| `LittleFSStorage` | Explicit `FreshLittleFSStorage` configuration and `db.storage()` file access. |
| `SDSPIStorage` | ESP-IDF SD card storage over a managed SPI bus. |
| `SDMMCStorage` | ESP-IDF SDMMC card configuration, including the Waveshare ESP32-P4-Module-DEV-KIT pins. |
| `EMMCStorage` | Dedicated 1/4/8-bit eMMC backend configuration. |
| `SameFilesystemBackup` | Stream a Fresh backup into an application file on the active backend. |
| `CustomStorage` | Implement an owned custom backend over an independently owned memory volume. |
| `StorageLifecycleRegressionTest` | Database-root protection, open-file shutdown blocking, and repeated initialization. |
| `StorageFailureRegressionTest` | Short-write, read, sync, close, and existence-query failure injection. |
| `HardeningRegressionTest` | Atomic mutation, retryable shutdown, timeout, and allocation hardening checks. |
| `SelfTest` | Destructive end-to-end development test. |
| `ReleaseHardeningTest` | Persistence and lifecycle release checks. |

## Storage initialization pattern

```cpp
Fresh database;
FreshConfig config;

FreshLittleFSConfig storageConfig;
storageConfig.maxOpenFiles = 12;

FreshInitResult initialized = database.init(
    "/fresh",
    config,
    FreshLittleFSStorage(storageConfig)
);
```

## Application-file pattern

```cpp
database.storage().ensureDirectory("/backups");

FreshFile archive;
FreshResult opened = database.storage().open(
    "/backups/system.fresh",
    FreshOpenMode::Write,
    archive
);
```

## Hardware examples

SD and eMMC examples compile across the CI target matrix but still require matching physical wiring, media, bus ownership, and board-level power/reset setup before runtime validation.
