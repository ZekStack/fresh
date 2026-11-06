#!/usr/bin/env python3
"""Compile and execute the production Fresh journal regression with POSIX ports.

The workflow pins ArduinoJson v7.4.3, and the host source uses the real
Fresh implementation. Only platform, RTOS, and filesystem ports are replaced.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get("ARDUINOJSON_DIR", ROOT / ".ci/ArduinoJson/src"))


def main() -> None:
    if not (JSON / "ArduinoJson.h").is_file():
        raise SystemExit(f"Missing ArduinoJson v7.4.3 headers: {JSON}")

    compiler = os.environ.get("CXX", "g++")
    extra = shlex.split(os.environ.get("FRESH_HOST_CXXFLAGS", ""))
    flags = [
        compiler, "-std=c++20", "-O1", "-g0", "-pthread", "-DFRESH_TESTING",
        "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
        "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
        "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
        f"-I{ROOT / 'test/host/fresh-platform'}", f"-I{JSON}", f"-I{ROOT / 'src'}",
        *extra,
    ]
    sources = [
        *sorted((ROOT / "src").rglob("*.cpp")),
        ROOT / "test/host/fresh_journal_preflight_test.cpp",
    ]
    with tempfile.TemporaryDirectory(prefix="fresh-journal-preflight-") as temporary:
        work = Path(temporary)

        def compile_one(entry: tuple[int, Path]) -> Path:
            index, source = entry
            output = work / f"{index}.o"
            subprocess.run([*flags, "-c", str(source), "-o", str(output)],
                           check=True, timeout=180)
            return output

        with ThreadPoolExecutor(max_workers=2) as pool:
            objects = list(pool.map(compile_one, enumerate(sources)))
        binary = work / "fresh-journal-host"
        subprocess.run([compiler, "-pthread", *map(str, objects), *extra,
                        "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary), str(work / "store")], check=True, timeout=180)


if __name__ == "__main__":
    main()
