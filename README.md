# Fresh

Fresh is a RAM-first document database for ESP32 with async LittleFS persistence.

It is built for Arduino ESP32 projects that need small document collections, simple query/update helpers, stream-style logs, and background persistence without touching flash from normal public write calls.

## Status

Fresh is currently early-stage software at `0.0.1`.

The public API, storage format, and backup format may still change before a stable release. Use it as a practical starting point, test it on your target hardware, and avoid treating the current flash layout as a permanent compatibility contract.

## Features

- General document models backed by ArduinoJson `JsonDocument`.
- Stream models for append-style records such as logs.
- RAM-first public writes.
- Dirty-only background sync to reduce unnecessary flash writes.
- MessagePack persistence through ArduinoJson v7.
- Automatic `_id`, `createdAt`, and `updatedAt` fields.
- Validators for create/update checks.
- Event, sync, backup, and custom time callbacks.
- Backup streaming through a small read buffer.
- Model create, lookup, rename, drop, and drop-all helpers.
- LittleFS storage usage reporting.
- FreeRTOS mutex protection for shared state.
- No exceptions; operations report status through `FreshResult`.

## Durability Model

Fresh accepts normal public writes into RAM first. A successful `create`, `update`, `delete`, or `append` result means the change was validated and accepted in memory.

Flash persistence happens later in the sync task. If power is lost before the dirty state is synced, recent accepted changes can be lost.

Periodic sync is dirty-only: if nothing changed, the sync task should not write, rename, truncate, or update metadata on flash.

`forceSync()` is the blocking exception. It runs sync work in the caller context and touches flash intentionally. Use `forceSyncAsync()` when you want the sync task to do the work instead.

## Installation

### PlatformIO

Fresh is designed for Arduino ESP32 and depends on ArduinoJson v7.

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

Fresh is not published to Arduino Library Manager yet. Until it is published, install it from the repository ZIP or clone it into your Arduino libraries folder.

## Quick Start

```cpp
#include <Arduino.h>
#include <Fresh.h>

Fresh db;
FreshModel users;

void setup() {
	Serial.begin(115200);

	FreshConfig config;
	config.syncIntervalMS = 5000;

	FreshResult initResult = db.init("/fresh_app", config);
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

	db.forceSyncAsync();
}

void loop() {
	delay(1000);
}
```

## API Overview

### `FreshConfig`

Controls sync interval, sync task priority/core/stack size, LittleFS mount behavior, default model type, snapshot thresholds, and backup buffer size.

### `Fresh`

Owns the database instance.

Common methods:

- `init(path, config)`
- `createModel(name)`
- `createModel(name, FreshModelType::Stream)`
- `model(name)`
- `dropModel(name)`
- `dropModels({...})`
- `dropAllModels()`
- `renameModel(oldName, newName)`
- `forceSyncAsync()`
- `forceSync()`
- `storageInfo()`
- `startBackup()`
- `readBackup(buffer, length, timeoutMS)`
- `backupStatus()`
- `cancelBackup()`
- `backupImport(Stream&)`
- `backupImport(buffer, length)`

### `FreshModel`

Represents a model handle.

Document model methods:

- `create(doc)`
- `findById(id)`
- `findOne(field, value)`
- `find(predicate)`
- `updateById(id, patch)`
- `updateOne(predicate, patch)`
- `update(predicate, patch)`
- `deleteById(id)`
- `deleteOne(predicate)`
- `deleteMany(predicate)`

Stream model methods:

- `append(doc)`
- `retrieve()`
- `retrieve(predicate, options)`
- `streamTo(Print&)`

### `FreshResult`

All operations return result objects instead of throwing exceptions.

Important fields:

- `result`
- `status`
- `message`
- `doc`
- `affectedCount`

Use it directly in conditions:

```cpp
FreshResult result = users.findOne("name", "Panna");
if (!result) {
	Serial.println(result.message.c_str());
}
```

### Callbacks

Fresh callbacks use `std::function`, so lambdas and `std::bind` both work.

- `onEvent`
- `onSync`
- `onTimeGet`
- `onBackupStart`
- `onBackupProgress`
- `onBackupEnd`
- `onBackupError`

### Backup

Backup generation runs through the sync task and can be read in chunks.

```cpp
db.startBackup();

uint8_t buffer[256];
size_t read = db.readBackup(buffer, sizeof(buffer), 50);
```

Backups can be restored from an already-open `Stream` such as a `File`, or from a memory buffer:

```cpp
File backup = LittleFS.open("/backup.msgpack", "r");
db.backupImport(backup);

FreshResult imported = db.backupImport(buffer, length);
```

The current backup format is a streamable MessagePack archive. Heavy compression and web-server-specific helpers are not part of the current core API.

## Examples

- [Basic](examples/Basic/Basic.ino): minimal init, create, find, update, stream append, and async sync.
- [Crud](examples/Crud/Crud.ino): create, find, update, and delete operations.
- [ValidatorsAndCallbacks](examples/ValidatorsAndCallbacks/ValidatorsAndCallbacks.ino): validators, event/sync callbacks, custom time, and `std::bind`.
- [SyncAndStorage](examples/SyncAndStorage/SyncAndStorage.ino): RAM-first writes, `forceSyncAsync`, `forceSync`, `storageInfo`, and `db.model`.
- [StreamModel](examples/StreamModel/StreamModel.ino): stream model append, `retrieve`, and `streamTo`.
- [BackupStream](examples/BackupStream/BackupStream.ino): backup callbacks, `startBackup`, `readBackup`, `backupStatus`, and `backupImport`.
- [ModelManagement](examples/ModelManagement/ModelManagement.ino): create, rename, drop, drop selected, and drop all models.

## Storage Layout

Fresh stores database metadata under the configured database root in LittleFS.

Each model has its own storage area. Document and stream changes are written as journal records, and snapshots are written when compaction thresholds are reached. On startup, Fresh loads the newest valid snapshot and replays valid journal records.

The sync task only writes dirty state. Clean models are skipped.

## Build And Verification

Compile an example with PlatformIO:

```sh
pio ci examples/Basic --board esp32dev --lib . --project-option build_unflags=-std=gnu++11 --project-option build_flags=-std=gnu++20
```

The included examples are intended to compile for `esp32dev` with Arduino ESP32.

## Limitations

- Arduino ESP32 is the only target for now.
- Full document models are loaded into RAM.
- Public writes are not flash-durable until sync runs.
- Backup uses a MessagePack archive, not heavy compression.
- ESPAsyncWebServer integration is not included yet.
- ESP-IDF component packaging is not included yet.
- Runtime behavior should be validated on your target ESP32 board and flash partition layout.

## License

Fresh is intended to be released as open source under the MIT license.
