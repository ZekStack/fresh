# Fresh

Fresh is a RAM-first document database for ESP32 with owned, pluggable storage.

Fresh keeps small document collections and append-style logs in RAM while a background task persists them through a selected ESP-IDF storage backend.

[![CI](https://github.com/ZekStack/fresh/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/fresh/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/fresh?sort=semver)](https://github.com/ZekStack/fresh/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Features

- RAM-first create, update, delete, and append operations.
- General JSON document models and append-style stream models.
- Owned LittleFS, SDSPI, SDMMC, eMMC, and custom storage backends.
- Application-file access through `db.storage()`.
- Background persistence, forced sync, streaming backup, and restore.
- `FreshResult` error handling without exceptions.
- FreeRTOS mutex protection and explicit shutdown behavior.

## Install

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/fresh.git
  bblanchon/ArduinoJson@>=7.0.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

### Arduino IDE

Fresh is not published to Arduino Library Manager yet. Download the repository ZIP or clone it into:

```txt
Arduino/libraries/Fresh
```

## Quick start

```cpp
#include <Arduino.h>
#include <Fresh.h>

Fresh db;

void setup() {
    Serial.begin(115200);

    FreshInitResult initialized = db.init("/fresh_app");
    if (!initialized) {
        Serial.println(initialized.message.c_str());
        return;
    }

    FreshModelResult usersResult = db.createModel("User");
    if (!usersResult && usersResult.status != FreshStatus::ModelExists) {
        Serial.println(usersResult.message.c_str());
        return;
    }

    FreshModel users = db.model("User");

    JsonDocument user;
    user["name"] = "Panna";
    user["age"] = 19;

    FreshResult created = users.create(user);
    if (!created) {
        Serial.println(created.message.c_str());
        return;
    }

    FreshResult found = users.findById(user["_id"].as<const char*>());
    if (found) {
        serializeJson(found.doc, Serial);
        Serial.println();
    }
}

void loop() {
    delay(1000);
}
```

The convenience overload above creates a default `FreshLittleFSStorage`.

## Explicit storage

`FreshConfig` contains database settings only. Construct storage independently and pass it to `init()`:

```cpp
FreshConfig config;
config.syncIntervalMS = 5000;

FreshLittleFSConfig storageConfig;
storageConfig.partitionLabel = "spiffs";
storageConfig.mountPath = "/littlefs";
storageConfig.maxOpenFiles = 12;
storageConfig.formatOnMountFailure = false;

FreshInitResult initialized = db.init(
    "/fresh_app",
    config,
    FreshLittleFSStorage(storageConfig)
);
```

Fresh owns the backend after successful initialization. A named backend must be moved:

```cpp
FreshLittleFSStorage storage(storageConfig);
db.init("/fresh_app", config, std::move(storage));
```

Supported built-in backends:

- `FreshLittleFSStorage`
- `FreshSDStorage` with SDSPI
- `FreshSDStorage` with SDMMC
- `FreshEMMCStorage`

Custom classes can derive from `FreshStorage`.

## Application files

Database and application files can share the selected backend without a separate filesystem wrapper:

```cpp
FreshResult directory = db.storage().ensureDirectory("/backups");

const uint8_t marker[] = {1, 2, 3, 4};
FreshResult written = db.storage().writeFile(
    "/backups/marker.bin",
    marker,
    sizeof(marker)
);

bool exists = db.storage().exists("/backups/marker.bin");
```

For streaming:

```cpp
FreshFile file;
FreshResult opened = db.storage().open(
    "/backups/system.fresh",
    FreshOpenMode::Write,
    file
);
if (!opened) return;

file.write(buffer, length);
file.syncAndClose();
```

The configured database root is protected from application storage operations. Open application files cause `deinit()` to return `FreshStatus::Busy` until they are closed.

Fresh uses ESP-IDF filesystem and media drivers directly. It does not include or synchronize Arduino's global `LittleFS`, `SD`, or `SD_MMC` objects.

## Persistence behavior

> [!IMPORTANT]
> A successful public mutation means the change was accepted in RAM. It does not necessarily mean the change has reached storage.

| Operation | RAM updated | Storage updated before return |
| --- | --- | --- |
| `create()` / `update()` / `delete()` / `append()` | yes | no |
| `flush()` | yes | captured journal operations |
| `forceSyncAsync()` | yes | no |
| `forceSync()` | yes | yes, when successful |
| `deinit({ .sync = true })` | yes | yes, when successful |

Additional lifecycle rules:

- Background sync captures dirty state under a short database lock and performs storage I/O outside that lock.
- `forceSync()` performs a blocking forced checkpoint in the caller context.
- `deinit()` performs a final sync by default and waits for the sync task to exit.
- A timed-out `deinit()` may be called again to finish shutdown.
- `FreshFile` operations are mutex-protected.
- Callbacks are notifications; schedule blocking database or storage work on another task.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal model and document usage. |
| `Crud` | General-model CRUD operations. |
| `StreamModel` | Append and retrieve stream records. |
| `BackupStream` | Streaming backup lifecycle. |
| `LittleFSStorage` | Explicit LittleFS backend and application files. |
| `SDSPIStorage` | SD card over SPI. |
| `SDMMCStorage` | SD card over SDMMC, including Waveshare ESP32-P4 pins. |
| `EMMCStorage` | Dedicated eMMC backend. |
| `SameFilesystemBackup` | Write a backup archive through `db.storage()`. |
| `CustomStorage` | Owned custom backend over an external medium. |
| `StorageLifecycleRegressionTest` | Storage ownership, path protection, and shutdown. |
| `StorageFailureRegressionTest` | Inject file and backend failures. |
| `HardeningRegressionTest` | Mutation and shutdown hardening. |

Regression sketches are compiled in CI but require manual execution on hardware.

## Documentation

- [Getting started](docs/getting-started.md)
- [Configuration](docs/configuration.md)
- [Storage](docs/storage.md)
- [API reference](docs/api.md)
- [Examples](docs/examples.md)
- [Migrating to 0.2.0](docs/migration-0.2.0.md)
- [0.2.0 release notes](docs/release-notes-0.2.0.md)
- [Storage implementation progress](docs/0.2.0-storage-progress.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Release hardening](docs/release-hardening.md)

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino as an ESP-IDF component / Arduino ESP32 |
| Language | C++20 |
| Storage drivers | ESP-IDF LittleFS, SDSPI, SDMMC, eMMC, custom |
| Persistence encoding | ArduinoJson MessagePack |
| PSRAM | Used for eligible internal allocations when available |
| Exceptions | Not used by the Fresh API |
| Status | `0.2.0` pre-release |

## Limitations

Fresh is not intended for large datasets, high-frequency telemetry, SQL-style queries, multi-device concurrency, or data that must be durable after every public mutation.

Automatic SD hot-swap recovery, automatic remount, and multiple simultaneously managed volumes are not part of 0.2.0.

## License

MIT — see [LICENSE.md](LICENSE.md).

## ZekStack

Fresh is part of the ZekStack ESP32 library stack.
