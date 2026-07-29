#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: str, old: str, new: str) -> None:
    file_path = ROOT / path
    text = file_path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, found {count}: {old!r}")
    file_path.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "README.md",
    "Fresh is a RAM-first document database for ESP32 with async LittleFS persistence.",
    "Fresh is a RAM-first document database for ESP32 with pluggable asynchronous storage.",
)
replace_once(
    "README.md",
    "* **ESP32-friendly storage** - dirty-only LittleFS sync reduces unnecessary flash work.",
    "* **Pluggable storage** - use Fresh-managed LittleFS, SDSPI, SDMMC, or a custom backend without changing database semantics.",
)
replace_once(
    "README.md",
    "* Sync captures dirty RAM state under a short database lock, then performs LittleFS writes without holding the global database mutex.",
    "* Sync captures dirty RAM state under a short database lock, then writes through the selected storage backend without holding the global database mutex.",
)
replace_once(
    "README.md",
    "* Fresh enforces configurable document, journal, snapshot, and LittleFS reserve limits. Oversized payloads return `FreshStatus::SizeLimitExceeded`; sync preflight space failures return `FreshStatus::StorageFull`.",
    "* Fresh enforces configurable document, journal, snapshot, and backend reserve limits. Oversized payloads return `FreshStatus::SizeLimitExceeded`; sync preflight space failures return `FreshStatus::StorageFull`.",
)
replace_once(
    "README.md",
    "* Fresh `0.1.0` uses manifest/snapshot payload v3 and journal v3. Manifest entries map logical names to immutable storage IDs, so rename never moves model directories. The release intentionally rejects earlier pre-release storage formats; erase the development database when upgrading.",
    "* Fresh `0.2.0` keeps one journal, snapshot, manifest, and backup format across LittleFS, SD, and custom backends. Manifest entries map logical names to immutable storage IDs, so rename never moves model directories.",
)
replace_once(
    "README.md",
    '''| `ModelManagement` | Create, rename, drop, drop selected, and drop all models. |
| `SelfTest` | Destructive Fresh development self-test for persistence, recovery, backup, and shutdown behavior. It uses `/fresh_selftest`, `/fresh_selftest_src`, and `/fresh_selftest_dst`, touches internal storage files, and should only be run on a test device or test partition. |
''',
    '''| `ModelManagement` | Create, rename, drop, drop selected, and drop all models. |
| `SDSPIStorage` | Configure Fresh-managed SD storage over SPI. |
| `SDMMCStorage` | Configure Fresh-managed SDMMC storage. |
| `SameFilesystemBackup` | Store a backup archive beside the database on the active backend. |
| `CustomStorage` | Implement and reload data through a caller-owned in-memory custom backend. |
| `SelfTest` | Destructive Fresh development self-test for persistence, recovery, backup, and shutdown behavior. It uses `/fresh_selftest`, `/fresh_selftest_src`, and `/fresh_selftest_dst`, touches internal storage files, and should only be run on a test device or test partition. |
''',
)
replace_once(
    "README.md",
    "| [`docs/configuration.md`](docs/configuration.md) | `FreshConfig` options and defaults. |",
    "| [`docs/configuration.md`](docs/configuration.md) | `FreshConfig` options and defaults. |\n| [`docs/storage.md`](docs/storage.md) | Built-in storage, custom backends, ownership, and durability contracts. |",
)
replace_once(
    "README.md",
    "| Filesystem | LittleFS |",
    "| Storage | LittleFS, SDSPI, SDMMC, or custom `FreshStorage` |",
)
replace_once(
    "README.md",
    "| Status | Early-stage `0.1.0` |",
    "| Status | `0.2.0` development |",
)

replace_once(
    "docs/api.md",
    "`FreshStatus` values include `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `FileSystemError`, `ModelExists`, `ModelNotFound`, `DocumentNotFound`, `InvalidModel`, `ValidationFailed`, `OutOfMemory`, `UnsupportedOperation`, `CorruptData`, `StorageFull`, `SizeLimitExceeded`, `Busy`, `BackupNotRunning`, `Cancelled`, `Timeout`, and `InternalError`.",
    "`FreshStatus` values include `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `FileSystemError`, `StorageUnavailable`, `ModelExists`, `ModelNotFound`, `DocumentNotFound`, `InvalidModel`, `ValidationFailed`, `OutOfMemory`, `UnsupportedOperation`, `CorruptData`, `StorageFull`, `SizeLimitExceeded`, `Busy`, `BackupNotRunning`, `Cancelled`, `Timeout`, and `InternalError`.",
)
replace_once(
    "docs/api.md",
    "Storage limits can return `StorageFull` when sync preflight cannot preserve the configured LittleFS reserve, or `SizeLimitExceeded` when a document, stream entry, journal record, or snapshot exceeds configured serialized-size limits.",
    "Storage limits can return `StorageFull` when sync preflight cannot preserve the configured backend reserve, `StorageUnavailable` when the selected backend is not mounted, or `SizeLimitExceeded` when a document, stream entry, journal record, or snapshot exceeds configured serialized-size limits.",
)
replace_once(
    "docs/api.md",
    '''| `init(path, config)` | Mount/load the database and start the sync task. |
| `deinit(options)` | Stop the sync task, optionally run a final forced checkpoint, and release runtime state. |
''',
    '''| `init(path, config)` | Create and manage the selected built-in backend, load the database, and start the sync task. |
| `init(path, unique_ptr<FreshStorage>, config)` | Take ownership of a custom backend and manage its complete lifecycle. |
| `init(path, FreshStorage&, config)` | Attach an already-mounted caller-owned custom backend without unmounting it. |
| `deinit(options)` | Stop the sync task, optionally run a final forced checkpoint, detach or unmount storage, and release runtime state. |
''',
)
replace_once(
    "docs/api.md",
    '''| `storageInfo()` | Return LittleFS total, used, and free bytes. |
| `diagnostics()` | Return model load diagnostics collected during `init()`. |
''',
    '''| `storageInfo()` | Return convenience storage identity, state, capacity, and native diagnostics. |
| `storageInfo(result)` | Query storage information with a `FreshResult` that reports capacity-query failures. |
| `storage()` | Return the active backend while Fresh is running for same-filesystem application files. |
| `diagnostics()` | Return model load diagnostics collected during `init()`. |
''',
)
replace_once(
    "docs/api.md",
    '''| Field | Meaning |
| --- | --- |
| `totalBytes` | Total LittleFS bytes reported by the filesystem. |
| `usedBytes` | Used LittleFS bytes reported by the filesystem. |
| `freeBytes` | Calculated free bytes. |
''',
    '''| Field | Meaning |
| --- | --- |
| `type` | `LittleFS`, `SD`, or `Custom`. |
| `state` | Current storage lifecycle state. |
| `name` | Backend-provided display name. |
| `mountPath` | VFS mount path when applicable. |
| `nativeError` | Backend-native error code captured by the latest backend operation. |
| `openFileCount` | Active `FreshFile` handles tracked by the backend. |
| `totalBytes` | Total bytes reported by the backend. |
| `usedBytes` | Used bytes reported by the backend. |
| `freeBytes` | Free bytes reported or calculated by the backend. |
''',
)
replace_once(
    "docs/api.md",
    "`FreshStorageInfo` is returned by `Fresh::storageInfo()`.",
    "`FreshStorageInfo` is returned by `Fresh::storageInfo()`. Prefer `FreshResult storageInfo(FreshStorageInfo&)` when a failed capacity query must be distinguished from zero capacity. See [`storage.md`](storage.md) for backend and custom-storage APIs.",
)

examples_path = ROOT / "docs/examples.md"
examples = examples_path.read_text(encoding="utf-8")
marker = "## SelfTest\n"
if examples.count(marker) != 1:
    raise SystemExit("docs/examples.md: SelfTest marker not found exactly once")
insert = '''## SDSPIStorage

Path: [`../examples/SDSPIStorage/SDSPIStorage.ino`](../examples/SDSPIStorage/SDSPIStorage.ino)

Configures Fresh-managed SD storage over SPI, including bus ownership, pins, mount path, frequency, and result-aware capacity reporting.

## SDMMCStorage

Path: [`../examples/SDMMCStorage/SDMMCStorage.ino`](../examples/SDMMCStorage/SDMMCStorage.ino)

Configures Fresh-managed SDMMC storage using target-default routing. See `storage.md` for custom GPIO-matrix routing.

## SameFilesystemBackup

Path: [`../examples/SameFilesystemBackup/SameFilesystemBackup.ino`](../examples/SameFilesystemBackup/SameFilesystemBackup.ino)

Streams a Fresh backup into `/backups/configuration.fresh` through the active backend, then explicitly synchronizes and closes the archive.

## CustomStorage

Path: [`../examples/CustomStorage/CustomStorage.ino`](../examples/CustomStorage/CustomStorage.ino)

Implements a non-VFS in-memory `FreshStorage` and `FreshFileBackend`, attaches it as caller-owned storage, persists a document, deinitializes, reinitializes, and verifies the document reloads.

'''
examples_path.write_text(examples.replace(marker, insert + marker, 1), encoding="utf-8")

print("storage documentation references updated")
