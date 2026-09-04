# Fresh 0.2.0 release notes

Fresh 0.2.0 introduces owned, pluggable storage, a unified application-file API, and Strata-backed memory/FreeRTOS ownership.

## Strata integration

- Fresh now depends on Strata v0.1.2.
- `FreshConfig::memory` exposes allocation and sync-task stack placement policy.
- Fresh-owned placement-sensitive allocations route through Strata while the existing `FRESH_TESTING` allocation categories remain available for deterministic fault injection.
- Fresh-owned recursive mutex control blocks, the sync-task exit semaphore, and the synchronization task use Strata FreeRTOS wrappers.
- Sync-task diagnostics expose requested placement, effective placement, observed memory region, stack high-water mark, and storage-imposed constraints.
- The backup buffer exposes requested placement and observed region diagnostics.

### Storage-aware sync-task stacks

`FreshConfig::memory.taskStack` is a requested placement rather than an unconditional command. Storage backends can declare `FreshTaskStackRequirement::Internal` when their I/O path requires an internal task stack.

`FreshLittleFSStorage` declares this requirement, so LittleFS synchronization always runs with an internal stack even when `PreferExternal` or `RequireExternal` was requested. This protects internal-flash operations from depending on a PSRAM-backed stack while flash access is active.

`FreshSDStorage` and `FreshEMMCStorage` are unconstrained. Applications using those backends can place the sync-task stack in PSRAM, for example with `Strata::Placement::PreferExternal`, and recover the corresponding internal RAM when external memory is available.

Custom storage defaults to unconstrained and may override `syncTaskStackRequirement()` when necessary.

## Storage architecture

- `FreshConfig` now configures database behavior and memory policy.
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
- Optional Fresh-owned ESP-IDF on-chip LDO power control for SDMMC.
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

Added:

- Strata v0.1.2 dependency;
- `FreshConfig::memory`;
- `FreshTaskStackRequirement` for storage backends;
- memory and sync-task placement diagnostics.

## Validation

The source audit rejects direct Fresh production use of ESP-IDF heap-placement primitives and dynamically allocated Fresh-owned task/semaphore creation, in addition to Arduino filesystem singleton APIs. CI compiles the library and examples for ESP32, ESP32-C3, ESP32-S3, and ESP32-P4 through Arduino CLI and PIOArduino, and compiles the hardening fault-injection suite with Strata present.

Physical qualification is still required for representative SDSPI, SDMMC, and eMMC hardware, including formatting, absent media, full media, removal, power loss, and board-specific power/reset sequencing.
