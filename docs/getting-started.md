# Getting Started

This guide shows the smallest useful Fresh flow: initialize the database, open a model, create a document, read it back, and update it.

## Requirements

Fresh targets Arduino ESP32 projects and uses LittleFS for persistence.

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

Create one `Fresh` instance and initialize it with a LittleFS database root path.

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

`init()` mounts LittleFS, loads existing model data into RAM, starts the background sync task, and returns a `FreshResult`.

## Create a model

Models are lightweight handles owned by the database.

```cpp
FreshModel users = db.createModel("User");
if (!users) {
    Serial.println("Failed to open User model");
    return;
}
```

Use `createModel(name)` for a normal document model. Use `createModel(name, FreshModelType::Stream)` for an append-style stream model.

## Create a document

Fresh stores ArduinoJson `JsonDocument` values. `create()` updates the input document in place with `_id`, `createdAt`, and `updatedAt`.

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

## Persistence behavior

Fresh is RAM-first. A successful write result means the operation was accepted into memory. It does not mean the change has already been written to flash.

The sync task persists dirty state to LittleFS later. It captures a batch under a short database lock, then performs LittleFS writes without holding the global database mutex. If power is lost before the next sync, recent accepted changes can be lost.

Use the configured `syncIntervalMS` for normal background persistence. `forceSyncAsync()` requests a forced checkpoint through the sync task for dirty state captured when that sync starts. `forceSync()` runs the same forced captured-state checkpoint synchronously and touches flash in the caller context, so reserve it for advanced checkpoints. Writes accepted after a forced sync captures its batch remain pending for a later sync.

Call `deinit()` when a local or test database instance should shut down explicitly. It waits for the sync task to exit and performs a final forced checkpoint by default. Use `deinit({.sync = false})` only when stopping quickly is more important than persisting dirty RAM state that has not synced yet.

## Next steps

Start with [`../examples/Basic/Basic.ino`](../examples/Basic/Basic.ino), then read [`examples.md`](examples.md) to choose a more specific example.
