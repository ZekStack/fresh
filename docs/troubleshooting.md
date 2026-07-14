# Troubleshooting

## `init()` fails

Print the returned message and status:

```cpp
FreshResult result = db.init("/fresh_app");
if (!result) {
    Serial.printf("Fresh init failed: %s\n", result.message.c_str());
}
```

Common causes:

* LittleFS cannot mount.
* The database path is invalid.
* Existing data is corrupt.
* The sync task cannot be created because of memory pressure.

Fresh `0.1.0` intentionally does not migrate `0.0.x` storage. Erase the development database once when moving to the v2 journal/checkpoint format.

If LittleFS mount recovery is acceptable for your product, set `eraseOnFileSystemFailure = true`. This can erase stored data, so keep it disabled unless that tradeoff is intentional.

## Data is missing after power loss

Fresh is RAM-first. A successful public write means the change was accepted into memory. It may not have reached LittleFS yet.

Reduce the loss window by lowering `syncIntervalMS`. When a caller must not continue until captured operations are durable, call `flush()` and check its result. Use `forceSyncAsync()` or `forceSync()` only when an explicit checkpoint snapshot is also required. Any writes accepted after a sync captures its batch remain pending for a later sync.

## Flash usage does not change immediately

Background sync is dirty-only, interval-based, and threshold-compacted. `storageInfo()` may look unchanged immediately after `create`, `update`, `delete`, or `append`.

Wait longer than `syncIntervalMS`, then check storage again.

## Writes fail with storage limits

`FreshStatus::SizeLimitExceeded` means the serialized document, stream entry, journal payload, or snapshot exceeded the configured limit.

`FreshStatus::StorageFull` means Fresh could not preflight the next sync write while preserving `FreshConfig::minFreeBytes`.

Use `storageInfo()` to inspect LittleFS space. If larger payloads are intentional, raise `maxDocumentBytes`, `maxJournalRecordBytes`, or `maxSnapshotBytes` only when the device has enough RAM and LittleFS capacity.

## Model creation fails

Check the returned `FreshModelResult` message and status.

```cpp
FreshModelResult usersResult = db.createModel("User");
if (!usersResult) {
    Serial.println(usersResult.message.c_str());
    return;
}
FreshModel users = usersResult.model;
```

`createModel(...)` can report invalid names, uninitialized databases, stopping databases, dropped models, or an existing model with a different type.

Use `db.model("User")` when you expect the model to already exist.

## Validator failures

Validators run on create and update. If a validator rejects a document, the returned `FreshResult` should have `FreshStatus::ValidationFailed`.

For clearer messages, use a `FreshValidationResult` validator:

```cpp
model.setValidator([](const JsonDocument &doc) {
    bool valid = !doc["type"].isNull();
    return FreshValidationResult{
        .result = valid,
        .message = valid ? "ok" : "Document requires type"
    };
});
```

## Backup returns busy or not running

`startBackup()` can fail with `FreshStatus::Busy` if a backup is already running or the sync task is occupied.

`readBackup(...)`, `backupStatus()`, or `cancelBackup()` can report backup-not-running state when no backup is active. Use `backupStatus().state` for lifecycle control and `backupStatus().result` for detailed diagnostics.

After `startBackup()`, keep calling `readBackup(...)` until backup completion/error, or call `cancelBackup()` if the reader stops. Backup generation writes into a bounded buffer from the sync task; if the buffer is not drained, normal persistence can stop progressing until space is available.

Typical backup loop:

```cpp
FreshResult started = db.startBackup();
if (!started) {
    Serial.println(started.message.c_str());
    return;
}

uint8_t buffer[256];
size_t read = db.readBackup(buffer, sizeof(buffer), 50);
FreshBackupStatus status = db.backupStatus();
if (status.state == FreshBackupState::Finished) {
    Serial.println(status.result.message.c_str());
}
```

Use `onBackupEnd` and `onBackupError` callbacks to know when a backup has completed or failed.

## Stream reads return no records

Confirm the model was created as a stream model:

```cpp
FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
if (!logsResult) {
    Serial.println(logsResult.message.c_str());
    return;
}
FreshModel logs = logsResult.model;
```

`append()` is for stream models. Document CRUD methods are for general models.

## Compile errors about language standard

Fresh expects C++20 for the current codebase.

```ini
build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```
