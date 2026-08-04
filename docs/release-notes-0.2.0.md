# Fresh 0.2.0 release notes

Fresh 0.2.0 introduces owned, pluggable storage and a unified application-file API.

## Storage architecture

- `FreshConfig` now configures database behavior only.
- Built-in backends are passed as objects to `init()`.
- Fresh owns the backend for the complete database lifecycle.
- Default initialization still creates a default LittleFS backend.
- Application files use `db.storage()`.
- Database-root paths are protected from application operations.
- Open application files block `deinit()` with `FreshStatus::Busy`.
- `FreshFile` remains move-only and serializes file operations with a mutex.
- `Fresh::format()` destructively formats the complete configured storage volume and restarts an empty database.

## Built-in backends

- `FreshLittleFSStorage` using ESP-IDF LittleFS VFS.
- `FreshSDStorage` using ESP-IDF SDSPI or SDMMC.
- `FreshEMMCStorage` using ESP-IDF SDMMC with 1-, 4-, or 8-bit bus width.
- User-defined `FreshStorage` backends.

LittleFS, SDSPI, SDMMC, and eMMC support whole-volume formatting. Custom backends can opt in by overriding `supportsFormat()` and `formatBackend()`.

Fresh does not include or synchronize Arduino's `LittleFS`, `SD`, or `SD_MMC` singleton objects.

## Application storage API

`db.storage()` supports:

- file open/read/write/sync/close;
- complete-file read and write helpers;
- existence and file-size queries;
- recursive directory creation;
- directory listing;
- file/directory removal;
- rename with optional replacement;
- capacity and open-file diagnostics.

## Destructive formatting

`database.format()` removes the Fresh database, application files, and unrelated files stored on the same configured filesystem. Fresh stops synchronization, closes tracked files, invalidates existing file and model handles, formats the backend, writes an empty durable manifest, and restarts the same database instance.

Formatting is not equivalent to `dropAllModels()` and is not a secure erase guarantee. Failures after native formatting begins leave Fresh fail-closed.

## Breaking changes

Removed:

- storage factory selection through `FreshConfig`;
- borrowed storage initialization;
- `Fresh::withStorage()`;
- raw backend pointers;
- Arduino LittleFS compatibility bridge;
- deprecated filesystem aliases and migration-only compatibility code.

## Validation

The source audit rejects production includes or direct use of Arduino filesystem singleton APIs. CI compiles the library and examples for ESP32, ESP32-C3, ESP32-S3, and ESP32-P4 through Arduino CLI and PIOArduino. A custom in-memory regression covers supported and unsupported formatting, stale handle invalidation, persistence after restart, repeated formatting, and native formatter failure.

Physical qualification is still required for representative SDSPI, SDMMC, and eMMC hardware, including formatting, absent media, full media, removal, power loss, and board-specific power/reset sequencing.
