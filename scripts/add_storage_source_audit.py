#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[1] / ".github/workflows/ci.yml"
text = path.read_text(encoding="utf-8")
old = '''          if grep -RInE '(^|[^[:alnum:]_])throw([^[:alnum:]_]|$)|std::abort[[:space:]]*\\(' src; then
            echo "Embedded safety audit failed"
            exit 1
          fi
'''
new = '''          if grep -RInE '(^|[^[:alnum:]_])throw([^[:alnum:]_]|$)|std::abort[[:space:]]*\\(' src; then
            echo "Embedded safety audit failed"
            exit 1
          fi
          if grep -RInE '#include[[:space:]]*[<"](LittleFS|SD|SD_MMC)\\.h[>"]' src; then
            echo "Production storage code must use FreshStorage or ESP-IDF backends"
            exit 1
          fi
          if grep -RInE '(^|[^[:alnum:]_])LittleFS[[:space:]]*\\.' src; then
            echo "Direct Arduino LittleFS singleton access is forbidden"
            exit 1
          fi
'''
if text.count(old) != 1:
    raise SystemExit("expected production source audit block exactly once")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
print("permanent storage source audit added")
