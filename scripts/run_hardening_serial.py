#!/usr/bin/env python3
"""Validate every Fresh FRESH_TESTING regression on a hardware serial port.

A successful run must use the requested mounted backend, execute every named
regression exactly once, report an outcome for each, and finish with matching
counters. A build-only check is not a hardware validation.
"""

import argparse
import re
import sys
import time

# Keep this list in sync with the runTest calls in HardeningRegressionTest.ino.
# Named coverage prevents a missing or duplicated test from silently passing.
EXPECTED_TESTS = frozenset(
    (
        "direct ID operations retain journal semantics",
        "invalid patch is atomic",
        "predicate reentrancy returns busy",
        "failed final sync is retryable",
        "snapshot preflight is reported and retryable",
        "timed-out final sync intent is preserved",
        "persisted size mismatch fails closed",
        "failed new model preserves manifest",
        "journal replays after failed snapshot",
        "OOM synchronization diagnostics",
        "verification failure stage and recovery",
        "allocation failure is retryable",
    )
)
SUMMARY = re.compile(r"Hardening regression summary:\s*(\d+) passed,\s*(\d+) failed")
BACKEND = re.compile(r"Hardening storage backend: (LittleFS|SDMMC)")


class HardeningRun:
    """One complete hardware run; raises ValueError for invalid output."""

    def __init__(self, expected_backend: str):
        self.expected_backend = expected_backend
        self.backend = None
        self.current_test = None
        self.results = {}
        self.complete = False

    def feed(self, line: str) -> bool:
        line = line.strip()
        if self.complete:
            raise ValueError("unexpected output after the regression summary")

        backend = BACKEND.fullmatch(line)
        if backend:
            if self.backend is not None:
                raise ValueError("duplicate storage backend announcement")
            self.backend = backend.group(1)
            if self.backend != self.expected_backend:
                raise ValueError(
                    f"wrong mounted backend: expected {self.expected_backend}, got {self.backend}"
                )
            return False

        if line.startswith("[TEST] "):
            name = line[len("[TEST] "):]
            if self.backend is None:
                raise ValueError("test started before mounted backend confirmation")
            if self.current_test is not None:
                raise ValueError(f"missing result for {self.current_test!r}")
            if name not in EXPECTED_TESTS:
                raise ValueError(f"unexpected regression test: {name!r}")
            if name in self.results:
                raise ValueError(f"duplicate regression test: {name!r}")
            self.current_test = name
            return False

        if line in ("PASS", "FAIL"):
            if self.current_test is None:
                raise ValueError(f"{line} without a preceding [TEST] line")
            self.results[self.current_test] = line == "PASS"
            self.current_test = None
            return False

        summary = SUMMARY.fullmatch(line)
        if summary:
            passed, failed = map(int, summary.groups())
            missing = EXPECTED_TESTS.difference(self.results)
            if self.backend is None or self.current_test is not None or missing:
                raise ValueError(
                    f"incomplete regression suite: missing={sorted(missing)!r}, "
                    f"unfinished={self.current_test!r}"
                )
            observed_passes = sum(self.results.values())
            observed_failures = len(self.results) - observed_passes
            if passed != observed_passes or failed != observed_failures:
                raise ValueError(
                    f"summary mismatch: reported {passed} passed/{failed} failed, "
                    f"observed {observed_passes} passed/{observed_failures} failed"
                )
            if failed:
                raise ValueError(
                    "hardware regression failures: "
                    + ", ".join(name for name, ok in self.results.items() if not ok)
                )
            self.complete = True
            return True

        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument(
        "--expected-backend",
        choices=("LittleFS", "SDMMC"),
        default="LittleFS",
        help="Require this mounted backend (specify SDMMC for Core ESP32-P4 qualification)",
    )
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial is required: python3 -m pip install pyserial", file=sys.stderr)
        return 2

    deadline = time.monotonic() + args.timeout
    run = HardeningRun(args.expected_backend)
    try:
        with serial.Serial(args.port, args.baud, timeout=1) as connection:
            print("Listening for Fresh hardening results (reset board if needed)", flush=True)
            while time.monotonic() < deadline:
                line = connection.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(line, flush=True)
                try:
                    if run.feed(line):
                        print(
                            f"Validated {len(run.results)} hardware regressions on {run.backend}",
                            flush=True,
                        )
                        return 0
                except ValueError as error:
                    print(f"Hardening validation failed: {error}", file=sys.stderr)
                    return 1
    except (OSError, serial.SerialException) as error:
        print(f"Serial test failed: {error}", file=sys.stderr)
        return 2

    print(
        f"Timed out without a complete hardening regression summary "
        f"({len(run.results)}/{len(EXPECTED_TESTS)} tests observed)",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
