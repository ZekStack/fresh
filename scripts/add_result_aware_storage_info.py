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
    "src/FreshStorage.h",
    '''\tvirtual const char *name() const = 0;
\tvirtual FreshStorageInfo info() const = 0;
\tvirtual int nativeError() const {
''',
    '''\tvirtual const char *name() const = 0;
\tvirtual FreshStorageInfo info() const = 0;
\tFreshResult readInfo(FreshStorageInfo &result) const;
\tvirtual int nativeError() const {
''',
)
replace_once(
    "src/FreshStorage.h",
    '''\t// Default implementations resolve the logical path below mountPath() and
\t// use ESP-IDF VFS/POSIX. Custom backends may override every primitive.
\tvirtual FreshResult openBackend(
''',
    '''\t// Default implementations resolve the logical path below mountPath() and
\t// use ESP-IDF VFS/POSIX. Custom backends may override every primitive.
\tvirtual FreshResult readInfoBackend(FreshStorageInfo &result) const;
\tvirtual FreshResult openBackend(
''',
)

replace_once(
    "src/FreshStorageBackend.cpp",
    '''FreshResult FreshStorage::validateCanUnmount() const {
''',
    '''FreshResult FreshStorage::readInfo(FreshStorageInfo &result) const {
\tresult = FreshStorageInfo();
\tif (!isMounted()) {
\t\tresult.type = type();
\t\tresult.state = state();
\t\tresult.name = name() != nullptr ? name() : "";
\t\tresult.mountPath = mountPath() != nullptr ? mountPath() : "";
\t\tresult.nativeError = nativeError();
\t\tresult.openFileCount = openFileCount();
\t\treturn FreshResult::failure(FreshStatus::StorageUnavailable, "storage is not mounted");
\t}
\tFreshResult infoResult = readInfoBackend(result);
\tresult.type = type();
\tresult.state = state();
\tresult.name = name() != nullptr ? name() : "";
\tresult.mountPath = mountPath() != nullptr ? mountPath() : "";
\tresult.nativeError = nativeError();
\tresult.openFileCount = openFileCount();
\treturn infoResult;
}

FreshResult FreshStorage::readInfoBackend(FreshStorageInfo &result) const {
\tresult = info();
\treturn FreshResult::success("storage information read");
}

FreshResult FreshStorage::validateCanUnmount() const {
''',
)

replace_once(
    "src/storage/FreshLittleFSStorage.h",
    '''\tFreshResult mount() override;
\tFreshResult unmount() override;

\tFreshLittleFSConfig _config;
\tstd::string _partitionLabel;
\tint _nativeError = 0;
''',
    '''\tFreshResult mount() override;
\tFreshResult unmount() override;
\tFreshResult readInfoBackend(FreshStorageInfo &result) const override;

\tFreshLittleFSConfig _config;
\tstd::string _partitionLabel;
\tmutable int _nativeError = 0;
''',
)
replace_once(
    "src/storage/FreshLittleFSStorage.cpp",
    '''FreshStorageInfo FreshLittleFSStorage::info() const {
\tFreshStorageInfo info;
#if defined(ESP32)
\tif (!isMounted()) return info;
\tsize_t totalBytes = 0;
\tsize_t usedBytes = 0;
\tconst esp_err_t result = esp_littlefs_info(
\t    _partitionLabel.c_str(),
\t    &totalBytes,
\t    &usedBytes
\t);
\tif (result != ESP_OK) return info;
\tinfo.totalBytes = totalBytes;
\tinfo.usedBytes = usedBytes;
\tinfo.freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
#endif
\treturn info;
}
''',
    '''FreshStorageInfo FreshLittleFSStorage::info() const {
\tFreshStorageInfo result;
\treadInfoBackend(result);
\treturn result;
}

FreshResult FreshLittleFSStorage::readInfoBackend(FreshStorageInfo &result) const {
\tresult = FreshStorageInfo();
#if !defined(ESP32)
\treturn FreshResult::failure(FreshStatus::UnsupportedOperation, "LittleFS backend requires ESP32");
#else
\tif (!isMounted()) {
\t\treturn FreshResult::failure(FreshStatus::StorageUnavailable, "LittleFS storage is not mounted");
\t}
\tsize_t totalBytes = 0;
\tsize_t usedBytes = 0;
\tconst esp_err_t queried = esp_littlefs_info(
\t    _partitionLabel.c_str(),
\t    &totalBytes,
\t    &usedBytes
\t);
\t_nativeError = static_cast<int>(queried);
\tif (queried != ESP_OK) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to query LittleFS storage");
\t}
\tresult.totalBytes = totalBytes;
\tresult.usedBytes = usedBytes;
\tresult.freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
\treturn FreshResult::success("LittleFS storage information read");
#endif
}
''',
)

replace_once(
    "src/storage/FreshSDStorage.h",
    '''\tFreshResult mount() override;
\tFreshResult unmount() override;
\tFreshResult mountSPI();
''',
    '''\tFreshResult mount() override;
\tFreshResult unmount() override;
\tFreshResult readInfoBackend(FreshStorageInfo &result) const override;
\tFreshResult mountSPI();
''',
)
replace_once(
    "src/storage/FreshSDStorage.h",
    '''\tbool _spiBusInitialized = false;
\tint _nativeError = 0;
''',
    '''\tbool _spiBusInitialized = false;
\tmutable int _nativeError = 0;
''',
)
replace_once(
    "src/storage/FreshSDStorage.cpp",
    '''FreshStorageInfo FreshSDStorage::info() const {
\tFreshStorageInfo info;
#if defined(ESP32)
\tif (!isMounted()) return info;
\tuint64_t totalBytes = 0;
\tuint64_t freeBytes = 0;
\tif (esp_vfs_fat_info(mountPath(), &totalBytes, &freeBytes) != ESP_OK) return info;
\tconst uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
\tinfo.totalBytes = static_cast<size_t>(totalBytes > maxSize ? maxSize : totalBytes);
\tinfo.freeBytes = static_cast<size_t>(freeBytes > maxSize ? maxSize : freeBytes);
\tinfo.usedBytes = info.totalBytes > info.freeBytes ? info.totalBytes - info.freeBytes : 0;
#endif
\treturn info;
}
''',
    '''FreshStorageInfo FreshSDStorage::info() const {
\tFreshStorageInfo result;
\treadInfoBackend(result);
\treturn result;
}

FreshResult FreshSDStorage::readInfoBackend(FreshStorageInfo &result) const {
\tresult = FreshStorageInfo();
#if !defined(ESP32)
\treturn FreshResult::failure(FreshStatus::UnsupportedOperation, "SD storage requires ESP32");
#else
\tif (!isMounted()) {
\t\treturn FreshResult::failure(FreshStatus::StorageUnavailable, "SD storage is not mounted");
\t}
\tuint64_t totalBytes = 0;
\tuint64_t freeBytes = 0;
\tconst esp_err_t queried = esp_vfs_fat_info(mountPath(), &totalBytes, &freeBytes);
\t_nativeError = static_cast<int>(queried);
\tif (queried != ESP_OK) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to query SD storage");
\t}
\tconst uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
\tresult.totalBytes = static_cast<size_t>(totalBytes > maxSize ? maxSize : totalBytes);
\tresult.freeBytes = static_cast<size_t>(freeBytes > maxSize ? maxSize : freeBytes);
\tresult.usedBytes = result.totalBytes > result.freeBytes ? result.totalBytes - result.freeBytes : 0;
\treturn FreshResult::success("SD storage information read");
#endif
}
''',
)

replace_once(
    "src/internal/FreshStorageReference.h",
    '''\tFreshResult mount() override;
\tFreshResult unmount() override;
\tFreshResult openBackend(
''',
    '''\tFreshResult mount() override;
\tFreshResult unmount() override;
\tFreshResult readInfoBackend(FreshStorageInfo &result) const override;
\tFreshResult openBackend(
''',
)
replace_once(
    "src/internal/FreshStorageReference.cpp",
    '''int FreshStorageReference::nativeError() const {
\treturn _target.nativeError();
}

FreshResult FreshStorageReference::mount() {
''',
    '''int FreshStorageReference::nativeError() const {
\treturn _target.nativeError();
}

FreshResult FreshStorageReference::readInfoBackend(FreshStorageInfo &result) const {
\treturn _target.readInfo(result);
}

FreshResult FreshStorageReference::mount() {
''',
)

replace_once(
    "src/Fresh.h",
    '''\tFreshStorageInfo storageInfo() const;
\tFreshStorage *storage();
''',
    '''\tFreshStorageInfo storageInfo() const;
\tFreshResult storageInfo(FreshStorageInfo &result) const;
\tFreshStorage *storage();
''',
)
replace_once(
    "src/Fresh.cpp",
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
    '''FreshStorageInfo Fresh::storageInfo() const {
\tFreshStorageInfo result;
\tstorageInfo(result);
\treturn result;
}

FreshResult Fresh::storageInfo(FreshStorageInfo &result) const {
\tFreshLock lock(*_mutex);
\tif (!lock) {
\t\tresult = FreshStorageInfo();
\t\treturn FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
\t}
\tif (!_storage) {
\t\tresult = FreshStorageInfo();
\t\treturn FreshResult::failure(FreshStatus::NotInitialized, "database storage is not initialized");
\t}
\treturn _storage->readInfo(result);
}
''',
)

replace_once(
    "src/FreshStorage.cpp",
    '''FreshResult Fresh::checkFreeSpace(size_t requiredBytes) const {
\tconst size_t total = FreshFS.totalBytes();
\tconst size_t used = FreshFS.usedBytes();
\tconst size_t freeBytes = total > used ? total - used : 0;
''',
    '''FreshResult Fresh::checkFreeSpace(size_t requiredBytes) const {
\tFreshStorage *storage = FreshCurrentStorage();
\tif (storage == nullptr) {
\t\treturn FreshResult::failure(FreshStatus::StorageUnavailable, "storage is unavailable");
\t}
\tFreshStorageInfo info;
\tFreshResult infoResult = storage->readInfo(info);
\tif (!infoResult) return infoResult;
\tconst size_t freeBytes = info.freeBytes;
''',
)

replace_once(
    "src/FreshGarbageCollection.cpp",
    '''\t\tconst size_t usedBefore = storage->info().usedBytes;
\t\tconst bool removed = FreshRemoveGarbageCollectionTree(*storage, candidate);
\t\tconst size_t usedAfter = storage->info().usedBytes;
''',
    '''\t\tFreshStorageInfo beforeInfo;
\t\tFreshResult beforeInfoResult = storage->readInfo(beforeInfo);
\t\tif (!beforeInfoResult) return beforeInfoResult;
\t\tconst bool removed = FreshRemoveGarbageCollectionTree(*storage, candidate);
\t\tFreshStorageInfo afterInfo;
\t\tFreshResult afterInfoResult = storage->readInfo(afterInfo);
\t\tif (!afterInfoResult) return afterInfoResult;
\t\tconst size_t usedBefore = beforeInfo.usedBytes;
\t\tconst size_t usedAfter = afterInfo.usedBytes;
''',
)

print("result-aware storage information applied")
