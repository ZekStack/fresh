#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "src/FreshUtils.cpp"
text = path.read_text(encoding="utf-8")

replacements = {
    "void FreshWriteU16(File &file, uint16_t value)": "void FreshWriteU16(Print &file, uint16_t value)",
    "void FreshWriteU32(File &file, uint32_t value)": "void FreshWriteU32(Print &file, uint32_t value)",
    "void FreshWriteU64(File &file, uint64_t value)": "void FreshWriteU64(Print &file, uint64_t value)",
    "bool FreshReadU16(File &file, uint16_t &value)": "bool FreshReadU16(Stream &file, uint16_t &value)",
    "bool FreshReadU32(File &file, uint32_t &value)": "bool FreshReadU32(Stream &file, uint32_t &value)",
    "bool FreshReadU64(File &file, uint64_t &value)": "bool FreshReadU64(Stream &file, uint64_t &value)",
}

for old, new in replacements.items():
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old}")
    text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("persistence helpers generalized")
