# Configuration

`FreshConfig` controls persistence timing, sync task settings, storage behavior, model defaults, snapshot thresholds, and backup buffering.

```cpp
FreshConfig config;
config.syncIntervalMS = 5000;
config.syncTaskPriority = 1;
config.syncTaskCore = tskNO_AFFINITY;
config.syncTaskStackSize = 8192;
config.eraseOnFileSystemFailure = false;
config.compressionType = FreshCompressionType::MessagePack;
config.defaultModelType = FreshModelType::General;
config.snapshotRecordThreshold = 128;
config.snapshotBytesThreshold = 32 * 1024;
config.backupBufferSize = 8 * 1024;
config.minFreeBytes = 4096;
config.maxDocumentBytes = 16 * 1024;
config.maxJournalRecordBytes = 32 * 1024;
config.maxSnapshotBytes = 256 * 1024;

FreshResult result = db.init("/fresh_app", config);
```

## Defaults

| Option | Default | Meaning |
| --- | --- | --- |
| `syncIntervalMS` | `5000` | Background sync interval in milliseconds. |
| `syncTaskPriority` | `1` | FreeRTOS priority for the sync task. |
| `syncTaskCore` | `tskNO_AFFINITY` | ESP32 core selection for the sync task. |
| `syncTaskStackSize` | `8192` | Sync task stack size in bytes. |
| `eraseOnFileSystemFailure` | `false` | Whether Fresh may erase LittleFS when mounting fails. |
| `compressionType` | `FreshCompressionType::MessagePack` | Persistence format. MessagePack is the only current option. |
| `defaultModelType` | `FreshModelType::General` | Model type used by `createModel(name)`. |
| `snapshotRecordThreshold` | `128` | Journal record count threshold before snapshot compaction. |
| `snapshotBytesThreshold` | `32 * 1024` | Journal byte threshold before snapshot compaction. |
| `backupBufferSize` | `8 * 1024` | Internal backup buffer size in bytes. |
| `minFreeBytes` | `4096` | LittleFS free-space reserve Fresh leaves unused during sync preflight. |
| `maxDocumentBytes` | `16 * 1024` | Maximum serialized MessagePack size for stored documents and stream entries. |
| `maxJournalRecordBytes` | `32 * 1024` | Maximum serialized journal payload size, excluding the fixed journal header. |
| `maxSnapshotBytes` | `256 * 1024` | Maximum serialized snapshot payload size, excluding the durable slot header. |

## Sync interval

`syncIntervalMS` controls how often the background task checks for dirty state. Public writes are accepted into RAM first, and the task writes dirty models to LittleFS later. Sync captures a batch under a short database lock, then performs LittleFS writes without holding the global database mutex. Normal background sync compacts snapshots only when thresholds are reached or a snapshot is explicitly required.

Shorter intervals reduce the window of data loss after power failure but can increase flash activity. Longer intervals reduce background work but leave more accepted RAM state waiting for persistence.

## Sync task settings

Fresh uses an ESP-IDF style FreeRTOS task on ESP32.

`syncTaskStackSize` is a byte count. Increase it if backup, snapshot, or large model sync work exhausts stack on your target board.

`syncTaskCore` can pin the task to a core, or remain `tskNO_AFFINITY` to let FreeRTOS choose.

## Filesystem recovery

`eraseOnFileSystemFailure` defaults to `false`. Keep it disabled unless your product can safely erase the entire LittleFS partition when mounting fails.

When enabled, Fresh may erase LittleFS during initialization recovery. That can make a device bootable again, but stored database data can be lost.

## Persistence format

`compressionType` currently supports only `FreshCompressionType::MessagePack`.

Do not treat the current storage or backup format as a stable external compatibility contract while Fresh is early-stage `0.0.1`.

## Model defaults

`defaultModelType` controls calls to `createModel(name)`.

```cpp
FreshConfig config;
config.defaultModelType = FreshModelType::Stream;
```

Prefer passing the type explicitly at the call site when mixed model types are used:

```cpp
FreshModelResult usersResult = db.createModel("User", FreshModelType::General);
FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
```

## Snapshot thresholds

Fresh stores changes as journal records and writes snapshots when compaction thresholds are reached.

Lower thresholds compact more often and may reduce startup replay work. Higher thresholds compact less often and may reduce snapshot writes.

`forceSync()` and `forceSyncAsync()` bypass these thresholds for dirty models captured by that sync, forcing a checkpoint snapshot after pending journal records are written. Clean models are not snapshotted just because a forced sync was requested.

## Storage limits

Fresh checks document, journal record, snapshot, and LittleFS free-space limits before accepting large writes or starting sync writes.

`maxDocumentBytes` is measured after Fresh applies stored metadata such as `_id`, `createdAt`, and `updatedAt`. `maxJournalRecordBytes` applies to the serialized journal payload only. `maxSnapshotBytes` applies to the serialized snapshot payload only. Fresh accounts for fixed journal and durable-slot headers separately during free-space preflight.

`minFreeBytes` prevents Fresh from intentionally filling LittleFS to the end of the partition. A sync fails with `FreshStatus::StorageFull` when the bytes Fresh needs to write now plus this reserve exceed reported free space.

The defaults are strict embedded defaults. Raise them only when the LittleFS partition and RAM budget can support larger payloads.

## Backup buffer

`backupBufferSize` controls the internal buffer used while backup data is generated and read in chunks.

Backup generation runs on the sync task. After `startBackup()`, the application must keep calling `readBackup()` until `backupStatus().state` is `FreshBackupState::Finished`, `FreshBackupState::Cancelled`, or `FreshBackupState::Error`, or call `cancelBackup()` if the consumer stops. If the buffer fills because the application does not drain it, normal persistence can stop progressing until space is available.

Applications still choose their own read chunk size:

```cpp
uint8_t buffer[256];
size_t read = db.readBackup(buffer, sizeof(buffer), 50);
```

Use `FreshBackupStatus.state` for lifecycle control and `FreshBackupStatus.result` for detailed success/failure diagnostics.
