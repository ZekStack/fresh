# Troubleshooting

## `init()` fails

Always print the returned status and message:

```cpp
FreshInitResult result = db.init("/fresh");
if (!result) {
    Serial.printf(
        "Fresh init failed: %s (%s)\n",
        db.statusToString(result.status),
        result.message.c_str()
    );
}
```

Common causes:

- invalid database or mount path;
- missing LittleFS partition label;
- absent or unpowered SD/eMMC media;
- incorrect pins, host slot, or bus ownership;
- formatting disabled on an unformatted medium;
- corrupt database data;
- insufficient memory for the sync task or backup buffer.

Formatting is configured on the storage backend:

```cpp
FreshLittleFSConfig storageConfig;
storageConfig.formatOnMountFailure = true;
```

Formatting can erase data. Keep it disabled unless destructive recovery is intentional.

## Arduino `LittleFS`, `SD`, or `SD_MMC` says the filesystem is not mounted

Fresh 0.2.0 uses ESP-IDF drivers directly and does not initialize Arduino filesystem singleton objects.

Use:

```cpp
db.storage().exists("/file.bin");
db.storage().open(...);
db.storage().writeFile(...);
```

Do not use an Arduino filesystem singleton for the same Fresh-managed volume.

## `deinit()` returns `FreshStatus::Busy`

An application `FreshFile` is still open.

```cpp
FreshResult result = file.syncAndClose();
FreshResult stopped = db.deinit();
```

Capacity diagnostics include application and internal open-file counts:

```cpp
FreshStorageInfo info = db.storage().info();
Serial.printf("application files: %u\n", info.applicationOpenFileCount);
```

## Storage access is rejected below the database root

Fresh protects its journal, snapshot, and manifest files. When initialized at `/fresh`, calls such as this fail:

```cpp
db.storage().removeFile("/fresh/manifest.a.msgpack");
```

Use sibling application paths:

```text
/backups
/uploads
/configuration
```

## Data is missing after power loss

Fresh is RAM-first. A successful mutation can still be pending in RAM.

- Lower `syncIntervalMS` to reduce the normal loss window.
- Call `flush()` when captured journal operations must be durable before continuing.
- Call `forceSync()` when a forced checkpoint is also required.
- Check every persistence result.

## Storage usage does not change immediately

Background sync is interval-based and dirty-only. Check after a sync or call:

```cpp
db.forceSync();
FreshStorageInfo info = db.storage().info();
```

## Writes fail with limits

- `FreshStatus::SizeLimitExceeded`: a serialized document, journal record, or snapshot exceeded its configured bound.
- `FreshStatus::StorageFull`: the next persistence operation would violate `minFreeBytes` or reported free space is insufficient.
- `FreshStatus::Busy`: the backend's configured open-file limit was reached or shutdown is in progress.

Inspect storage with the result-aware API:

```cpp
FreshStorageInfo info;
FreshResult queried = db.storage().info(info);
```

## SD or eMMC mount fails

Verify:

- media power and reset sequencing completed before `db.init()`;
- the selected SDMMC slot is correct;
- all required pins are configured for the selected bus width;
- pull-ups required by the board are present;
- SDSPI managed/external ownership matches actual bus initialization;
- no other component already owns the mount point or card device;
- `formatOnMountFailure` is not hiding a wiring or power problem.

Fresh does not perform board-specific GPIO, LDO, or regulator setup automatically.

## Card removal

Automatic hot-swap recovery is not implemented. Removing media while Fresh is running can make current and subsequent operations fail. Close files and perform a controlled shutdown or restart before remounting.

## Backup stalls

After `startBackup()`, continue draining `readBackup()` until completion or cancel it. The producer uses a bounded buffer; an undrained backup can occupy the sync task.

```cpp
FreshBackupStatus status = db.backupStatus();
```

Use `status.state` for lifecycle control and `status.result` for diagnostics.

## Compile errors about the language standard

Fresh requires C++20:

```ini
build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```
