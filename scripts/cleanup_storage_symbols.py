#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = [
    "src/FreshStorage.cpp",
    "src/FreshBackupRestore.cpp",
    "src/FreshModel.cpp",
]

for path in FILES:
    file_path = ROOT / path
    text = file_path.read_text(encoding="utf-8")
    define = "#define LittleFS FreshCurrentFileSystem()"
    if text.count(define) != 1:
        raise SystemExit(f"{path}: expected exactly one compatibility definition")
    call_count = text.count("LittleFS.")
    if call_count == 0:
        raise SystemExit(f"{path}: expected at least one compatibility call")
    text = text.replace(define, "#define FreshFS FreshCurrentFileSystem()")
    text = text.replace("LittleFS.", "FreshFS.")
    file_path.write_text(text, encoding="utf-8")
    print(f"{path}: migrated {call_count} calls")
