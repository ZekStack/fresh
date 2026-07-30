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

Use this when tuning persistence timing or explaining why an accepted write may not immediately appear in storage usage.

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
* `backupStatus()` with `FreshBackupState`
* `backupImport(data, length)`
* restore into another `Fresh` instance

After `startBackup()`, keep reading chunks until `backupStatus().state` is `FreshBackupState::Finished`, `FreshBackupState::Cancelled`, or `FreshBackupState::Error`, or call `cancelBackup()` if the consumer stops. An undrained backup can occupy the sync task and delay normal persistence.

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

## LittleFSStorage

Path: [`../examples/LittleFSStorage/LittleFSStorage.ino`](../examples/LittleFSStorage/LittleFSStorage.ino)

Configures the managed LittleFS backend explicitly, including partition label, VFS mount path, open-file limit, formatting policy, grow-on-mount behavior, and result-aware capacity reporting.

## SDSPIStorage

Path: [`../examples/SDSPIStorage/SDSPIStorage.ino`](../examples/SDSPIStorage/SDSPIStorage.ino)

Configures Fresh-managed SD storage over SPI, including bus ownership, pins, mount path, frequency, and result-aware capacity reporting.

## SDMMCStorage

Path: [`../examples/SDMMCStorage/SDMMCStorage.ino`](../examples/SDMMCStorage/SDMMCStorage.ino)

Configures Fresh-managed SDMMC storage using target-default routing. See `storage.md` for custom GPIO-matrix routing.

## SameFilesystemBackup

Path: [`../examples/SameFilesystemBackup/SameFilesystemBackup.ino`](../examples/SameFilesystemBackup/SameFilesystemBackup.ino)

Streams a Fresh backup into `/backups/configuration.fresh` through lifecycle-safe `withStorage()` access, then explicitly synchronizes and closes the archive.

## CustomStorage

Path: [`../examples/CustomStorage/CustomStorage.ino`](../examples/CustomStorage/CustomStorage.ino)

Implements a non-VFS in-memory `FreshStorage` and `FreshFileBackend`, attaches it as caller-owned storage, persists a document, deinitializes, reinitializes, verifies the document reloads, propagates a storage-information failure, protects the database root, and blocks shutdown while an application file remains open.

## StorageLifecycleRegressionTest

Path: [`../examples/StorageLifecycleRegressionTest/StorageLifecycleRegressionTest.ino`](../examples/StorageLifecycleRegressionTest/StorageLifecycleRegressionTest.ino)

Uses the built-in LittleFS backend to verify:

* application access below the database root is rejected
* sibling application directories remain available
* `deinit()` returns `FreshStatus::Busy` while a file is open
* shutdown succeeds after `syncAndClose()`
* repeated init/deinit remains valid

## StorageFailureRegressionTest

Path: [`../examples/StorageFailureRegressionTest/StorageFailureRegressionTest.ino`](../examples/StorageFailureRegressionTest/StorageFailureRegressionTest.ino)

Implements a custom fault-injecting backend and verifies that `FreshFile` propagates:

* short writes and `Print` write errors
* synchronization failures
* close failures
* unavailable reads and native error values

## LegacyDatabaseP4RegressionTest

Path: [`../examples/LegacyDatabaseP4RegressionTest/LegacyDatabaseP4RegressionTest.ino`](../examples/LegacyDatabaseP4RegressionTest/LegacyDatabaseP4RegressionTest.ino)

ESP32-P4 two-boot runtime regression for Fresh 0.1.1 database compatibility. The first boot writes a byte-compatible legacy manifest and a 2,048-record `journal.log` through Arduino LittleFS, then restarts. The second boot mounts the existing partition through Fresh 0.2.0, replays on a CPU0-pinned task, verifies every record, checks that a priority-zero CPU0 probe ran during replay, and validates cached VFS file-size growth across write, append, and read opens.

Run this sketch with the five-second CPU0 idle-task watchdog used by the target application. CI compiles the sketch for ESP32-P4 but cannot execute the two-boot hardware test.

## SelfTest

Path: [`../examples/SelfTest/SelfTest.ino`](../examples/SelfTest/SelfTest.ino)

Destructive Fresh development self-test for persistence, recovery, backup, and shutdown behavior.

It uses `/fresh_selftest`, `/fresh_selftest_src`, and `/fresh_selftest_dst`, touches internal storage files, and should only be run on a test device or test partition. SelfTest intentionally depends on the current Fresh storage layout and may need updates when the storage format changes.

SelfTest and the regression sketches are compiled by CI, but they are not executed in CI. Run them manually on ESP32 hardware. A successful SelfTest run ends like this:

```txt
Fresh SelfTest starting
[PASS] create -> forceSync -> reload
...
SelfTest complete: 16 passed, 0 failed
```

## Compiling examples

Compile an example with PlatformIO CI:

```sh
pio ci examples/Basic --board esp32dev --lib . --project-option build_unflags=-std=gnu++11 --project-option build_flags=-std=gnu++20
```

Run the same command for each example folder.
