#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "examples/CustomStorage/CustomStorage.ino"
text = path.read_text(encoding="utf-8")

replacements = [
    (
        '''\tFreshResult detach() {
\t\tFreshResult canDetach = validateCanUnmount();
\t\tif (!canDetach) return canDetach;
\t\tsetState(FreshStorageState::Uninitialized);
\t\treturn FreshResult::success("memory storage detached");
\t}

\tconst char *name() const override {
''',
        '''\tFreshResult detach() {
\t\tFreshResult canDetach = validateCanUnmount();
\t\tif (!canDetach) return canDetach;
\t\tsetState(FreshStorageState::Uninitialized);
\t\treturn FreshResult::success("memory storage detached");
\t}

\tvoid failNextInfoQuery() {
\t\t_failNextInfoQuery = true;
\t}

\tconst char *name() const override {
''',
    ),
    (
        '''\tFreshResult unmount() override {
\t\treturn detach();
\t}

\tFreshResult openBackend(
''',
        '''\tFreshResult unmount() override {
\t\treturn detach();
\t}

\tFreshResult readInfoBackend(FreshStorageInfo &result) const override {
\t\tif (_failNextInfoQuery) {
\t\t\t_failNextInfoQuery = false;
\t\t\tresult = FreshStorageInfo();
\t\t\treturn FreshResult::failure(
\t\t\t    FreshStatus::FileSystemError,
\t\t\t    "injected memory storage information failure"
\t\t\t);
\t\t}
\t\tresult = info();
\t\treturn FreshResult::success("memory storage information read");
\t}

\tFreshResult openBackend(
''',
    ),
    (
        '''\tstd::map<std::string, std::vector<uint8_t>> _files;
\tstd::set<std::string> _directories;
''',
        '''\tstd::map<std::string, std::vector<uint8_t>> _files;
\tstd::set<std::string> _directories;
\tmutable bool _failNextInfoQuery = false;
''',
    ),
    (
        '''\t\tFreshResult found = database.model("settings").findById(documentId);
\t\trequire(found && std::string(found.doc["name"] | "") == "custom-storage", "reload document");
\t\trequire(database.deinit(), "deinitialize second database");
''',
        '''\t\tFreshResult found = database.model("settings").findById(documentId);
\t\trequire(found && std::string(found.doc["name"] | "") == "custom-storage", "reload document");

\t\tFreshStorageInfo storageInfo;
\t\tstorage.failNextInfoQuery();
\t\tFreshResult infoFailure = database.storageInfo(storageInfo);
\t\trequire(
\t\t    !infoFailure && infoFailure.status == FreshStatus::FileSystemError,
\t\t    "propagate custom storage information failure"
\t\t);
\t\trequire(database.storageInfo(storageInfo), "retry custom storage information");

\t\tFreshStorage *activeStorage = database.storage();
\t\trequire(activeStorage != nullptr, "resolve custom storage view");
\t\tFreshFile forbidden;
\t\tFreshResult forbiddenOpen = activeStorage->open(
\t\t    "/fresh/forbidden.bin",
\t\t    FreshOpenMode::Write,
\t\t    forbidden
\t\t);
\t\trequire(
\t\t    !forbiddenOpen && forbiddenOpen.status == FreshStatus::UnsupportedOperation,
\t\t    "protect custom database root"
\t\t);

\t\trequire(activeStorage->createDirectory("/backups"), "create custom backup directory");
\t\tFreshFile archive;
\t\trequire(
\t\t    activeStorage->open("/backups/custom.bin", FreshOpenMode::Write, archive),
\t\t    "open custom application file"
\t\t);
\t\tconst uint8_t marker[] = {0x46, 0x52, 0x45, 0x53, 0x48};
\t\trequire(archive.write(marker, sizeof(marker)) == sizeof(marker), "write custom application file");

\t\tFreshResult busy = database.deinit();
\t\trequire(
\t\t    !busy && busy.status == FreshStatus::Busy,
\t\t    "custom storage deinit rejects open file"
\t\t);
\t\trequire(archive.syncAndClose(), "close custom application file");
\t\trequire(database.deinit(), "deinitialize second database");
''',
    ),
]

for old, new in replacements:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old!r}")
    text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("custom storage conformance extended")
