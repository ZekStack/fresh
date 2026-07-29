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
    "src/internal/FreshStorageContext.h",
    '''FreshStorageFileSystem &FreshCurrentFileSystem();
FreshStorage *FreshCurrentStorage();
''',
    '''FreshStorageFileSystem &FreshCurrentFileSystem();
FreshStorage *FreshCurrentStorage();
bool FreshHasInternalStorageAccess(const FreshStorage *storage);
''',
)
replace_once(
    "src/internal/FreshStorageContext.cpp",
    '''FreshStorage *FreshCurrentStorage() {
\treturn FreshActiveStorage;
}

FreshStorage *FreshStorageFileSystem::storage() const {
''',
    '''FreshStorage *FreshCurrentStorage() {
\treturn FreshActiveStorage;
}

bool FreshHasInternalStorageAccess(const FreshStorage *storage) {
\treturn storage != nullptr && FreshActiveStorage == storage;
}

FreshStorage *FreshStorageFileSystem::storage() const {
''',
)

replace_once(
    "src/FreshStorage.h",
    '''  private:
\tvoid releaseFileHandle();

\tFreshStorageType _type;
''',
    '''  private:
\tvoid releaseFileHandle();
\tvoid setProtectedPath(const std::string &path);
\tFreshResult validatePathAccess(const char *path) const;

\tFreshStorageType _type;
''',
)
replace_once(
    "src/FreshStorage.h",
    '''\tstd::string _mountPath;
\tstd::atomic<size_t> _openFileCount{0};
''',
    '''\tstd::string _mountPath;
\tstd::string _protectedPath;
\tstd::atomic<size_t> _openFileCount{0};
''',
)

replace_once(
    "src/FreshStorageBackend.cpp",
    '#include "internal/FreshVFSFile.h"\n',
    '#include "internal/FreshVFSFile.h"\n#include "internal/FreshStorageContext.h"\n',
)
replace_once(
    "src/FreshStorageBackend.cpp",
    '''FreshResult FreshStorage::resolvePath(const char *logicalPath, std::string &resolvedPath) const {
''',
    '''void FreshStorage::setProtectedPath(const std::string &path) {
\t_protectedPath = path;
\twhile (_protectedPath.size() > 1 && _protectedPath.back() == '/') {
\t\t_protectedPath.pop_back();
\t}
}

FreshResult FreshStorage::validatePathAccess(const char *path) const {
\tif (path == nullptr || *path == '\\0') {
\t\treturn FreshResult::failure(FreshStatus::InvalidArgument, "storage path is required");
\t}
\tif (_protectedPath.empty() || FreshHasInternalStorageAccess(this)) {
\t\treturn FreshResult::success("storage path allowed");
\t}
\tconst std::string logicalPath(path);
\tconst bool exact = logicalPath == _protectedPath;
\tconst bool child = _protectedPath == "/" ||
\t                   (logicalPath.size() > _protectedPath.size() &&
\t                    logicalPath.compare(0, _protectedPath.size(), _protectedPath) == 0 &&
\t                    logicalPath[_protectedPath.size()] == '/');
\tif (exact || child) {
\t\treturn FreshResult::failure(
\t\t    FreshStatus::UnsupportedOperation,
\t\t    "application access to the Fresh database root is forbidden"
\t\t);
\t}
\treturn FreshResult::success("storage path allowed");
}

FreshResult FreshStorage::resolvePath(const char *logicalPath, std::string &resolvedPath) const {
''',
)

for signature in [
    'FreshResult FreshStorage::open(const char *path, FreshOpenMode mode, FreshFile &file) {',
    'FreshResult FreshStorage::exists(const char *path, bool &result) const {',
    'FreshResult FreshStorage::createDirectory(const char *path) {',
    'FreshResult FreshStorage::removeFile(const char *path) {',
    'FreshResult FreshStorage::removeDirectory(const char *path) {',
]:
    replacement = signature + '''
\tFreshResult accessResult = validatePathAccess(path);
\tif (!accessResult) return accessResult;'''
    replace_once("src/FreshStorageBackend.cpp", signature, replacement)

replace_once(
    "src/FreshStorageBackend.cpp",
    '''FreshResult FreshStorage::listDirectory(
    const char *path,
    std::vector<FreshDirectoryEntry> &entries
) const {
''',
    '''FreshResult FreshStorage::listDirectory(
    const char *path,
    std::vector<FreshDirectoryEntry> &entries
) const {
\tFreshResult accessResult = validatePathAccess(path);
\tif (!accessResult) {
\t\tentries.clear();
\t\treturn accessResult;
\t}
''',
)

replace_once(
    "src/Fresh.cpp",
    '''\t_rootPath = dbPath;
\tif (_rootPath.back() == '/' && _rootPath.size() > 1) _rootPath.pop_back();

\tFreshResult dirResult = ensureDir(_rootPath);
''',
    '''\t_rootPath = dbPath;
\tif (_rootPath.back() == '/' && _rootPath.size() > 1) _rootPath.pop_back();
\t_storage->setProtectedPath(_rootPath);

\tFreshResult dirResult = ensureDir(_rootPath);
''',
)

print("database storage root protection applied")
