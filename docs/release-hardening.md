# v0.1.0 release hardening

Fresh `v0.1.0` uses storage format version 3. The format is intentionally incompatible with pre-release v2 data.

## Immutable model storage IDs

Each manifest model entry contains a logical `name`, immutable `storageId`, and model `type`. Model snapshots and journals live under:

```text
<database-root>/models/<storageId>/
```

Renaming changes only the logical name in the manifest. It never renames a directory and never rewrites a snapshot merely to change a name.

The sync commit order is:

1. Write active model journals and snapshots.
2. Commit the durable manifest slot.
3. Remove storage for models no longer referenced by the committed manifest.

A reset at any persistence boundary therefore exposes either the old committed manifest or the new committed manifest. A manifest never points at storage that Fresh has not attempted to make durable first. Cleanup failures may leave unreferenced storage, but cannot remove data still referenced by the committed manifest.

## Synchronization policy

`Fresh::_mutex` protects database lifetime state, the model registry, every mutable `FreshModel::State` field, callbacks, configuration observed by public operations, and the sync task handle.

`Fresh::_syncMutex` serializes filesystem sync operations. Code may acquire `_syncMutex` and later acquire `_mutex` while reconciling a captured batch. Public mutation code must not acquire `_syncMutex` while holding `_mutex`.

`FreshBackupRuntimeState::mutex` protects the backup ring and backup lifecycle fields. Avoid holding the database mutex while invoking user callbacks.

`FreshModel::name()` returns a value copy. Model validity, name, type, dropped state, and document operations are all checked while holding the database mutex.

## Shutdown lifetime

A bounded explicit `deinit()` may return `FreshStatus::Timeout`. The object then remains alive in its stopping state, and a later `deinit()` call can finish the shutdown.

The destructor performs an unbounded shutdown barrier. It does not destroy the exit semaphore, mutexes, backup state, callbacks, or models until the sync task has signaled that it made its final access to the `Fresh` object.

## Allocation failures

Fresh no longer supplies a null-returning allocator to standard containers. Fallible serialized-byte and backup-ring allocations use a move-only `FreshBuffer` with explicit allocation results. Allocation failure is reported as `FreshStatus::OutOfMemory`.

## Persisted-size ceilings

The absolute payload ceiling for manifest slots, snapshot slots, and journal records is 1 MiB. `FreshConfig` is rejected during `init()` when:

- `maxDocumentBytes`, `maxJournalRecordBytes`, or `maxSnapshotBytes` is zero or above 1 MiB;
- `maxJournalRecordBytes` does not exceed `maxDocumentBytes`;
- `maxSnapshotBytes` is below `maxDocumentBytes`;
- `backupBufferSize` is zero or above 1 MiB;
- sync task interval, stack, priority, or core settings are invalid.

Reader and writer paths apply the same absolute ceiling and the configured journal/snapshot limits. All persisted 32-bit length conversions are checked first.

## Validation

`examples/ReleaseHardeningTest` exercises:

- checkpoint, repeated rename, post-rename update, journal-only flush, restart, and document verification;
- stable storage identity across rename;
- rejection of invalid size configurations;
- concurrent model name/type/validity reads while logical renames occur;
- repeatable `deinit()` after successful shutdown.

The sketch must be executed on physical ESP32, ESP32-S3, and ESP32-C3 boards before the final `v0.1.0` tag.
