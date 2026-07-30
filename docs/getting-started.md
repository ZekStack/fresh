# Getting Started

This guide shows the smallest useful Fresh flow: initialize the database, open a model, create a document, read it back, and update it.

## Requirements

Fresh targets Arduino ESP32 projects. The default backend is managed LittleFS; Fresh 0.2.0 also supports managed SDSPI, managed SDMMC, and custom storage backends.

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

## Initialize Fresh

Create one `Fresh` instance and initialize it with a logical database root path.

```cpp
#include <Arduino.h>
#include <Fresh.h>

Fresh db;

void setup() {
    Serial.begin(115200);

    FreshResult result = db.init("/fresh_app");
    if (!result) {
        Serial.println(result.message.c_str());
        return;
    }
}
```

The zero-configuration form mounts the default LittleFS partition, loads existing model data into RAM, starts the background sync task, and returns a `FreshResult`.

Select another built-in backend through `FreshConfig`:

```cpp
FreshConfig config;
config.storageType = FreshStorageType::SD;
config.sd.interface = FreshSDInterface::SPI;
config.sd.spi.chipSelectPin = GPIO_NUM_10;
config.sd.spi.clockPin = GPIO_NUM_12;
config.sd.spi.mosiPin = GPIO_NUM_11;
config.sd.spi.misoPin = GPIO_NUM_13;

FreshResult result = db.init("/fresh_app", config);
```

See [`storage.md`](storage.md) before selecting SD or implementing custom storage. Storage formatting is disabled by default.

## Create a model

Models are lightweight handles owned by the database.

```cpp
FreshModelResult usersResult = db.createModel("User");
if (!usersResult) {
    Serial.println(usersResult.message.c_str());
    return;
}
FreshModel users = usersResult.model;
```

Use `createModel(name)` for a normal document model. Use `createModel(name, FreshModelType::Stream)` for an append-style stream model.

## Create a document

Fresh stores ArduinoJson `JsonDocument` values. `create()` intentionally updates the input document in place with `_id`, `createdAt`, and `updatedAt`.

```cpp
JsonDocument user;
user["name"] = "Panna";
user["age"] = 19;

FreshResult created = users.create(user);
if (!created) {
    Serial.println(created.message.c_str());
    return;
}

const char *id = user["_id"].as<const char *>();
```

The time fields use the callback registered with `onTimeGet()`. If no callback is registered, Fresh uses its default time source.

## Read and update

```cpp
FreshResult found = users.findById(id);
if (found) {
    serializeJson(found.doc, Serial);
    Serial.println();
}

JsonDocument patch;
patch["age"] = 20;

FreshResult updated = users.updateById(id, patch);
if (!updated) {
    Serial.println(updated.message.c_str());
}
```

Patch documents merge into the existing document and update `updatedAt`.

Update results default to count-only to avoid copying documents into RAM. Pass `FreshReturn::ChangedDocs` or `FreshReturn::AllDocs` when the updated JSON payload is needed.

## Persistence behavior

Fresh is RAM-first. A successful write result means the operation was accepted into memory. It does not mean the change has already been written to storage.

The sync task persists dirty state to the selected backend later. It captures a batch under a short database lock, then performs storage operations without holding the global database mutex. If power is lost before the next sync, recent accepted changes can be lost.

Use the configured `syncIntervalMS` for normal background persistence. Call `flush()` when pending operations must be durable before continuing, such as immediately before a controlled reboot; it does not force a full snapshot. `forceSyncAsync()` requests a forced checkpoint through the sync task. `forceSync()` runs the same forced captured-state checkpoint synchronously, so reserve it for explicit compaction. Writes accepted after a sync captures its batch remain pending for a later sync.

Call `deinit()` when a local or test database instance should shut down explicitly. It waits for the sync task to exit and performs a final forced checkpoint by default. Use `deinit({.sync = false})` only when stopping quickly is more important than persisting dirty RAM state that has not synced yet.

## Application files

Use `withStorage()` to create or open application files on the active backend without racing with shutdown:

```cpp
FreshFile archive;
FreshResult opened = db.withStorage(
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

Do not access files below the configured database root. Close every `FreshFile` before calling `deinit()`.

## Next steps

Start with [`../examples/Basic/Basic.ino`](../examples/Basic/Basic.ino), then read [`examples.md`](examples.md) to choose a storage, backup, or regression example. See [`migration-0.2.0.md`](migration-0.2.0.md) when upgrading an existing application.
