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

    FreshModelResult usersResult = db.createModel("User");
    if (!usersResult) {
        Serial.println(usersResult.message.c_str());
        return;
    }
    users = usersResult.model;

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
* `flush()` synchronously persists captured pending journal operations without forcing a checkpoint snapshot. Use it as a durability barrier before a controlled reboot.
* `deinit()` waits for the sync task to exit before owned state is destroyed. By default it performs a final forced checkpoint; pass `{.sync = false}` to stop without final persistence.
* A bounded explicit `deinit()` may return `FreshStatus::Timeout`; the object remains in a stopping state and a later `deinit()` can finish waiting. The destructor uses an unbounded task-exit barrier because owned state cannot be destroyed while the sync task may still access it. Production code should still call `FreshResult result = db.deinit();` manually when it needs to observe final-sync failures.
* `diagnostics()` reports model load recovery after `init()`, including corrupt snapshots or recovered journals.
* `create()` intentionally mutates the input `JsonDocument` by adding `_id`, `createdAt`, and `updatedAt`.
* After `startBackup()`, keep calling `readBackup()` until backup finishes or call `cancelBackup()`. An undrained backup can occupy the sync task and delay normal persistence.
* `backupStatus()` returns `FreshBackupStatus`: use `state` as the stable `FreshBackupState` lifecycle signal and `result` for detailed success/failure diagnostics.
* Normal background sync is dirty-only and uses snapshot thresholds for compaction. Forced checkpoints compact the dirty models involved in that sync.
* Fresh enforces configurable document, journal, snapshot, and LittleFS reserve limits. Oversized payloads return `FreshStatus::SizeLimitExceeded`; sync preflight space failures return `FreshStatus::StorageFull`.
* Callbacks are notification hooks. Do not call `deinit()`, `flush()`, `forceSync()`, `forceSyncAsync()`, `startBackup()`, `backupImport()`, or long-blocking code from callbacks. Post work to another task instead.
* The current storage and backup formats use ArduinoJson MessagePack. Manifest and snapshot files use two durable slot files with checksummed binary headers. Formats are not stable compatibility contracts yet, and Fresh does not migrate older single-file `manifest.msgpack` / `snapshot.msgpack` storage into the durable-slot format.
* Fresh `0.1.0` uses manifest/snapshot payload v3 and journal v3. Manifest entries map logical names to immutable storage IDs, so rename never moves model directories. The release intentionally rejects earlier pre-release storage formats; erase the development database when upgrading.

## When not to use Fresh

Fresh is not intended for large datasets, high-frequency telemetry, SQL-like querying, multi-device concurrency, or data that must be flash-durable immediately after every write.

## Persistence guarantees

| Operation | RAM updated | Flash updated before return |
| --- | --- | --- |
| `create()` / `update()` / `delete()` / `append()` | yes | no |
| `flush()` | yes | yes, for the captured pending journal operations |
| `forceSyncAsync()` | yes | no |
| `forceSync()` | yes | yes, if successful |
| `deinit({ .sync = true })` | yes | yes, if successful |

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
| `SelfTest` | Destructive Fresh development self-test for persistence, recovery, backup, and shutdown behavior. It uses `/fresh_selftest`, `/fresh_selftest_src`, and `/fresh_selftest_dst`, touches internal storage files, and should only be run on a test device or test partition. |
| `ReleaseHardeningTest` | Focused v0.1.0 validation for immutable storage IDs, rename persistence, configuration ceilings, synchronized metadata access, and repeatable shutdown. |

`SelfTest` and `ReleaseHardeningTest` are compiled by CI through the examples build loop, but they are not executed in CI. Run both manually on ESP32 hardware. A successful run ends like this:

```txt
Fresh SelfTest starting
[PASS] create -> forceSync -> reload
...
SelfTest complete: 16 passed, 0 failed
```

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
| [`docs/release-hardening.md`](docs/release-hardening.md) | v0.1.0 persistence, synchronization, shutdown, allocation, and validation invariants. |

## API overview

```cpp
Fresh db;
FreshResult initResult = db.init("/fresh_app");

FreshModelResult usersResult = db.createModel("User");
FreshModel users = usersResult.model;
FreshResult created = users.create(userDoc);
FreshResult found = users.findById(id);
FreshResult updated = users.updateById(id, patchDoc);
FreshResult removed = users.deleteById(id);

FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
FreshModel logs = logsResult.model;
FreshResult appended = logs.append(logDoc, {.maxEntries = 50});

FreshStreamRetrieveOptions options;
options.reverse = true;
options.limit = 50;
FreshResult entries = logs.retrieve(options);
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
| Status | Early-stage `0.1.0` |

## Configuration

```cpp
FreshConfig config;
config.syncIntervalMS = 5000;
config.syncTaskStackSize = 8192;
config.snapshotRecordThreshold = 128;
config.backupBufferSize = 8 * 1024;
config.minFreeBytes = 4096;
config.maxDocumentBytes = 16 * 1024;
config.maxJournalRecordBytes = 32 * 1024;
config.maxSnapshotBytes = 256 * 1024;

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

Fresh is currently early-stage software at `0.1.0`.

The public API, storage format, and backup format may still change before a stable release. Fresh does not currently migrate older single-file `manifest.msgpack` / `snapshot.msgpack` storage into the durable-slot format. Data written by early versions may require export/import, manual migration, or a storage reset after format changes. Test it on your target ESP32 board before using it in production.

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.

ZekStack libraries are designed to provide small, reusable building blocks for ESP32 applications.
