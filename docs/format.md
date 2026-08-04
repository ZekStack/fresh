# Formatting storage

`Fresh::format()` performs a destructive filesystem-level format of the complete storage backend owned by the initialized database.

```cpp
FreshResult result = database.format();
if (!result) {
    Serial.printf("Format failed: %s\n", result.message.c_str());
    return;
}

FreshModelResult settings = database.createModel("settings");
```

> **Warning:** `format()` deletes the Fresh database, application files accessed through `db.storage()`, and every other file on the same configured volume. The operation cannot be undone.

Formatting is different from `dropAllModels()`:

- `dropAllModels()` removes Fresh models while preserving other files on the volume.
- `format()` recreates the filesystem and starts Fresh again as an empty database.

A filesystem format is not a secure erase guarantee. Applications that require cryptographic sanitization or media-specific erase commands must implement that policy outside Fresh.

## Lifecycle behavior

`format()` is synchronous. Fresh performs the following lifecycle transition:

1. rejects new database and application-storage operations;
2. cancels backup work and stops the background sync task without a final sync;
3. closes and invalidates all tracked `FreshFile` handles;
4. formats the complete storage backend;
5. invalidates all previously acquired `FreshModel` handles;
6. recreates the configured database root and an empty durable manifest;
7. restarts the sync task and re-enables the existing storage facade.

On success, the same `Fresh` instance and previously copied `FreshStorageAccess` facades remain usable. Previously acquired model and file handles do not become valid again.

`FreshResult::affectedCount` reports the number of model handles invalidated by the operation.

## Supported backends

The built-in LittleFS, SDSPI, SDMMC, and eMMC backends support formatting.

Custom backends are unsupported by default. A custom backend must explicitly opt in and implement whole-volume formatting:

```cpp
class CustomStorage final : public FreshStorage {
  private:
    bool supportsFormat() const override {
        return true;
    }

    FreshResult formatBackend() override {
        // Erase and recreate the complete underlying filesystem or volume.
        return FreshResult::success("custom storage formatted");
    }
};
```

`formatBackend()` is called only after Fresh has stopped background work and closed every tracked file. The backend must return success only when the volume is mounted and ready for immediate file operations.

## Failure behavior

Failures before the native formatter starts restore the running database when possible.

Once native formatting begins, failures are fail-closed because the previous filesystem contents can no longer be trusted. Fresh invalidates model handles, disables the application storage facade, and does not restart background synchronization. Use a controlled shutdown without sync and then initialize the storage again:

```cpp
FreshResult formatted = database.format();
if (!formatted) {
    database.deinit(FreshDeinitOptions{
        .sync = false,
        .timeoutMS = 2000,
    });
}
```

Inspect `storageInfo().nativeError` before deinitialization when backend-specific diagnostics are needed.
