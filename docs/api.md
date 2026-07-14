# API Reference

This page summarizes the public API declared in `src/Fresh.h`.

## Result model

Fresh does not throw exceptions. Operations return `FreshResult`.

```cpp
FreshResult result = db.init("/fresh_app");
if (!result) {
    Serial.println(result.message.c_str());
}
```

`FreshResult` fields:

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `FreshStatus`. |
| `message` | Human-readable status message. |
| `doc` | Optional returned `JsonDocument`. |
| `affectedCount` | Number of affected documents, records, or models. |

`FreshStatus` values include `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `FileSystemError`, `ModelExists`, `ModelNotFound`, `DocumentNotFound`, `InvalidModel`, `ValidationFailed`, `OutOfMemory`, `UnsupportedOperation`, `CorruptData`, `StorageFull`, `SizeLimitExceeded`, `Busy`, `BackupNotRunning`, `Cancelled`, `Timeout`, and `InternalError`.

`FreshModelResult` is returned by `createModel(...)`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `FreshStatus`. |
| `message` | Human-readable status message. |
| `model` | Opened `FreshModel` handle on success. |
| `affectedCount` | `1` when a model was created or opened. |

## FreshConfig

`FreshConfig` is passed to `Fresh::init()`.

```cpp
FreshConfig config;
config.syncIntervalMS = 5000;
config.syncTaskStackSize = 8192;

FreshResult result = db.init("/fresh_app", config);
```

See [`configuration.md`](configuration.md) for every option and default.

Storage limits can return `StorageFull` when sync preflight cannot preserve the configured LittleFS reserve, or `SizeLimitExceeded` when a document, stream entry, journal record, or snapshot exceeds configured serialized-size limits.

## Fresh

`Fresh` owns a database instance, model registry, sync task, callbacks, storage access, and backup state.

```cpp
Fresh db;
FreshResult result = db.init("/fresh_app");
```

Common methods:

| Method | Purpose |
| --- | --- |
| `init(path, config)` | Mount/load the database and start the sync task. |
| `deinit(options)` | Stop the sync task, optionally run a final forced checkpoint, and release runtime state. |
| `createModel(name)` | Create or open a model using the default model type and return `FreshModelResult`. |
| `createModel(name, type)` | Create or open a general or stream model and return `FreshModelResult`. |
| `model(name)` | Look up an existing model handle. |
| `dropModel(name)` | Drop one model. |
| `dropModels({...})` | Drop selected models. |
| `dropAllModels()` | Drop every model. |
| `renameModel(oldName, newName)` | Rename a model. |
| `flush()` | Block until captured pending operations are journaled, without forcing a checkpoint snapshot. |
| `forceSyncAsync()` | Request a forced checkpoint for dirty state captured by the sync task. |
| `forceSync()` | Run a blocking forced checkpoint for captured dirty state that touches flash in the caller context. |
| `storageInfo()` | Return LittleFS total, used, and free bytes. |
| `diagnostics()` | Return model load diagnostics collected during `init()`. |

`FreshDeinitOptions` controls explicit shutdown:

```cpp
db.deinit();
db.deinit({.sync = false});
db.deinit({.sync = true, .timeoutMS = 5000});
```

`sync` defaults to `true`. When `sync` is `false`, Fresh stops without a final checkpoint, so dirty RAM state that has not already synced may be lost. The destructor calls `deinit({.sync = true, .timeoutMS = 2000})` because destructors should not block indefinitely, but automatic destructor cleanup is best-effort only. If shutdown times out, the destructor cannot report the failure. Applications that need deterministic shutdown or guaranteed final persistence should call `FreshResult result = db.deinit();` manually and check the result before the object is destroyed.

String helper methods:

| Method | Purpose |
| --- | --- |
| `eventToString(type)` | Convert `FreshEventType` to text. |
| `backupStateToString(state)` | Convert `FreshBackupState` to text. |
| `backupErrorToString(error)` | Convert `FreshBackupError` to text. |
| `loadStatusToString(status)` | Convert `FreshLoadStatus` to text. |
| `statusToString(status)` | Convert `FreshStatus` to text. |

## Storage info

`FreshStorageInfo` is returned by `Fresh::storageInfo()`.

| Field | Meaning |
| --- | --- |
| `totalBytes` | Total LittleFS bytes reported by the filesystem. |
| `usedBytes` | Used LittleFS bytes reported by the filesystem. |
| `freeBytes` | Calculated free bytes. |

## Diagnostics

`FreshDiagnostics` is returned by `Fresh::diagnostics()` after `init()`.

```cpp
FreshDiagnostics diagnostics = db.diagnostics();
for (const FreshModelLoadInfo &load : diagnostics.modelLoads) {
    Serial.printf("%s: %s\n", load.modelName.c_str(), db.loadStatusToString(load.status));
}
```

`FreshModelLoadInfo` includes `modelName`, `modelType`, `status`, `degraded`, and `message`.

`FreshLoadStatus` values include `LoadedOk`, `LoadedWithRecoveredJournal`, `LoadedWithCorruptSnapshot`, `LoadedWithCorruptJournal`, and `FailedToLoad`.

## FreshModel

`FreshModel` is a handle to a database-owned model.

```cpp
FreshModelResult usersResult = db.createModel("User");
if (!usersResult) {
    Serial.println(usersResult.message.c_str());
    return;
}
FreshModel users = usersResult.model;
```

Shared methods:

| Method | Purpose |
| --- | --- |
| `name()` | Return the model name. |
| `type()` | Return `FreshModelType::General` or `FreshModelType::Stream`. |
| `setValidator(boolValidator)` | Register a validator that returns `bool`. |
| `setValidator(resultValidator)` | Register a validator that returns `FreshValidationResult`. |

Document model methods:

| Method | Purpose |
| --- | --- |
| `create(doc)` | Insert a document and intentionally add `_id`, `createdAt`, and `updatedAt` to the input document. |
| `findById(id)` | Find one document by `_id`. |
| `findOne(field, value)` | Find the first matching field/value document. |
| `find(predicate)` | Find matching documents with a custom predicate. |
| `updateById(id, patch, returnMode)` | Patch one document by `_id`. |
| `updateOne(predicate, patch, returnMode)` | Patch the first matching document. |
| `update(predicate, patch, returnMode)` | Patch every matching document. |
| `deleteById(id)` | Delete one document by `_id`. |
| `deleteOne(predicate)` | Delete the first matching document. |
| `deleteMany(predicate)` | Delete every matching document. |

`FreshReturn` controls update result payload size:

| Value | Result payload |
| --- | --- |
| `FreshReturn::None` | Default. Return `affectedCount` only and leave `doc` empty. |
| `FreshReturn::ChangedDocs` | Return only changed documents in `doc`. |
| `FreshReturn::AllDocs` | Return the full model in `doc` after updates. |

Stream model methods:

| Method | Purpose |
| --- | --- |
| `append(doc)` | Append one stream record. |
| `append(doc, options)` | Append one record and optionally retain only the newest bounded number of entries. |
| `retrieve()` | Return all stream records into memory. Prefer bounded options for append-style logs. |
| `retrieve(options)` | Return records with offset, limit, and reverse options. |
| `retrieve(predicate, options)` | Return filtered records with options. |
| `streamTo(Print&)` | Write stream records to an Arduino `Print`. |

`FreshStreamRetrieveOptions` fields:

| Field | Meaning |
| --- | --- |
| `offset` | Number of matching records to skip. |
| `limit` | Maximum records to return. `0` means no explicit limit. |
| `reverse` | Read newest records first when `true`. |

`FreshStreamAppendOptions::maxEntries` is `0` for an unbounded append. A positive value applies retention atomically with the append and is stored in the journal record, so crash recovery produces the same bounded stream state.

## Validators

Validators run on create and update.

```cpp
users.setValidator([](const JsonDocument &doc) {
    return !doc["name"].isNull();
});
```

Use `FreshValidationResult` when a custom failure message is useful.

```cpp
users.setValidator([](const JsonDocument &doc) {
    bool valid = !doc["type"].isNull() && !doc["pin"].isNull();
    return FreshValidationResult{
        .result = valid,
        .message = valid ? "ok" : "Sensor requires type and pin"
    };
});
```

## Callbacks

Callbacks use `std::function`, so lambdas and `std::bind` both work.

| Method | Callback |
| --- | --- |
| `onEvent(callback)` | Receives `FreshEvent`. |
| `onSync(callback)` | Receives sync `FreshResult`. |
| `onTimeGet(callback)` | Returns `uint64_t` for `_id`, `createdAt`, and `updatedAt` time data. |
| `onBackupStart(callback)` | Receives `FreshBackupInfo` when backup starts. |
| `onBackupProgress(callback)` | Receives backup progress. |
| `onBackupEnd(callback)` | Receives backup completion. |
| `onBackupError(callback)` | Receives backup errors. |

`FreshEventType` values include model lifecycle events, document events, stream append, sync events, and backup events.

Callbacks are notification hooks. Do not call `deinit()`, `flush()`, `forceSync()`, `forceSyncAsync()`, `startBackup()`, `backupImport()`, or long-blocking code from callbacks. Post work to another task instead.

## Backup

Backup generation runs through the sync task and is read in chunks.

> [!IMPORTANT]
> After `startBackup()`, repeatedly call `readBackup()` until `backupStatus().state` reports `FreshBackupState::Finished`, `FreshBackupState::Cancelled`, or `FreshBackupState::Error`, or until your backup callbacks fire. If the backup output is not drained or cancelled, the sync task can remain occupied and normal persistence may stop progressing. Use `cancelBackup()` if the consumer stops reading.

```cpp
FreshResult started = db.startBackup();
if (!started) {
    Serial.println(started.message.c_str());
}

uint8_t buffer[256];
size_t read = db.readBackup(buffer, sizeof(buffer), 50);
FreshBackupStatus status = db.backupStatus();
if (status.state == FreshBackupState::Finished) {
    Serial.println(status.result.message.c_str());
}
```

Backup methods:

| Method | Purpose |
| --- | --- |
| `startBackup()` | Request backup generation. |
| `readBackup(buffer, length, timeoutMS)` | Read generated backup bytes. |
| `backupStatus()` | Return `FreshBackupStatus` with typed lifecycle state and detailed result. |
| `cancelBackup()` | Cancel a running backup. |
| `backupImport(Stream&)` | Import backup data from an Arduino `Stream`. |
| `backupImport(data, length)` | Import backup data from memory. |

`FreshBackupStatus.state` is the stable lifecycle signal. `FreshBackupStatus.result` is the detailed success/failure result. `FreshBackupStatus::operator bool()` reflects only `result`, not lifecycle state.

`FreshBackupState` values are `NotRunning`, `Queued`, `Running`, `Finished`, `Cancelled`, and `Error`.

`FreshBackupInfo` includes `progress`, `total`, `size`, `estimatedSize`, `error`, and `result`.

`FreshBackupError` values include `None`, `AlreadyRunning`, `NotRunning`, `Cancelled`, `SerializationFailed`, `FileSystemError`, and `OutOfMemory`.
