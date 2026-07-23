# Restore modes and protected models

Fresh supports two restore paths over the same validated backup archive:

- `backupImport()` performs the original atomic in-memory registry replacement and schedules normal persistence.
- `restoreBackup()` performs a durable transactional restore and commits storage before changing the live registry.

Both APIs support selected-model and full-registry semantics through `FreshRestoreOptions`.

## Restore options

```cpp
FreshRestoreOptions options;
options.mode = FreshRestoreMode::ReplaceAll;
options.protectedModels = {
    "SystemIdentity",
    "RemoteAccess",
};
```

### `ReplaceSelected`

`ReplaceSelected` replaces or creates only models contained in the archive. Existing models absent from the archive remain unchanged.

```text
Existing: User, Hardware, Logs
Archive:  User, Hardware
Result:   User*, Hardware*, Logs
```

### `ReplaceAll`

`ReplaceAll` treats the archive as the complete model registry except for explicitly protected models.

```text
Existing: User, Hardware, Logs
Archive:  User, Hardware
Result:   User*, Hardware*
```

Unprotected destination models absent from the archive are removed from the committed manifest.

## Protected models

Every name in `FreshRestoreOptions::protectedModels` must identify an existing active model.

Protected models:

- are preserved during `ReplaceAll` when absent from the archive;
- cannot be replaced by either restore mode;
- cause the whole restore to fail if the archive contains the same model name.

Fresh rejects invalid, duplicate, or missing protected names before parsing the archive. Models are never silently skipped.

## In-memory import

Existing imports keep their original behavior:

```cpp
FreshResult result = db.backupImport(input);
```

This is equivalent to `FreshRestoreMode::ReplaceSelected`.

To choose the mode explicitly:

```cpp
FreshResult result = db.backupImport(input, options);
```

`backupImport()` parses and validates the complete archive into detached model states, verifies that the database did not change, swaps the live registry once, and marks replacement snapshots and the manifest dirty.

Call `forceSync()` when the application must persist this RAM-first import before continuing or rebooting.

## Durable transactional restore

Production restore should use:

```cpp
FreshResult result = db.restoreBackup(input, options);
```

Memory-backed input is also supported:

```cpp
FreshResult result = db.restoreBackup(data, length, options);
```

`restoreBackup()` follows this sequence:

1. force the current database to a clean durable checkpoint;
2. acquire exclusive backup and storage synchronization;
3. capture the current registry and model revisions;
4. parse and validate the entire archive into detached model states;
5. calculate created, replaced, removed, preserved, and protected models;
6. assign new immutable storage IDs to every imported model;
7. preflight the exact snapshot and manifest storage requirement;
8. write and read back every replacement snapshot under its new storage ID;
9. verify that the live database still matches the captured revisions;
10. commit the alternate manifest slot;
11. switch the in-memory model registry;
12. remove obsolete storage on a best-effort basis.

The existing manifest remains the active database definition until every replacement snapshot has been written and semantically verified.

## Power-loss boundary

The alternate manifest slot is the only durable commit point.

- Failure or reset before manifest commit loads the complete original database after reboot.
- Failure or reset after manifest commit loads the complete restored database after reboot.
- A reset can leave unreferenced old or staged model directories, but it cannot produce a manifest that mixes old and incomplete replacement data.

Obsolete storage removal happens after the commit and is not required for restore correctness. A cleanup failure is reported as successful restore with deferred cleanup.

## Current-database synchronization

A durable restore begins with `forceSync()` and then verifies that the captured registry is clean. This is required because models preserved by `ReplaceSelected` or `protectedModels` continue to use their existing durable storage IDs.

If another task modifies the database after the forced checkpoint, restore returns `FreshStatus::Busy` rather than committing a manifest based on stale preserved state.

The restore holds Fresh's storage synchronization lock while parsing and staging. Normal persistence resumes after restore returns.

## Validators

When an archive replaces an existing model, Fresh attaches the destination model's current validator to the detached imported state and validates every restored record before any durable write.

There is no validator-bypass option. Any validation failure leaves the original manifest and live registry unchanged.

## Empty archives

An archive with zero models has mode-specific behavior:

- `ReplaceSelected`: successful no-op;
- `ReplaceAll`: removes every unprotected model;
- protected models remain unchanged.

A durable empty `ReplaceAll` restore only commits the new manifest because there are no replacement snapshots to stage.

## Result accounting

On success, `FreshResult::affectedCount` is:

```text
created models + replaced models + removed models
```

Preserved and protected models are not counted.

## Model handles

Handles for replaced or removed models become invalid after either restore path. Models preserved by `ReplaceSelected` or protection retain their existing state and handles.

Core-style structural restore should still be followed by a prepared reboot so all feature managers reopen model handles against the committed registry.

## Stream lifecycle

Both restore APIs consume the supplied stream and do not rewind it. When an application first calls `inspectBackup()`, it should close and reopen the uploaded file before restore.

## Fault-injection testing

When Fresh is built with `FRESH_TESTING`, include `FreshRestoreTesting.h` and configure a one-shot restore failure:

```cpp
FreshTestConfigureRestoreFailure(
    FreshRestoreTestFailurePoint::BeforeManifestCommit
);
```

The available boundaries cover snapshot staging, manifest commit, the post-commit/pre-registry-switch reset window, and pre-cleanup behavior. These hooks do not exist in normal production behavior.
