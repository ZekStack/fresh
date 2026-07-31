# Storage

Fresh 0.2.0 uses an object-based storage API. `FreshConfig` configures database behavior only; the storage backend configures mounting, filesystem options, media transport, pins, and bus ownership.

## Initialization

The default overload creates and owns a default LittleFS backend:

```cpp
Fresh db;
FreshInitResult result = db.init("/fresh");
```

Explicit storage is passed as an rvalue and becomes owned by Fresh:

```cpp
FreshConfig config;

FreshLittleFSConfig storageConfig;
storageConfig.partitionLabel = "spiffs";
storageConfig.mountPath = "/littlefs";
storageConfig.maxOpenFiles = 12;

FreshInitResult result = db.init(
    "/fresh",
    config,
    FreshLittleFSStorage(storageConfig)
);
```

A named backend must be moved:

```cpp
FreshLittleFSStorage storage(storageConfig);
FreshInitResult result = db.init(
    "/fresh",
    config,
    std::move(storage)
);
```

Fresh mounts the backend during `init()`, uses it for database and application files, unmounts it during `deinit()`, and destroys it with the database. There is no borrowed-storage initialization mode in 0.2.0.

## Application files

Use `db.storage()` for files outside the protected database root:

```cpp
FreshResult directory = db.storage().ensureDirectory("/backups");

const uint8_t bytes[] = {1, 2, 3, 4};
FreshResult written = db.storage().writeFile(
    "/backups/config.bin",
    bytes,
    sizeof(bytes)
);

bool exists = db.storage().exists("/backups/config.bin");
```

For streaming and large files, use `FreshFile`:

```cpp
FreshFile file;
FreshResult opened = db.storage().open(
    "/backups/system.fresh",
    FreshOpenMode::Write,
    file
);
if (!opened) return;

file.write(buffer, length);
FreshResult committed = file.syncAndClose();
```

The storage facade provides:

- `available()`
- `open()`
- `exists()`
- `fileSize()`
- `ensureDirectory()` and `createDirectory()`
- `removeFile()` and `removeDirectory()`
- `rename()`
- `listDirectory()`
- `writeFile()` and `readFile()`
- `info()`

The configured database root is protected. When Fresh is initialized at `/fresh`, application calls cannot open, remove, rename, or enumerate `/fresh` or any child below it. Use sibling paths such as `/backups`, `/uploads`, or `/configuration`.

Open application files are tracked. `deinit()` returns `FreshStatus::Busy` while a `FreshFile` remains open. Each `FreshFile` serializes its own operations with a mutex, and the database lifecycle prevents new application files while shutdown is in progress.

## Logical and physical paths

Storage APIs use logical absolute paths. The backend adds its ESP-IDF VFS mount point.

```text
LittleFS mount:    /littlefs
Fresh database:    /fresh
Application file:  /backups/system.fresh

Physical VFS paths:
/littlefs/fresh/...
/littlefs/backups/system.fresh
```

Switching to SD or eMMC changes the physical mount point, not database or application paths.

## LittleFS

`FreshLittleFSStorage` uses ESP-IDF LittleFS directly through `esp_vfs_littlefs_register()`.

```cpp
FreshLittleFSConfig storageConfig;
storageConfig.partitionLabel = "spiffs";
storageConfig.mountPath = "/littlefs";
storageConfig.maxOpenFiles = 12;
storageConfig.formatOnMountFailure = false;
storageConfig.growOnMount = true;

db.init(
    "/fresh",
    FreshConfig(),
    FreshLittleFSStorage(storageConfig)
);
```

Fresh does not include or synchronize Arduino's global `LittleFS` object. Application files should use `db.storage()`.

## SD over SPI

`FreshSDStorage` supports SDSPI through ESP-IDF.

```cpp
FreshSDConfig storageConfig;
storageConfig.interface = FreshSDInterface::SPI;
storageConfig.mountPath = "/sd";
storageConfig.maxOpenFiles = 8;
storageConfig.formatOnMountFailure = false;

storageConfig.spi.host = SPI2_HOST;
storageConfig.spi.chipSelectPin = GPIO_NUM_10;
storageConfig.spi.clockPin = GPIO_NUM_12;
storageConfig.spi.mosiPin = GPIO_NUM_11;
storageConfig.spi.misoPin = GPIO_NUM_13;
storageConfig.spi.frequencyHz = 10'000'000;
storageConfig.spi.busOwnership = FreshSPIBusOwnership::Managed;

db.init(
    "/fresh",
    FreshConfig(),
    FreshSDStorage(storageConfig)
);
```

`Managed` initializes and releases the SPI bus. `External` assumes the application already initialized the selected bus and never releases it.

Fresh does not include or synchronize Arduino's global `SD` object.

## SDMMC

`FreshSDStorage` also supports SD cards connected to an ESP-IDF SDMMC host:

```cpp
FreshSDConfig storageConfig;
storageConfig.interface = FreshSDInterface::SDMMC;
storageConfig.mountPath = "/sd";
storageConfig.sdmmc.slot = 1;
storageConfig.sdmmc.oneBitMode = false;

storageConfig.sdmmc.clockPin = GPIO_NUM_43;
storageConfig.sdmmc.commandPin = GPIO_NUM_44;
storageConfig.sdmmc.data0Pin = GPIO_NUM_39;
storageConfig.sdmmc.data1Pin = GPIO_NUM_40;
storageConfig.sdmmc.data2Pin = GPIO_NUM_41;
storageConfig.sdmmc.data3Pin = GPIO_NUM_42;

db.init(
    "/fresh",
    FreshConfig(),
    FreshSDStorage(storageConfig)
);
```

These pins match the onboard TF-card data signals on the Waveshare ESP32-P4-Module-DEV-KIT. Board-specific power, LDO, reset, and voltage-selection setup must be completed by the application before `db.init()`.

Fresh does not include or synchronize Arduino's global `SD_MMC` object.

## eMMC

Include the dedicated backend:

```cpp
#include <FreshEMMCStorage.h>
```

Then configure its SDMMC connection:

```cpp
FreshEMMCConfig storageConfig;
storageConfig.mountPath = "/emmc";
storageConfig.slot = 1;
storageConfig.busWidth = 8;
storageConfig.frequencyHz = 20'000'000;
storageConfig.formatOnMountFailure = false;

// Set clock, command, and data0-data7 when custom GPIO routing is used.

db.init(
    "/fresh",
    FreshConfig(),
    FreshEMMCStorage(storageConfig)
);
```

The backend supports 1-, 4-, and 8-bit widths. Board-specific power and reset sequencing remains outside Fresh and must complete before initialization.

## Custom backends

A custom backend derives from `FreshStorage` and implements mount, unmount, capacity reporting, and file/directory primitives. Fresh owns the backend object, while the underlying medium may live independently.

For an in-memory test backend, keep the volume outside the backend:

```cpp
MemoryVolume volume;

db.init(
    "/fresh",
    FreshConfig(),
    MemoryStorage(volume)
);
```

After `deinit()`, another `MemoryStorage(volume)` can mount the same external volume. This mirrors real hardware: Fresh owns the driver/backend object, not the physical flash chip or card.

## Failure and removal behavior

Fresh 0.2.0 fails closed when storage operations fail. Short writes, read failures, sync failures, close failures, capacity-query failures, and unavailable media are returned as `FreshResult` failures.

Automatic removable-media remount and hot-swap recovery are not implemented. Applications should treat card removal while Fresh is active as a storage failure and perform a controlled shutdown or restart before reinitializing the backend.
