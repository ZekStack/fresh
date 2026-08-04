# Getting started

## Default LittleFS

```cpp
#include <Arduino.h>
#include <Fresh.h>

Fresh db;

void setup() {
    Serial.begin(115200);

    FreshConfig config;
    FreshInitResult initialized = db.init("/fresh", config);
    if (!initialized) {
        Serial.println(initialized.message.c_str());
        return;
    }

    FreshModelResult settings = db.createModel("Settings");
    if (!settings && settings.status != FreshStatus::ModelExists) {
        Serial.println(settings.message.c_str());
        return;
    }

    FreshModel model = db.model("Settings");

    JsonDocument document;
    document["name"] = "controller";
    document["enabled"] = true;

    FreshResult created = model.create(document);
    if (!created) {
        Serial.println(created.message.c_str());
        return;
    }

    db.forceSync();
}

void loop() {
    delay(1000);
}
```

`db.init("/fresh", config)` creates a default `FreshLittleFSStorage`, mounts it through ESP-IDF, and stores the database below `/fresh` on that backend.

## Explicit storage

```cpp
FreshLittleFSConfig storageConfig;
storageConfig.partitionLabel = "spiffs";
storageConfig.mountPath = "/littlefs";
storageConfig.maxOpenFiles = 12;
storageConfig.formatOnMountFailure = false;

FreshInitResult initialized = db.init(
    "/fresh",
    config,
    FreshLittleFSStorage(storageConfig)
);
```

The same database code can use `FreshSDStorage`, `FreshEMMCStorage`, or a custom `FreshStorage` implementation.

## Application files

Fresh exposes the active backend through `db.storage()`:

```cpp
FreshResult directory = db.storage().ensureDirectory("/backups");
if (!directory) return;

const uint8_t payload[] = {1, 2, 3, 4};
FreshResult written = db.storage().writeFile(
    "/backups/settings.bin",
    payload,
    sizeof(payload)
);
```

Use `FreshFile` for streaming:

```cpp
FreshFile file;
FreshResult opened = db.storage().open(
    "/backups/archive.fresh",
    FreshOpenMode::Write,
    file
);
if (!opened) return;

file.write(buffer, length);
file.syncAndClose();
```

Do not access the configured database root through `db.storage()`. Fresh protects that path from application operations.

## Shutdown

```cpp
FreshResult stopped = db.deinit();
```

By default, `deinit()` performs a final sync, stops the background task, closes internal files, unmounts the backend, and releases it. It returns `FreshStatus::Busy` while application `FreshFile` handles remain open.
