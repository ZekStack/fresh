# v0.1.0 release hardening

Fresh `v0.1.0` uses manifest and snapshot payload version 4, journal format version 3, and backup archive version 2. These formats are intentionally incompatible with unsafe pre-release persisted data.

## Completeness metadata

Every durable structure is constructed with checked ArduinoJson operations and rejected if its document overflowed.

- manifests declare `modelCount`, which must exactly match the `models` array;
- snapshots declare `recordCount`, which must exactly match `docs` or `entries`;
- backup archives declare `modelCount` and a `recordCount` for every model;
- journal records are measured only after every field and payload insertion succeeds.

A newly written snapshot is read back and semantically validated for version, storage ID, type, applied sequence, and exact record count before Fresh removes its journal. An allocation failure can therefore leave the previous durable state and journal in place, but cannot commit a successful partial snapshot.

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

A reset at any persistence boundary therefore exposes either the old committed manifest or the new committed manifest. A manifest never points at storage that Fresh has not attempted to make durable first. Cleanup failures may leave unreferenced storage, but cannot remove data still referenced by the committed manifest. Storage ID generation also rejects IDs whose directories already exist, so an orphan left before manifest commit cannot be reused by a later model.

## Synchronization and user-code policy

`Fresh::_mutex` protects database lifetime state, the model registry, every mutable `FreshModel::State` field, callbacks, configuration observed by public operations, and the sync task handle.

`Fresh::_syncMutex` serializes filesystem sync and backup-import commits. Code acquires `_syncMutex` before `_mutex` when both are required. Public mutation code never acquires `_syncMutex` while holding `_mutex`.

Predicates and validators are synchronous, bounded user functions, but Fresh does not invoke them while holding the database mutex. Fresh instead clones a point-in-time snapshot, releases the mutex, evaluates user code once, and revalidates the model revision before committing. A conflicting reentrant or concurrent change is preserved and the outer operation returns `FreshStatus::Busy`; Fresh does not retry predicates or validators because they may have side effects.

Event, time, and backup callbacks are also copied under the database mutex and invoked after releasing it.

`FreshModel::name()` returns a value copy. Read APIs return a consistent cloned snapshot and do not hold the database mutex while evaluating predicates or serializing to a potentially blocking `Print` target.

## Transactional mutations and import

Patch updates reject null, scalar, and array patches. The complete merged document, validation result, document-size check, journal record, and requested return value are prepared before live state changes. `_id` and `createdAt` remain Fresh-owned metadata; `updatedAt` is assigned by Fresh.

Backup import uses prepare/commit semantics. The complete replacement registry is decoded, counted, cloned, size-checked, and assigned storage identities before commit. The commit is serialized against sync, revalidates the captured database state, invalidates old model handles, and swaps the registry using prepared containers. Any failure before the swap leaves the live database unchanged.

## Shutdown lifetime

`FreshDeinitOptions::timeoutMS` is the total explicit shutdown deadline, covering database and backup locks, waiting for sync ownership, final sync, stop notification, task exit, and cleanup checks.

If final sync fails before stop is committed, `deinit()` restores the `Running` lifecycle and preserves models, pending records, callbacks, configuration, and task ownership so the caller can correct the filesystem condition and retry.

Once stop is requested, a bounded `deinit()` may return `FreshStatus::Timeout`. The object remains alive in a stopping state, public database operations return `Busy`, and a later `deinit()` continues waiting without repeating final sync. The destructor performs an unbounded lifetime barrier.

## Allocation failures

Large and persisted ArduinoJson documents use the process-lifetime PSRAM-first allocator. Fallible serialized-byte and backup-ring allocations use a move-only `FreshBuffer` with explicit allocation results. Allocation failure is reported as `FreshStatus::OutOfMemory`.

When `FRESH_TESTING` is enabled, deterministic allocation failure can be configured by allocation number, category, minimum size, and one-shot behavior. Production builds contain no fault-injection bookkeeping.

## Persisted-size ceilings

The absolute payload ceiling for manifest slots, snapshot slots, and journal records is 1 MiB. `FreshConfig` is rejected during `init()` when:

- `maxDocumentBytes`, `maxJournalRecordBytes`, or `maxSnapshotBytes` is zero or above 1 MiB;
- `maxJournalRecordBytes` does not exceed `maxDocumentBytes`;
- `maxSnapshotBytes` is below `maxDocumentBytes`;
- `backupBufferSize` is zero or above 1 MiB;
- sync task interval, stack, priority, or core settings are invalid.

Reader and writer paths apply the same absolute ceiling and configured journal/snapshot limits. All persisted 32-bit length conversions are checked first.

## Validation

`examples/ReleaseHardeningTest` continues to exercise storage identity, rename durability, limits, concurrent handle reads, and repeated shutdown.

`examples/HardeningRegressionTest` adds:

- rejection and atomicity of non-object patches;
- reentrant predicate conflict detection;
- preservation of RAM state after a failed final sync;
- deterministic allocation-failure rollback and retry when built with `FRESH_TESTING`.

The sketches must be executed on physical ESP32, ESP32-S3, ESP32-C3, and ESP32-P4 boards before the final `v0.1.0` tag. Record each board, Arduino-ESP32 core version, filesystem partition size, PSRAM availability, and complete serial output in issue #9 or the release notes before tagging.
