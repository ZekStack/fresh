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

## Built-in backends

- `FreshLittleFSStorage` using ESP-IDF LittleFS VFS.
- `FreshSDStorage` using ESP-IDF SDSPI or SDMMC.
- `FreshEMMCStorage` using ESP-IDF SDMMC with 1-, 4-, or 8-bit bus width.
- User-defined `FreshStorage` backends.

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

## Breaking changes

Removed:

- storage factory selection through `FreshConfig`;
- borrowed storage initialization;
- `Fresh::withStorage()`;
- raw backend pointers;
- Arduino LittleFS compatibility bridge;
- deprecated filesystem aliases and migration-only compatibility code.

## Validation

The source audit rejects production includes or direct use of Arduino filesystem singleton APIs. CI compiles the library and examples for ESP32, ESP32-C3, ESP32-S3, and ESP32-P4 through Arduino CLI and PIOArduino.

Physical qualification is still required for representative SDSPI, SDMMC, and eMMC hardware, including absent media, full media, removal, power loss, and board-specific power/reset sequencing.
