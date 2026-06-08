# Fresh

Fresh is a RAM-first document database for ESP32 with async LittleFS persistence.

Fresh helps you keep small document collections and append-style logs in Arduino ESP32 projects without writing to flash from normal public write calls. It is designed for embedded applications that need predictable RAM-first behavior, background persistence, and simple result-based error handling.

[![CI](https://github.com/ZekStack/fresh/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/fresh/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/fresh?sort=semver)](https://github.com/ZekStack/fresh/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Fresh?

* **RAM-first writes** - public create, update, delete, and append calls accept data into memory before background persistence.
* **ESP32-friendly storage** - dirty-only LittleFS sync reduces unnecessary flash work.
* **Document and stream models** - use general JSON document models or append-style stream models.
* **Clear API** - operations return `FreshResult` instead of throwing exceptions.
* **Production-minded** - FreeRTOS mutex protection, bindable callbacks, storage reporting, and explicit limitations.

## Install

### PlatformIO

Fresh is built for Arduino ESP32 and depends on ArduinoJson v7.

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

Fresh is not published to Arduino Library Manager yet.

Install it by downloading the repository ZIP or cloning it into your Arduino libraries folder.

```txt
Arduino/libraries/Fresh
```

## Quick start

```cpp
#include <Arduino.h>
#include <Fresh.h>

Fresh db;
FreshModel users;

void setup() {
    Serial.begin(115200);

    FreshResult initResult = db.init("/fresh_app");
    if (!initResult) {
        Serial.println(initResult.message.c_str());
        return;
    }

    users = db.createModel("User");
    if (!users) {
        Serial.println("Failed to open User model");
        return;
    }

    JsonDocument user;
    user["name"] = "Panna";
    user["age"] = 19;

    FreshResult createResult = users.create(user);
    if (!createResult) {
        Serial.println(createResult.message.c_str());
        return;
    }

    FreshResult found = users.findById(user["_id"].as<const char *>());
    if (found) {
        serializeJson(found.doc, Serial);
        Serial.println();
    }

    JsonDocument patch;
    patch["age"] = 20;
    users.updateById(user["_id"].as<const char *>(), patch);
}

void loop() {
    delay(1000);
}
```

## Important notes

> [!IMPORTANT]
> Fresh accepts normal public writes into RAM first. A successful `create`, `update`, `delete`, or `append` result means the change was accepted in memory, not necessarily persisted to flash yet.

* Flash persistence happens later in the sync task. Power loss before sync can lose recently accepted changes.
* Sync captures dirty RAM state under a short database lock, then performs LittleFS writes without holding the global database mutex.
* `forceSyncAsync()` requests a forced checkpoint through the sync task for dirty state captured when that sync starts.
* `forceSync()` runs the same forced captured-state checkpoint synchronously and touches flash in the caller context.
* `deinit()` waits for the sync task to exit before owned state is destroyed. By default it performs a final forced checkpoint; pass `{.sync = false}` to stop without final persistence.
* Normal background sync is dirty-only and uses snapshot thresholds for compaction. Forced checkpoints compact the dirty models involved in that sync.
* The current storage and backup formats use ArduinoJson MessagePack and are not stable compatibility contracts yet.

## Examples

The repository includes topic-focused Arduino sketches in the `examples/` folder.

| Example | Description |
| --- | --- |
| `Basic` | Minimal init, create, find, and update. |
| `Crud` | Full create, find, update, and delete operations. |
| `SyncAndStorage` | RAM-first writes, dirty background sync, `storageInfo`, and model lookup. |
| `StreamModel` | Stream model append, retrieve, filtered retrieve, reverse/limit options, and `streamTo`. |
| `ValidatorsAndCallbacks` | Bool/result validators, `std::bind`, event/sync callbacks, and custom time. |
| `BackupStream` | Backup callbacks, `startBackup`, chunked `readBackup`, status checks, and `backupImport`. |
| `ModelManagement` | Create, rename, drop, drop selected, and drop all models. |

Start with:

```txt
examples/Basic
```

## Documentation

Detailed documentation is available in the `docs/` folder.

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Step-by-step setup and first document flow. |
| [`docs/configuration.md`](docs/configuration.md) | `FreshConfig` options and defaults. |
| [`docs/api.md`](docs/api.md) | Public classes, result types, callbacks, and backup API. |
| [`docs/examples.md`](docs/examples.md) | Explanation of all included examples. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common issues and solutions. |

## API overview

```cpp
Fresh db;
FreshResult initResult = db.init("/fresh_app");

FreshModel users = db.createModel("User");
FreshResult created = users.create(userDoc);
FreshResult found = users.findById(id);
FreshResult updated = users.updateById(id, patchDoc);
FreshResult removed = users.deleteById(id);

FreshModel logs = db.createModel("Log", FreshModelType::Stream);
FreshResult appended = logs.append(logDoc);
FreshResult entries = logs.retrieve();
```

For the full API, see [`docs/api.md`](docs/api.md).

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Filesystem | LittleFS |
| Persistence format | ArduinoJson MessagePack |
| PSRAM | Used when available for internal allocations |
| Dependencies | `bblanchon/ArduinoJson >= 7.0.0` |
| Exceptions | Not used |
| Status | Early-stage `0.0.1` |

## Configuration

```cpp
FreshConfig config;
config.syncIntervalMS = 5000;
config.syncTaskStackSize = 8192;
config.snapshotRecordThreshold = 128;
config.backupBufferSize = 8 * 1024;

FreshResult result = db.init("/fresh_app", config);
```

For all options, see [`docs/configuration.md`](docs/configuration.md).

## Error handling

Fresh reports operation status through `FreshResult`.

```cpp
FreshResult result = db.init("/fresh_app");

if (!result) {
    Serial.println(result.message.c_str());
    return;
}
```

For result fields and status codes, see [`docs/api.md`](docs/api.md).

## Project structure

```txt
fresh/
├── examples/
├── docs/
├── src/
├── library.json
├── library.properties
├── README.md
├── LICENSE.md
└── PLAN.md
```

## Status

Fresh is currently early-stage software at `0.0.1`.

The public API, storage format, and backup format may still change before a stable release. Test it on your target ESP32 board before using it in production.

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.

ZekStack libraries are designed to provide small, reusable building blocks for ESP32 applications.
