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
    '''\tFreshResult init(const char *dbPath, const FreshConfig &config = FreshConfig());
\tFreshResult deinit(const FreshDeinitOptions &options = FreshDeinitOptions());
''',
    '''\tFreshResult init(const char *dbPath, const FreshConfig &config = FreshConfig());
\tFreshResult init(
\t    const char *dbPath,
\t    std::unique_ptr<FreshStorage> storage,
\t    const FreshConfig &config = FreshConfig()
\t);
\tFreshResult init(
\t    const char *dbPath,
\t    FreshStorage &storage,
\t    const FreshConfig &config = FreshConfig()
\t);
\tFreshResult deinit(const FreshDeinitOptions &options = FreshDeinitOptions());
''',
)
replace_once(
    "src/Fresh.h",
    '''\tFreshResult validateConfig(const FreshConfig &config) const;
\tstd::string modelPath(const std::string &storageId) const;
''',
    '''\tFreshResult validateConfig(const FreshConfig &config) const;
\tFreshResult initWithStorage(
\t    const char *dbPath,
\t    const FreshConfig &config,
\t    std::unique_ptr<FreshStorage> suppliedStorage
\t);
\tstd::string modelPath(const std::string &storageId) const;
''',
)

replace_once(
    "src/Fresh.cpp",
    '#include "internal/FreshStorageFactory.h"\n',
    '#include "internal/FreshStorageFactory.h"\n#include "internal/FreshStorageReference.h"\n',
)
replace_once(
    "src/Fresh.cpp",
    '''\tFreshLittleFSConfig littleFS = config.littleFS;
\tif (config.eraseOnFileSystemFailure) littleFS.formatOnMountFailure = true;
\tFreshResult storageConfig = FreshValidateStorageConfig(config.storageType, littleFS, config.sd);
\tif (!storageConfig) return storageConfig;
''',
    '''\tif (config.storageType != FreshStorageType::Custom) {
\t\tFreshLittleFSConfig littleFS = config.littleFS;
\t\tif (config.eraseOnFileSystemFailure) littleFS.formatOnMountFailure = true;
\t\tFreshResult storageConfig = FreshValidateStorageConfig(config.storageType, littleFS, config.sd);
\t\tif (!storageConfig) return storageConfig;
\t}
''',
)
replace_once(
    "src/Fresh.cpp",
    '''FreshResult Fresh::init(const char *dbPath, const FreshConfig &config) {
''',
    '''FreshResult Fresh::init(const char *dbPath, const FreshConfig &config) {
\tif (config.storageType == FreshStorageType::Custom) {
\t\treturn FreshResult::failure(
\t\t    FreshStatus::InvalidArgument,
\t\t    "custom storage requires a storage instance"
\t\t);
\t}
\treturn initWithStorage(dbPath, config, nullptr);
}

FreshResult Fresh::init(
    const char *dbPath,
    std::unique_ptr<FreshStorage> storage,
    const FreshConfig &config
) {
\tif (!storage) {
\t\treturn FreshResult::failure(FreshStatus::InvalidArgument, "custom storage is required");
\t}
\tFreshConfig effectiveConfig = config;
\teffectiveConfig.storageType = FreshStorageType::Custom;
\treturn initWithStorage(dbPath, effectiveConfig, std::move(storage));
}

FreshResult Fresh::init(
    const char *dbPath,
    FreshStorage &storage,
    const FreshConfig &config
) {
\tstd::unique_ptr<FreshStorage> reference(
\t    new (std::nothrow) FreshStorageReference(storage)
\t);
\tif (!reference) {
\t\treturn FreshResult::failure(
\t\t    FreshStatus::OutOfMemory,
\t\t    "failed to allocate custom storage reference"
\t\t);
\t}
\tFreshConfig effectiveConfig = config;
\teffectiveConfig.storageType = FreshStorageType::Custom;
\treturn initWithStorage(dbPath, effectiveConfig, std::move(reference));
}

FreshResult Fresh::initWithStorage(
    const char *dbPath,
    const FreshConfig &config,
    std::unique_ptr<FreshStorage> suppliedStorage
) {
''',
)
replace_once(
    "src/Fresh.cpp",
    '''\tFreshResult storageCreated = FreshCreateStorage(
\t    _config.storageType,
\t    _config.littleFS,
\t    _config.sd,
\t    _storage
\t);
\tif (!storageCreated) {
\t\tresetInitState();
\t\treturn storageCreated;
\t}
''',
    '''\tif (suppliedStorage) {
\t\t_storage = std::move(suppliedStorage);
\t} else {
\t\tFreshResult storageCreated = FreshCreateStorage(
\t\t    _config.storageType,
\t\t    _config.littleFS,
\t\t    _config.sd,
\t\t    _storage
\t\t);
\t\tif (!storageCreated) {
\t\t\tresetInitState();
\t\t\treturn storageCreated;
\t\t}
\t}
''',
)

print("custom storage init migration applied")
