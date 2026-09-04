#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def require(path: str, needle: str, message: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    if needle not in text:
        raise SystemExit(message)


def reject_tree(needle: str, message: str) -> None:
    for path in SRC.rglob("*"):
        if not path.is_file() or path.suffix not in {".h", ".hpp", ".c", ".cc", ".cpp"}:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if needle in text:
            raise SystemExit(f"{message}: {path.relative_to(ROOT)}")


require(
    "library.json",
    '"Strata": "https://github.com/ZekStack/strata.git#v0.1.2"',
    "Fresh 0.2.0 must pin Strata v0.1.2",
)
require(
    "src/Fresh.h",
    ".allocation = Strata::Placement::PreferExternal",
    "FreshConfig must preserve PSRAM-preferred allocation behavior by default",
)
require(
    "src/Fresh.h",
    ".taskStack = Strata::Placement::Internal",
    "FreshConfig must keep the sync-task stack internal by default",
)
require(
    "src/FreshStorage.h",
    "FreshTaskStackRequirement::Any",
    "storage backends must be unconstrained by default",
)
require(
    "src/storage/FreshLittleFSStorage.h",
    "return FreshTaskStackRequirement::Internal;",
    "LittleFS must force the Fresh sync-task stack into internal RAM",
)
require(
    "src/Fresh.cpp",
    "_storage->syncTaskStackRequirement() == FreshTaskStackRequirement::Internal",
    "Fresh must resolve storage-imposed sync-task stack constraints",
)
require(
    "src/Fresh.cpp",
    "? Strata::Placement::Internal",
    "storage safety constraints must override the requested sync-task placement",
)
require(
    "src/Fresh.cpp",
    "Strata::FreeRTOS::Task::create(",
    "Fresh sync task must be owned by Strata",
)
require(
    "src/Fresh.cpp",
    "_syncTask.reset();",
    "Fresh must externally release the suspended Strata sync task",
)
require(
    "src/FreshFormat.cpp",
    'startSyncTask("failed to restart sync task after format")',
    "format restart must use the shared storage-aware Strata task creation path",
)
require(
    "src/internal/FreshMemory.cpp",
    "return Strata::allocate(size, placement);",
    "Fresh allocation shim must route through Strata",
)
require(
    "src/internal/FreshMemory.cpp",
    "return Strata::reallocate(pointer, newSize, placement);",
    "Fresh reallocation shim must route through Strata",
)
require(
    "src/internal/FreshMemory.cpp",
    "Strata::free(pointer);",
    "Fresh deallocation shim must route through Strata",
)

for forbidden, message in (
    ("heap_caps_", "Fresh production sources must not call ESP-IDF heap-cap APIs"),
    ("MALLOC_CAP_", "Fresh production sources must not encode ESP-IDF heap capabilities"),
    ("ps_malloc", "Fresh production sources must not allocate PSRAM directly"),
    ("xTaskCreate(", "Fresh-owned dynamic task creation must not bypass Strata"),
    ("xTaskCreatePinnedToCore(", "Fresh-owned pinned task creation must not bypass Strata"),
    ("xSemaphoreCreateBinary(", "Fresh-owned binary semaphores must not bypass Strata"),
    ("xSemaphoreCreateRecursiveMutex(", "Fresh-owned recursive mutexes must not bypass Strata"),
):
    reject_tree(forbidden, message)

print("Fresh Strata integration source contract is valid")
