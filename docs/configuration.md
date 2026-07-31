# Configuration

Fresh 0.2.0 separates database configuration from storage configuration.

```cpp
FreshConfig config;
config.syncIntervalMS = 5000;
config.syncTaskPriority = 1;
config.syncTaskCore = tskNO_AFFINITY;
config.syncTaskStackSize = 8192;
config.snapshotRecordThreshold = 128;
config.snapshotBytesThreshold = 32 * 1024;
config.backupBufferSize = 8 * 1024;
config.minFreeBytes = 4096;
config.maxDocumentBytes = 16 * 1024;
config.maxJournalRecordBytes = 32 * 1024;
config.maxSnapshotBytes = 256 * 1024;
```

Storage selection is not part of `FreshConfig`. Construct the required backend separately:

```cpp
FreshLittleFSConfig storageConfig;
storageConfig.maxOpenFiles = 12;

FreshInitResult result = db.init(
    "/fresh",
    config,
    FreshLittleFSStorage(storageConfig)
);
```

## Database options

| Option | Default | Meaning |
| --- | ---: | --- |
| `syncIntervalMS` | `5000` | Background persistence interval. |
| `syncTaskPriority` | `1` | FreeRTOS sync task priority. |
| `syncTaskCore` | `tskNO_AFFINITY` | Core affinity for the sync task. |
| `syncTaskStackSize` | `8192` | Sync task stack size. |
| `compressionType` | `MessagePack` | Persisted payload encoding. |
| `defaultModelType` | `General` | Type used by `createModel(name)`. |
| `snapshotRecordThreshold` | `128` | Pending journal records that trigger a checkpoint. |
| `snapshotBytesThreshold` | `32 KiB` | Journal bytes that trigger a checkpoint. |
| `backupBufferSize` | `8 KiB` | Streaming backup ring-buffer size. |
| `minFreeBytes` | `4096` | Free-space reserve kept after persistence writes. |
| `maxDocumentBytes` | `16 KiB` | Maximum serialized document payload. |
| `maxJournalRecordBytes` | `32 KiB` | Maximum serialized journal record. |
| `maxSnapshotBytes` | `256 KiB` | Maximum serialized model snapshot. |

`FreshConfig` deliberately contains no filesystem type, partition label, mount path, SD pins, bus ownership, or formatting policy.

## Storage options

Each backend owns its own configuration type:

| Backend | Configuration |
| --- | --- |
| `FreshLittleFSStorage` | `FreshLittleFSConfig` |
| `FreshSDStorage` with SPI | `FreshSDConfig` + `FreshSDSPIConfig` |
| `FreshSDStorage` with SDMMC | `FreshSDConfig` + `FreshSDMMCConfig` |
| `FreshEMMCStorage` | `FreshEMMCConfig` |
| Custom backend | User-defined constructor parameters |

Formatting is disabled by default. Production applications should normally keep `formatOnMountFailure = false` so a wiring, power, or media failure cannot erase data automatically.

## Initialization order

The public explicit overload is:

```cpp
FreshInitResult init(
    const char* databasePath,
    const FreshConfig& config,
    Storage&& storage
);
```

The backend must be passed as an rvalue:

```cpp
FreshLittleFSStorage storage(storageConfig);
FreshInitResult result = db.init(
    "/fresh",
    config,
    std::move(storage)
);
```

This transfers backend ownership to Fresh. The original moved-from object must not be used.

The convenience overload:

```cpp
db.init("/fresh", config);
```

constructs a default `FreshLittleFSStorage` internally.
