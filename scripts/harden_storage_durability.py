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
    "src/FreshFile.cpp",
    '''FreshResult FreshFile::close() {
\tif (_backend == nullptr) {
\t\t_storage = nullptr;
\t\treturn FreshResult::success("file already closed");
\t}

\tFreshStorage *storage = _storage;
\tFreshResult result = _backend->close();
\t_backend.reset();
\t_storage = nullptr;
\tif (storage != nullptr) storage->releaseFileHandle();
\treturn result;
}
''',
    '''FreshResult FreshFile::close() {
\tif (_backend == nullptr) {
\t\t_storage = nullptr;
\t\treturn FreshResult::success("file already closed");
\t}

\tFreshStorage *storage = _storage;
\tFreshResult result = _backend->close();
\t_backend.reset();
\t_storage = nullptr;
\tif (storage != nullptr) storage->releaseFileHandle();
\treturn result;
}

FreshResult FreshFile::syncAndClose() {
\tFreshResult syncResult = sync();
\tFreshResult closeResult = close();
\treturn syncResult ? closeResult : syncResult;
}
''',
)

replace_once(
    "src/FreshStorage.cpp",
    '''\tconst size_t written = file.write(bytes.data(), bytes.size());
\tfile.flush();
\tfile.close();
\tif (written != bytes.size()) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to write durable slot");
\t}
''',
    '''\tconst size_t written = file.write(bytes.data(), bytes.size());
\tconst bool writeFailed = file.getWriteError() != 0;
\tFreshResult durabilityResult = file.syncAndClose();
\tif (writeFailed || written != bytes.size()) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to write durable slot");
\t}
\tif (!durabilityResult) return durabilityResult;
''',
)
replace_once(
    "src/FreshStorage.cpp",
    '''\tconst size_t written = file.write(record.payload.data(), record.payload.size());
\tfile.flush();
\tfile.close();

\tif (written != record.payload.size()) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to write journal");
\t}
''',
    '''\tconst size_t written = file.write(record.payload.data(), record.payload.size());
\tconst bool writeFailed = file.getWriteError() != 0;
\tFreshResult durabilityResult = file.syncAndClose();

\tif (writeFailed || written != record.payload.size()) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to write journal");
\t}
\tif (!durabilityResult) return durabilityResult;
''',
)
replace_once(
    "src/FreshStorage.cpp",
    'return FreshResult::failure(FreshStatus::StorageFull, "not enough LittleFS space");',
    'return FreshResult::failure(FreshStatus::StorageFull, "not enough storage space");',
)

replace_once(
    "src/FreshBackupRestore.cpp",
    '''\tconst size_t written = output.write(encoded.data(), encoded.size());
\toutput.flush();
\toutput.close();

\tconst FreshRestoreSlotVerification verification = FreshVerifyExactRestoreSlot(
''',
    '''\tconst size_t written = output.write(encoded.data(), encoded.size());
\tconst bool writeFailed = output.getWriteError() != 0;
\tFreshResult durabilityResult = output.syncAndClose();

\tconst FreshRestoreSlotVerification verification = FreshVerifyExactRestoreSlot(
''',
)
replace_once(
    "src/FreshBackupRestore.cpp",
    '''\tif (verification == FreshRestoreSlotVerification::Exact) {
\t\tcommitState = FreshRestoreManifestCommitState::Committed;
\t\treturn FreshResult::success("restore manifest committed");
\t}
''',
    '''\tif (verification == FreshRestoreSlotVerification::Exact) {
\t\tif (writeFailed || written != encoded.size() || !durabilityResult) {
\t\t\tcommitState = FreshRestoreManifestCommitState::Unknown;
\t\t\treturn FreshResult::failure(
\t\t\t    FreshStatus::FileSystemError,
\t\t\t    "restore manifest durability could not be confirmed; reboot required"
\t\t\t);
\t\t}
\t\tcommitState = FreshRestoreManifestCommitState::Committed;
\t\treturn FreshResult::success("restore manifest committed");
\t}
''',
)
replace_once(
    "src/FreshBackupRestore.cpp",
    '''\t\treturn FreshResult::failure(
\t\t    FreshStatus::FileSystemError,
\t\t    written == encoded.size()
\t\t        ? "restore manifest verification failed"
\t\t        : "restore manifest write was incomplete"
\t\t);
''',
    '''\t\treturn FreshResult::failure(
\t\t    FreshStatus::FileSystemError,
\t\t    !writeFailed && written == encoded.size() && durabilityResult
\t\t        ? "restore manifest verification failed"
\t\t        : "restore manifest write was incomplete"
\t\t);
''',
)
replace_once(
    "src/FreshBackupRestore.cpp",
    '''\tconst size_t written = output.write(encoded.data(), encoded.size());
\toutput.flush();
\toutput.close();
\tif (written != encoded.size()) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to write restore snapshot");
\t}
''',
    '''\tconst size_t written = output.write(encoded.data(), encoded.size());
\tconst bool writeFailed = output.getWriteError() != 0;
\tFreshResult durabilityResult = output.syncAndClose();
\tif (writeFailed || written != encoded.size()) {
\t\treturn FreshResult::failure(FreshStatus::FileSystemError, "failed to write restore snapshot");
\t}
\tif (!durabilityResult) return durabilityResult;
''',
)

replace_once(
    "src/internal/FreshStorageContext.cpp",
    '''bool FreshStorageFileSystem::exists(const char *path) const {
\tif (FreshActiveStorage == nullptr) return false;
\tbool result = false;
\treturn FreshActiveStorage->exists(path, result) && result;
}
''',
    '''bool FreshStorageFileSystem::exists(const char *path) const {
\tif (FreshActiveStorage == nullptr) return true;
\tbool result = false;
\tFreshResult existsResult = FreshActiveStorage->exists(path, result);
\t// Existing persistence code treats false as proven absence. On backend
\t// failure, conservatively report present so the following open/read fails
\t// instead of creating an empty database or deleting uncertain storage.
\treturn !existsResult || result;
}
''',
)

print("storage durability hardening applied")
