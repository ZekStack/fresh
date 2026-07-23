# Restore modes and protected models

Fresh can restore a validated backup archive using either selected-model replacement or exact full-registry replacement.

## API

Existing imports keep their original behavior:

```cpp
FreshResult result = db.backupImport(input);
```

This is equivalent to `FreshRestoreMode::ReplaceSelected`.

To choose restore behavior explicitly:

```cpp
FreshRestoreOptions options;
options.mode = FreshRestoreMode::ReplaceAll;
options.protectedModels = {
    "SystemIdentity",
    "RemoteAccess",
};

FreshResult result = db.backupImport(input, options);
```

Memory-backed input supports the same options:

```cpp
FreshResult result = db.backupImport(data, length, options);
```

## `ReplaceSelected`

`ReplaceSelected` replaces or creates only models contained in the archive. Existing models absent from the archive remain unchanged.

```text
Existing: User, Hardware, Logs
Archive:  User, Hardware
Result:   User*, Hardware*, Logs
```

This is the default and preserves compatibility with the original `backupImport()` overloads.

## `ReplaceAll`

`ReplaceAll` treats the archive as the complete model registry except for explicitly protected models.

```text
Existing: User, Hardware, Logs
Archive:  User, Hardware
Result:   User*, Hardware*
```

Unprotected destination models absent from the archive are removed from the live registry and from the next persisted manifest.

## Protected models

Every name in `FreshRestoreOptions::protectedModels` must identify an existing active model.

Protected models:

- are preserved during `ReplaceAll` when absent from the archive;
- cannot be replaced by either restore mode;
- cause the whole restore to fail if the archive contains the same model name.

Fresh rejects invalid, duplicate, or missing protected names before parsing the archive. A protected-model collision also fails before the live model registry changes. Models are never silently skipped.

## Empty archives

An archive with zero models has mode-specific behavior:

- `ReplaceSelected`: successful no-op;
- `ReplaceAll`: removes every unprotected model;
- protected models remain unchanged.

## Validators

When an archive replaces an existing model, Fresh attaches the destination model's current validator to the detached imported state and validates every restored record before commit.

There is no validator-bypass option. Any validation failure leaves the live database unchanged.

## Atomic in-memory commit

Restore follows these stages:

1. capture the current model registry and revisions;
2. parse and validate the complete archive into detached model states;
3. calculate created, replaced, removed, and protected models;
4. acquire sync and database locks;
5. verify the database did not change during parsing;
6. precompute all revision increments;
7. swap the live registry once;
8. mark the manifest dirty and notify the sync task.

Failures before the registry swap do not modify live model state.

## Result accounting

On success, `FreshResult::affectedCount` is:

```text
created models + replaced models + removed models
```

Preserved and protected models are not counted.

## Persistence boundary

The restore updates RAM first and schedules normal Fresh persistence. Call `forceSync()` when the application needs the restored registry and snapshots persisted before continuing or rebooting.

This is an atomic in-memory restore, not yet a power-loss-transactional restore. A future durable restore API will write replacement storage generations and commit an alternate manifest slot before switching the live registry.

## Stream lifecycle

`backupImport(Stream&)` consumes the supplied stream and does not rewind it. When an application first calls `inspectBackup()`, it should close and reopen the uploaded file before restore.

For Core-style structural restore, restore should be followed by a prepared reboot so feature managers reopen model handles from the new registry.
