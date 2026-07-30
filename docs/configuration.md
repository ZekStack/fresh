# Configuration

`FreshConfig` controls storage selection, persistence timing, sync task settings, model defaults, snapshot thresholds, backup buffering, and persisted-size limits.

```cpp
FreshConfig config;

config.storageType = FreshStorageType::LittleFS;
config.littleFS.partitionLabel = "spiffs";
config.littleFS.mountPath = "/littlefs";
config.littleFS.maxOpenFiles = 10;
config.littleFS.formatOnMountFailure = false;
config.littleFS.growOnMount = true;

config.syncIntervalMS = 5000;
config.syncTaskPriority = 1;
config.syncTaskCore = tskNO_AFFINITY;
config.syncTaskStackSize = 8192;
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
| `storageType` | `FreshStorageType::LittleFS` | Built-in managed storage selected by the normal `init(path, config)` overload. |
| `littleFS.partitionLabel` | `"spiffs"` | ESP partition label used for LittleFS. |
| `littleFS.mountPath` | `"/littlefs"` | ESP-IDF VFS mount point for LittleFS. |
| `littleFS.maxOpenFiles` | `10` | Maximum concurrent files opened through this Fresh storage instance. |
| `littleFS.formatOnMountFailure` | `false` | Whether Fresh may format LittleFS after a mount failure. |
| `littleFS.growOnMount` | `true` | Allow LittleFS to grow to the partition boundary when supported. |
| `sd.interface` | `FreshSDInterface::SPI` | Select SDSPI or SDMMC. |
| `sd.mountPath` | `"/fresh-sd"` | ESP-IDF FATFS VFS mount point. |
| `sd.maxOpenFiles` | `8` | Maximum concurrent files opened through this Fresh storage instance and FATFS mount. |
| `sd.allocationUnitSize` | `16 * 1024` | FAT allocation unit requested if explicit formatting is enabled. |
| `sd.formatOnMountFailure` | `false` | Whether Fresh may format the SD filesystem after a mount failure. |
| `sd.spi.host` | `SPI2_HOST` | SPI host used by SDSPI. |
| `sd.spi.frequencyHz` | `20'000'000` | Maximum SDSPI clock frequency. |
| `sd.spi.busOwnership` | `FreshSPIBusOwnership::Managed` | Whether Fresh initializes and frees the SPI host. |
| `sd.sdmmc.slot` | `0` | SDMMC host slot. |
| `sd.sdmmc.oneBitMode` | `false` | Use four-bit SDMMC unless one-bit mode is explicitly selected. |
| `syncIntervalMS` | `5000` | Background sync interval in milliseconds. |
| `syncTaskPriority` | `1` | FreeRTOS priority for the sync task. |
| `syncTaskCore` | `tskNO_AFFINITY` | ESP32 core selection for the sync task. |
| `syncTaskStackSize` | `8192` | Sync task stack size in bytes. |
| `compressionType` | `FreshCompressionType::MessagePack` | Persistence format. MessagePack is the only current option. |
| `defaultModelType` | `FreshModelType::General` | Model type used by `createModel(name)`. |
| `snapshotRecordThreshold` | `128` | Journal record count threshold before snapshot compaction. |
| `snapshotBytesThreshold` | `32 * 1024` | Journal byte threshold before snapshot compaction. |
| `backupBufferSize` | `8 * 1024` | Internal backup buffer size in bytes. |
| `minFreeBytes` | `4096` | Storage reserve Fresh leaves unused during sync preflight. |
| `maxDocumentBytes` | `16 * 1024` | Maximum serialized MessagePack size for stored documents and stream entries. |
| `maxJournalRecordBytes` | `32 * 1024` | Maximum serialized journal payload size, excluding the fixed journal header. |
| `maxSnapshotBytes` | `256 * 1024` | Maximum serialized snapshot payload size, excluding the durable slot header. |

`eraseOnFileSystemFailure` remains temporarily available as a deprecated compatibility alias for `littleFS.formatOnMountFailure`.

## Storage selection

### LittleFS

```cpp
FreshConfig config;
config.storageType = FreshStorageType::LittleFS;
config.littleFS.partitionLabel = "spiffs";

FreshResult result = db.init("/fresh", config);
```

Fresh mounts and owns this partition for the complete database lifecycle.

### SDSPI

```cpp
FreshConfig config;
config.storageType = FreshStorageType::SD;
config.sd.interface = FreshSDInterface::SPI;
config.sd.spi.chipSelectPin = GPIO_NUM_10;
config.sd.spi.clockPin = GPIO_NUM_12;
config.sd.spi.mosiPin = GPIO_NUM_11;
config.sd.spi.misoPin = GPIO_NUM_13;
config.sd.spi.busOwnership = FreshSPIBusOwnership::Managed;

FreshResult result = db.init("/fresh", config);
```

Managed SDSPI requires chip-select, clock, MOSI, and MISO pins. With `FreshSPIBusOwnership::External`, only chip-select is required by Fresh; the application must initialize and preserve the selected SPI host.

### SDMMC

```cpp
FreshConfig config;
config.storageType = FreshStorageType::SD;
config.sd.interface = FreshSDInterface::SDMMC;
config.sd.sdmmc.slot = 0;
config.sd.sdmmc.oneBitMode = false;

FreshResult result = db.init("/fresh", config);
```

Unset pins use the target's default SDMMC routing. Custom pin configuration is accepted only on targets whose SDMMC host supports GPIO-matrix routing.

### Custom storage

Custom storage is supplied through a dedicated initialization overload, not created from `FreshConfig` alone.

Fresh-owned custom backend:

```cpp
std::unique_ptr<FreshStorage> storage = createCustomStorage();
FreshResult result = db.init("/fresh", std::move(storage), config);
```

Caller-owned custom backend:

```cpp
CustomStorage storage;
storage.attachApplicationStorage();
FreshResult result = db.init("/fresh", storage, config);
```

The caller-owned backend must already be mounted and must outlive the `Fresh` instance. See [`storage.md`](storage.md) for the complete custom backend contract.

## Sync interval

`syncIntervalMS` controls how often the background task checks for dirty state. Public writes are accepted into RAM first, and the task writes dirty models to the selected backend later. Sync captures a batch under a short database lock, then performs storage operations without holding the global database mutex. Normal background sync compacts snapshots only when thresholds are reached or a snapshot is explicitly required.

Shorter intervals reduce the window of data loss after power failure but increase storage activity. Longer intervals reduce background work but leave more accepted RAM state waiting for persistence.

## Sync task settings

Fresh uses a FreeRTOS task on ESP32.

`syncTaskStackSize` is a byte count. Increase it if backup, snapshot, restore, or large model sync work exhausts stack on the target board.

`syncTaskCore` can pin the task to a core, or remain `tskNO_AFFINITY` to let FreeRTOS choose.

## Formatting and recovery

Formatting is always opt-in.

- `littleFS.formatOnMountFailure` applies only to the selected LittleFS partition.
- `sd.formatOnMountFailure` applies only to the selected FAT filesystem.
- Custom backend formatting behavior is backend-defined.

Keep formatting disabled unless the product can safely erase the selected medium after a mount failure.

Fresh never automatically falls back to another filesystem because that could split one logical database across multiple storage devices.

## Persistence format

`compressionType` currently supports only `FreshCompressionType::MessagePack`.

Storage transport does not change the persisted database or backup format. LittleFS, SD, and custom backends use the same journal, snapshot, manifest, and backup encodings.

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

`forceSync()` and `forceSyncAsync()` bypass these thresholds for dirty models captured by that sync, forcing a checkpoint snapshot after pending journal records are written. Clean models are not snapshotted only because a forced sync was requested.

`flush()` does not bypass thresholds. It blocks until captured pending operations are durable in their journals while allowing normal threshold-based compaction when already due.

## Storage limits

Fresh checks document, journal record, snapshot, backend free-space, and concurrent file-handle limits.

`littleFS.maxOpenFiles` and `sd.maxOpenFiles` include files opened internally for journals, snapshots, manifests, restore staging, and backups as well as application files opened through `withStorage()`. When the limit is reached, another open returns `FreshStatus::Busy`. Closing a file immediately releases its slot.

`maxDocumentBytes` is measured after Fresh applies stored metadata such as `_id`, `createdAt`, and `updatedAt`. `maxJournalRecordBytes` applies to the serialized journal payload. `maxSnapshotBytes` applies to the serialized snapshot payload. Fresh accounts for fixed journal and durable-slot headers separately during free-space preflight.

`minFreeBytes` prevents Fresh from intentionally filling the selected backend. A sync fails with `FreshStatus::StorageFull` when the required bytes plus this reserve exceed reported free space.

Capacity lookup is result-aware. A backend query failure is returned as a storage or filesystem error instead of being interpreted as zero free space. A failed existence probe used while allocating model storage also aborts the current sync rather than being treated as an indefinitely occupied identifier.

Manifest, snapshot, and journal payloads have an absolute 1 MiB ceiling. The configured limits must be nonzero and no greater than this ceiling; the journal limit must exceed the document limit, and the snapshot limit must be at least the document limit. `backupBufferSize` must be between 1 byte and 1 MiB.

## Backup buffer

`backupBufferSize` controls the internal buffer used while backup data is generated and read in chunks.

Backup generation runs on the sync task. After `startBackup()`, the application must keep calling `readBackup()` until `backupStatus().state` is `FreshBackupState::Finished`, `FreshBackupState::Cancelled`, or `FreshBackupState::Error`, or call `cancelBackup()` if the consumer stops. If the buffer fills because the application does not drain it, normal persistence can stop progressing until space is available.

Applications choose their own read chunk size:

```cpp
uint8_t buffer[256];
size_t read = db.readBackup(buffer, sizeof(buffer), 50);
```

Use `FreshBackupStatus.state` for lifecycle control and `FreshBackupStatus.result` for detailed success or failure diagnostics.
