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

## Sync interval

`syncIntervalMS` controls how often the background task checks for dirty state. Public writes are accepted into RAM first, and the task writes dirty models to LittleFS later.

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
FreshModel users = db.createModel("User", FreshModelType::General);
FreshModel logs = db.createModel("Log", FreshModelType::Stream);
```

## Snapshot thresholds

Fresh stores changes as journal records and writes snapshots when compaction thresholds are reached.

Lower thresholds compact more often and may reduce startup replay work. Higher thresholds compact less often and may reduce snapshot writes.

## Backup buffer

`backupBufferSize` controls the internal buffer used while backup data is generated and read in chunks.

Applications still choose their own read chunk size:

```cpp
uint8_t buffer[256];
size_t read = db.readBackup(buffer, sizeof(buffer), 50);
```
