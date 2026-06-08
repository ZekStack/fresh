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

`FreshStatus` values include `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `FileSystemError`, `ModelExists`, `ModelNotFound`, `InvalidModel`, `ValidationFailed`, `OutOfMemory`, `UnsupportedOperation`, `CorruptData`, `Busy`, `BackupNotRunning`, `Cancelled`, and `InternalError`.

## FreshConfig

`FreshConfig` is passed to `Fresh::init()`.

```cpp
FreshConfig config;
config.syncIntervalMS = 5000;
config.syncTaskStackSize = 8192;

FreshResult result = db.init("/fresh_app", config);
```

See [`configuration.md`](configuration.md) for every option and default.

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
| `createModel(name)` | Create or open a model using the default model type. |
| `createModel(name, type)` | Create or open a general or stream model. |
| `model(name)` | Look up an existing model handle. |
| `dropModel(name)` | Drop one model. |
| `dropModels({...})` | Drop selected models. |
| `dropAllModels()` | Drop every model. |
| `renameModel(oldName, newName)` | Rename a model. |
| `forceSyncAsync()` | Request a forced dirty-state checkpoint through the sync task. |
| `forceSync()` | Run a blocking forced dirty-state checkpoint that touches flash in the caller context. |
| `storageInfo()` | Return LittleFS total, used, and free bytes. |

String helper methods:

| Method | Purpose |
| --- | --- |
| `eventToString(type)` | Convert `FreshEventType` to text. |
| `backupErrorToString(error)` | Convert `FreshBackupError` to text. |
| `statusToString(status)` | Convert `FreshStatus` to text. |

## Storage info

`FreshStorageInfo` is returned by `Fresh::storageInfo()`.

| Field | Meaning |
| --- | --- |
| `totalBytes` | Total LittleFS bytes reported by the filesystem. |
| `usedBytes` | Used LittleFS bytes reported by the filesystem. |
| `freeBytes` | Calculated free bytes. |

## FreshModel

`FreshModel` is a handle to a database-owned model.

```cpp
FreshModel users = db.createModel("User");
if (!users) {
    Serial.println("Failed to open User model");
}
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
| `create(doc)` | Insert a document and add `_id`, `createdAt`, and `updatedAt`. |
| `findById(id)` | Find one document by `_id`. |
| `findOne(field, value)` | Find the first matching field/value document. |
| `find(predicate)` | Find matching documents with a custom predicate. |
| `updateById(id, patch)` | Patch one document by `_id`. |
| `updateOne(predicate, patch)` | Patch the first matching document. |
| `update(predicate, patch)` | Patch every matching document. |
| `deleteById(id)` | Delete one document by `_id`. |
| `deleteOne(predicate)` | Delete the first matching document. |
| `deleteMany(predicate)` | Delete every matching document. |

Stream model methods:

| Method | Purpose |
| --- | --- |
| `append(doc)` | Append one stream record. |
| `retrieve()` | Return stream records. |
| `retrieve(options)` | Return records with offset, limit, and reverse options. |
| `retrieve(predicate, options)` | Return filtered records with options. |
| `streamTo(Print&)` | Write stream records to an Arduino `Print`. |

`FreshStreamRetrieveOptions` fields:

| Field | Meaning |
| --- | --- |
| `offset` | Number of matching records to skip. |
| `limit` | Maximum records to return. `0` means no explicit limit. |
| `reverse` | Read newest records first when `true`. |

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

## Backup

Backup generation runs through the sync task and is read in chunks.

```cpp
FreshResult started = db.startBackup();
if (!started) {
    Serial.println(started.message.c_str());
}

uint8_t buffer[256];
size_t read = db.readBackup(buffer, sizeof(buffer), 50);
```

Backup methods:

| Method | Purpose |
| --- | --- |
| `startBackup()` | Request backup generation. |
| `readBackup(buffer, length, timeoutMS)` | Read generated backup bytes. |
| `backupStatus()` | Inspect backup state. |
| `cancelBackup()` | Cancel a running backup. |
| `backupImport(Stream&)` | Import backup data from an Arduino `Stream`. |
| `backupImport(data, length)` | Import backup data from memory. |

`FreshBackupInfo` includes `progress`, `total`, `size`, `estimatedSize`, `error`, and `result`.

`FreshBackupError` values include `None`, `AlreadyRunning`, `NotRunning`, `Cancelled`, `SerializationFailed`, `FileSystemError`, and `OutOfMemory`.
