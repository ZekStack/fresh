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
    "src/Fresh.h",
    '''struct FreshStorageInfo {
\tsize_t totalBytes = 0;
\tsize_t usedBytes = 0;
\tsize_t freeBytes = 0;
};
''',
    '''struct FreshStorageInfo {
\tFreshStorageType type = FreshStorageType::LittleFS;
\tFreshStorageState state = FreshStorageState::Uninitialized;
\tstd::string name;
\tstd::string mountPath;
\tint nativeError = 0;
\tsize_t openFileCount = 0;
\tsize_t totalBytes = 0;
\tsize_t usedBytes = 0;
\tsize_t freeBytes = 0;
};
''',
)

replace_once(
    "src/Fresh.cpp",
    '''\t\tif (_lifecycle == Lifecycle::Running) {
\t\t\t_lifecycle = Lifecycle::FinalSync;
''',
    '''\t\tif (_storage && _storage->openFileCount() != 0) {
\t\t\treturn FreshResult::failure(
\t\t\t    FreshStatus::Busy,
\t\t\t    "storage still has open files",
\t\t\t    _storage->openFileCount()
\t\t\t);
\t\t}
\t\tif (_lifecycle == Lifecycle::Running) {
\t\t\t_lifecycle = Lifecycle::FinalSync;
''',
)
replace_once(
    "src/Fresh.cpp",
    '''FreshStorageInfo Fresh::storageInfo() const {
\tFreshLock lock(*_mutex);
\treturn _storage ? _storage->info() : FreshStorageInfo();
}
''',
    '''FreshStorageInfo Fresh::storageInfo() const {
\tFreshLock lock(*_mutex);
\tFreshStorageInfo info;
\tif (!_storage) return info;
\tinfo = _storage->info();
\tinfo.type = _storage->type();
\tinfo.state = _storage->state();
\tinfo.name = _storage->name() != nullptr ? _storage->name() : "";
\tinfo.mountPath = _storage->mountPath() != nullptr ? _storage->mountPath() : "";
\tinfo.nativeError = _storage->nativeError();
\tinfo.openFileCount = _storage->openFileCount();
\treturn info;
}
''',
)

replace_once(
    "src/FreshModel.cpp",
    '''#include <LittleFS.h>

''',
    '''#include "internal/FreshStorageContext.h"

#define LittleFS FreshCurrentFileSystem()

''',
)
replace_once(
    "src/FreshModel.cpp",
    '''\t\tif (_stopping || _lifecycle != Lifecycle::Running) {
\t\t\treturn {.result = false, .status = FreshStatus::Busy, .message = "database is stopping"};
\t\t}
\t\tauto existing = _models.find(modelName);
''',
    '''\t\tif (_stopping || _lifecycle != Lifecycle::Running) {
\t\t\treturn {.result = false, .status = FreshStatus::Busy, .message = "database is stopping"};
\t\t}
\t\tif (!_storage || !_storage->isMounted()) {
\t\t\treturn {
\t\t\t    .result = false,
\t\t\t    .status = FreshStatus::StorageUnavailable,
\t\t\t    .message = "storage is unavailable"
\t\t\t};
\t\t}
\t\tFreshStorageScope storageScope(_storage.get());
\t\tauto existing = _models.find(modelName);
''',
)

print("storage hardening migration applied")
