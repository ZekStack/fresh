#!/usr/bin/env python3
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

with (ROOT / "library.json").open(encoding="utf-8") as handle:
    json_version = json.load(handle)["version"]

properties = (ROOT / "library.properties").read_text(encoding="utf-8")
match = re.search(r"^version=(.+)$", properties, re.MULTILINE)
if match is None:
    print("library.properties has no version entry", file=sys.stderr)
    raise SystemExit(1)
properties_version = match.group(1).strip()

versions = {"library.json": json_version, "library.properties": properties_version}
tag = os.environ.get("GITHUB_REF_NAME", "")
if tag.startswith("v"):
    versions["tag"] = tag[1:]

unique = set(versions.values())
if len(unique) != 1:
    for source, version in versions.items():
        print(f"{source}: {version}", file=sys.stderr)
    raise SystemExit("version metadata is inconsistent")

for path in SRC.rglob("*"):
    if not path.is_file() or path.suffix not in {".h", ".hpp", ".c", ".cc", ".cpp"}:
        continue
    relative_path = path.relative_to(ROOT)
    text = path.read_text(encoding="utf-8", errors="replace")
    if re.search(r'#include\s*[<"](?:LittleFS|SD|SD_MMC)\.h[>"]', text):
        raise SystemExit(
            f"{relative_path} directly includes an Arduino filesystem singleton"
        )
    if re.search(r"(?:^|[^A-Za-z0-9_])LittleFS\s*\.", text, re.MULTILINE):
        raise SystemExit(
            f"{relative_path} directly accesses the Arduino LittleFS singleton"
        )

print(f"version metadata is consistent: {json_version}")
print("storage source boundaries are valid")
