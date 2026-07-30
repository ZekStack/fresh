# Fresh 0.2.0 Release Notes

Fresh 0.2.0 replaces the LittleFS-specific persistence boundary with a pluggable storage architecture while preserving the RAM-first document database API and existing storage formats.

## Added

- Managed LittleFS through ESP-IDF VFS
- Managed SD storage over SPI
- Managed SD storage over SDMMC on supported targets
- Managed and externally owned SPI bus modes
- Fresh-owned custom storage initialization
- Caller-owned custom storage attachment
- Non-VFS custom file backends
- Move-only `FreshFile` with durable `sync()`, `close()`, and `syncAndClose()`
- Lifecycle-safe `withStorage()` application file access
- Result-aware storage information queries
- Backend identity, state, native error, capacity, and open-handle diagnostics
- Database-root protection for application storage operations
- Same-filesystem backup archive support
- LittleFS, SDSPI, SDMMC, custom storage, lifecycle, and failure regression examples

## Changed

- Journals, snapshots, manifest slots, restore staging, garbage collection, and storage preflight now operate through the selected `FreshStorage` backend.
- Binary persistence helpers use Arduino `Print` and `Stream` rather than the Arduino filesystem `File` type.
- Built-in filesystems mount directly through ESP-IDF APIs.
- Durable writes reject short writes and explicit write errors, require successful synchronization and close, and retain existing read-back verification.
- `deinit()` rejects open application files before stopping the sync task.
- The raw `storage()` accessor is deprecated in favor of `withStorage()`.
- Storage formatting remains opt-in and SD formatting remains disabled by default.

## Compatibility

- `database.init("/fresh")` remains source-compatible and selects managed LittleFS.
- Existing journal, snapshot, manifest, and backup formats are preserved.
- `FreshConfig::eraseOnFileSystemFailure` remains temporarily available as a deprecated LittleFS compatibility alias.
- Automatic filesystem migration is not included.

## Validation

The repository CI matrix compiles every example with Arduino CLI and PIOArduino for:

- ESP32
- ESP32-C3
- ESP32-S3
- ESP32-P4

The matrix also runs source-boundary auditing, Arduino lint, and the `FRESH_TESTING` hardening build.

Physical validation of managed/external SDSPI wiring, SDMMC wiring, missing/full cards, card removal, and target-specific power-loss behavior must be completed on representative hardware before declaring the storage transports production-qualified.

## Migration

See [`migration-0.2.0.md`](migration-0.2.0.md) for configuration, lifecycle, custom backend, and manual filesystem migration guidance.
