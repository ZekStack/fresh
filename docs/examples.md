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
* bounded `retrieve`
* filtered `retrieve`
* `FreshStreamRetrieveOptions`
* `streamTo(Print&)`

Use this for logs, telemetry records, and other append-heavy data. Prefer `reverse = true` with a `limit` for normal log views so reads stay bounded.

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

After `startBackup()`, keep reading chunks until backup completion/error, or call `cancelBackup()` if the consumer stops. An undrained backup can occupy the sync task and delay normal persistence.

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

## SelfTest

Path: [`../examples/SelfTest/SelfTest.ino`](../examples/SelfTest/SelfTest.ino)

Destructive Fresh development self-test for persistence, recovery, backup, and shutdown behavior.

It uses `/fresh_selftest`, `/fresh_selftest_src`, and `/fresh_selftest_dst`, touches internal storage files, and should only be run on a test device or test partition. SelfTest intentionally depends on the current Fresh storage layout and may need updates when the storage format changes.

SelfTest is compiled by CI through the examples build loop, but it is not executed in CI. Run it manually on ESP32 hardware. A successful run ends like this:

```txt
Fresh SelfTest starting
[PASS] create -> forceSync -> reload
...
SelfTest complete: 8 passed, 0 failed
```

## Compiling examples

Compile an example with PlatformIO CI:

```sh
pio ci examples/Basic --board esp32dev --lib . --project-option build_unflags=-std=gnu++11 --project-option build_flags=-std=gnu++20
```

Run the same command for each example folder.
