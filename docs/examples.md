# Examples

Fresh examples are topic-focused Arduino sketches. Start with `Basic`, then move to the example that matches the behavior you need.

## Basic

Path: [`../examples/Basic/Basic.ino`](../examples/Basic/Basic.ino)

Shows the smallest complete document flow:

* initialize `Fresh`
* create a `User` model
* insert a document
* find it by `_id`
* update it

Use this first when wiring Fresh into a new project.

## Crud

Path: [`../examples/Crud/Crud.ino`](../examples/Crud/Crud.ino)

Demonstrates document model operations:

* `create`
* `findById`
* `findOne`
* `find`
* `updateById`
* `updateOne`
* `update`
* `deleteById`
* `deleteOne`
* `deleteMany`

Use this when validating query and patch behavior.

## SyncAndStorage

Path: [`../examples/SyncAndStorage/SyncAndStorage.ino`](../examples/SyncAndStorage/SyncAndStorage.ino)

Demonstrates RAM-first writes and background persistence:

* custom `syncIntervalMS`
* storage usage before and after writes
* delayed background sync
* `storageInfo()`
* `model(name)` lookup

Use this when tuning persistence timing or explaining why an accepted write may not immediately appear in flash usage.

## StreamModel

Path: [`../examples/StreamModel/StreamModel.ino`](../examples/StreamModel/StreamModel.ino)

Demonstrates append-style records:

* `createModel(name, FreshModelType::Stream)`
* `append`
* `retrieve`
* filtered `retrieve`
* `FreshStreamRetrieveOptions`
* `streamTo(Print&)`

Use this for logs, telemetry records, and other append-heavy data.

## ValidatorsAndCallbacks

Path: [`../examples/ValidatorsAndCallbacks/ValidatorsAndCallbacks.ino`](../examples/ValidatorsAndCallbacks/ValidatorsAndCallbacks.ino)

Demonstrates validation and callback wiring:

* bool validators
* `FreshValidationResult` validators
* event callbacks
* sync callbacks
* custom time callback
* `std::bind` with private class methods

Use this when integrating Fresh into a class-based application.

## BackupStream

Path: [`../examples/BackupStream/BackupStream.ino`](../examples/BackupStream/BackupStream.ino)

Demonstrates chunked backup and restore:

* backup start/progress/end/error callbacks
* `startBackup()`
* repeated `readBackup(...)`
* `backupStatus()`
* `backupImport(data, length)`
* restore into another `Fresh` instance

Use this when building backup download, upload, or migration flows.

## ModelManagement

Path: [`../examples/ModelManagement/ModelManagement.ino`](../examples/ModelManagement/ModelManagement.ino)

Demonstrates model lifecycle helpers:

* create multiple models
* `renameModel`
* `dropModel`
* `dropModels`
* `dropAllModels`

Use this for setup tools, reset flows, and maintenance screens.

## Compiling examples

Compile an example with PlatformIO CI:

```sh
pio ci examples/Basic --board esp32dev --lib . --project-option build_unflags=-std=gnu++11 --project-option build_flags=-std=gnu++20
```

Run the same command for each example folder.
