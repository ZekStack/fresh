# Fresh 0.2.0 release hardening

Fresh 0.2.0 preserves one manifest, snapshot, journal, and backup format across supported storage backends. The storage redesign changes source APIs and lifecycle ownership, not the durable database model.

## Completeness metadata

Every durable structure is constructed with checked ArduinoJson operations and rejected if its document overflowed.

- manifests declare `modelCount`, which must exactly match the `models` array;
- snapshots declare `recordCount`, which must exactly match `docs` or `entries`;
- backup archives declare `modelCount` and a `recordCount` for every model;
- journal records are measured only after every field and payload insertion succeeds.

A newly written snapshot is read back and semantically validated before its journal is removed. Allocation or storage failure can leave the previous durable state and journal in place, but cannot commit a successful partial snapshot.

## Immutable model storage IDs

Each manifest entry contains a logical name, immutable storage ID, and model type. Model snapshots and journals live under:

```text
<database-root>/models/<storage-id>/
```

Renaming changes only the logical manifest name. It does not rename model directories.

The commit order remains:

1. Persist active model journals and snapshots.
2. Commit a durable manifest slot.
3. Remove storage no longer referenced by the committed manifest.

A reset at a persistence boundary therefore exposes the previous or new committed manifest. Cleanup failures may leave orphaned storage but cannot delete storage still referenced by the committed manifest.

## Storage ownership and access

Fresh owns the configured `FreshStorage` backend from successful `init()` until `deinit()` or destruction. It is the only component allowed to mount or unmount that backend.

Application file operations use `db.storage()`. The facade:

- holds a backend-owned recursive access state while entering storage operations;
- becomes detached before the backend can be destroyed;
- refuses new operations while shutdown is active;
- rejects the configured database root and all descendants;
- returns `FreshFile` handles that participate in open-file accounting;
- exposes no mount or unmount operation.

`FreshFile` owns a mutex-protected file state. Reads, writes, seek, size, sync, close, and error queries are serialized per handle. Storage file counters distinguish internal persistence handles from application handles.

`deinit()` returns `FreshStatus::Busy` before stopping the sync task when an application file remains open. The destructor force-closes application files before its unbounded lifetime barrier so a surviving handle fails closed.

## Synchronization and user-code policy

`Fresh::_mutex` protects database lifecycle state, models, callbacks, configuration, and storage-facade acquisition. Each backend access state serializes filesystem operations and gates teardown; it is recursive because facade entry calls the same guarded base storage operations used by internal persistence.

`Fresh::_syncMutex` serializes persistence and backup-import commits. Code acquires `_syncMutex` before `_mutex` when both are required. Public mutation code does not acquire `_syncMutex` while holding `_mutex`.

Predicates and validators are invoked outside the database mutex. Fresh clones a point-in-time view, evaluates user code once, and validates the model revision before commit. A concurrent or reentrant change is preserved and the outer operation returns `FreshStatus::Busy`.

Callbacks are copied under the database mutex and invoked after release. Blocking lifecycle, persistence, backup, or storage work must be scheduled on another task.

## Storage driver boundary

Production Fresh sources use ESP-IDF storage and VFS APIs. The source audit rejects direct includes or use of Arduino filesystem singleton APIs:

```text
LittleFS
SD
SD_MMC
```

Board-level power, regulator, reset, voltage selection, and external bus coordination remain application responsibilities and must be complete before initialization.

## Transactional mutations and import

Patch updates reject non-object patches. The merged document, validation result, size checks, journal record, and requested return value are prepared before live state changes.

Backup restore uses prepare/commit semantics. The replacement registry is decoded, counted, cloned, size-checked, and assigned storage identities before commit. Model creation, sync repair, and restore all use the same bounded storage-ID allocator, which propagates existence-query failures instead of treating them as collisions or absence. The commit is serialized against sync, revalidates captured state, invalidates old handles, and swaps prepared containers. Failure before the swap leaves the live database unchanged.

Replacement rename does not delete an existing target before the backend rename succeeds. Identical source and target paths are a no-op, and failed replacement preserves the old target. Complete-file helpers reject undersized buffers with the required capacity instead of reporting truncated success.

## Shutdown lifetime

`FreshDeinitOptions::timeoutMS` is the complete explicit shutdown deadline, covering locks, final sync, stop notification, task exit, storage checks, and unmount.

When final sync fails before stop is committed, Fresh returns to `Running`, re-enables application file acceptance, and preserves pending state so the caller can correct the storage problem and retry.

After stop is requested, a bounded call may return `FreshStatus::Timeout`. The object remains in a stopping state and a later `deinit()` continues waiting. The destructor uses an unbounded task-lifetime barrier.

## Allocation failures

Large persisted ArduinoJson documents use the process-lifetime PSRAM-first allocator. Serialized buffers and backup-ring allocations use move-only explicit-result buffers. Allocation failures return `FreshStatus::OutOfMemory`.

`FRESH_TESTING` supports deterministic allocation failure by allocation number, category, minimum size, and one-shot behavior. Production builds contain no fault-injection bookkeeping.

## Persisted-size ceilings

The absolute payload ceiling for manifest slots, snapshots, and journal records is 1 MiB. Configuration is rejected when:

- document, journal, or snapshot limits are zero or above the ceiling;
- journal limit does not exceed document limit;
- snapshot limit is below document limit;
- backup buffer is zero or above the ceiling;
- sync task interval, stack, priority, or core are invalid.

Reader and writer paths apply the same configured and absolute bounds. All persisted 32-bit length conversions are checked before conversion.

## Automated validation

CI validates:

- production source boundaries;
- metadata and Arduino lint;
- Arduino CLI builds on ESP32, ESP32-C3, ESP32-S3, and ESP32-P4;
- PIOArduino builds for all examples on the same targets;
- hardening build with deterministic allocation failure;
- custom storage, file lifecycle, and storage failure regression sketches.

## Physical validation

Before tagging 0.2.0, execute runtime qualification for:

- LittleFS two-boot persistence;
- managed and external SDSPI;
- SDMMC on representative one-bit/four-bit boards;
- Waveshare ESP32-P4-Module-DEV-KIT onboard TF card;
- four-bit/eight-bit eMMC;
- absent, full, removed, and write-protected media;
- power loss during journal, snapshot, manifest, backup, and application-file operations.
