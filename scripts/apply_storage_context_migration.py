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
    "src/Fresh.cpp",
    '#include "internal/FreshStorageFactory.h"\n',
    '#include "internal/FreshStorageFactory.h"\n#include "internal/FreshStorageContext.h"\n',
)
replace_once(
    "src/Fresh.cpp",
    '''\t// The SD backend and VFS file layer are implemented, but the persistence
\t// engine still has direct Arduino LittleFS calls. Keep SD unavailable until
\t// every journal, snapshot, manifest, restore, and GC path is migrated.
\tif (config.storageType != FreshStorageType::LittleFS) {
\t\treturn FreshResult::failure(
\t\t    FreshStatus::UnsupportedOperation,
\t\t    "selected storage backend is not connected to the persistence engine yet"
\t\t);
\t}

''',
    "",
)
replace_once(
    "src/Fresh.cpp",
    '''\tFreshResult storageMounted = _storage->mount();
\tif (!storageMounted) {
\t\tresetInitState();
\t\treturn storageMounted;
\t}

''',
    '''\tFreshResult storageMounted = _storage->mount();
\tif (!storageMounted) {
\t\tresetInitState();
\t\treturn storageMounted;
\t}
\tFreshStorageScope storageScope(_storage.get());

''',
)

replace_once(
    "src/FreshStorage.cpp",
    "#include <LittleFS.h>\n",
    '''#include "FreshFile.h"
#include "internal/FreshStorageContext.h"

#define File FreshFile
#define LittleFS FreshCurrentFileSystem()
''',
)
replace_once(
    "src/FreshStorage.cpp",
    "FreshResult Fresh::syncDirty(bool force) {\n",
    '''FreshResult Fresh::syncDirty(bool force) {
\tif (!_storage || !_storage->isMounted()) {
\t\treturn FreshResult::failure(FreshStatus::StorageUnavailable, "storage is unavailable");
\t}
\tFreshStorageScope storageScope(_storage.get());
''',
)

replace_once(
    "src/FreshBackupRestore.cpp",
    "#include <LittleFS.h>\n",
    '''#include "FreshFile.h"
#include "internal/FreshStorageContext.h"

#define File FreshFile
#define LittleFS FreshCurrentFileSystem()
''',
)
replace_once(
    "src/FreshBackupRestore.cpp",
    '''\t{
\t\tFreshLock lock(*_mutex);
\t\tif (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
\t\tif (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
\t\tif (_stopping || _lifecycle != Lifecycle::Running) {
\t\t\treturn FreshResult::failure(FreshStatus::Busy, "database is stopping");
\t\t}
\t}

\t// Preserved and protected states keep their existing storage IDs, so the
''',
    '''\t{
\t\tFreshLock lock(*_mutex);
\t\tif (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
\t\tif (!_initialized || !_storage || !_storage->isMounted()) {
\t\t\treturn FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
\t\t}
\t\tif (_stopping || _lifecycle != Lifecycle::Running) {
\t\t\treturn FreshResult::failure(FreshStatus::Busy, "database is stopping");
\t\t}
\t}
\tFreshStorageScope storageScope(_storage.get());

\t// Preserved and protected states keep their existing storage IDs, so the
''',
)

replace_once("src/FreshGarbageCollection.cpp", "\nn\t\t}\n", "\n\t\t}\n")

print("storage context migration applied")
