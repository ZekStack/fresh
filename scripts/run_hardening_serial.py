#!/usr/bin/env python3
"""Run the Fresh hardening sketch on an attached serial device.

Requires pyserial and a board flashed with HardeningRegressionTest built using
-DFRESH_TESTING. Fails closed if the summary is missing, incomplete, or reports
any failing test.
"""

import argparse
import re
import sys
import time

SUMMARY = re.compile(r"Hardening regression summary:\s*(\d+) passed,\s*(\d+) failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--minimum-tests", type=int, default=11)
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial is required: python3 -m pip install pyserial", file=sys.stderr)
        return 2

    deadline = time.monotonic() + args.timeout
    seen_tests = 0
    try:
        with serial.Serial(args.port, args.baud, timeout=1) as connection:
            print("Listening for Fresh hardening results (reset board if needed)", flush=True)
            while time.monotonic() < deadline:
                line = connection.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(line, flush=True)
                if line.startswith("[TEST] "):
                    seen_tests += 1
                match = SUMMARY.search(line)
                if match:
                    passed, failed = map(int, match.groups())
                    if failed != 0 or passed < args.minimum_tests or seen_tests < args.minimum_tests:
                        print(
                            f"Hardening tests failed or incomplete: "
                            f"{passed} passed, {failed} failed, {seen_tests} observed",
                            file=sys.stderr,
                        )
                        return 1
                    print(f"Validated {passed} runtime tests", flush=True)
                    return 0
    except (OSError, serial.SerialException) as error:
        print(f"Serial test failed: {error}", file=sys.stderr)
        return 2

    print("Timed out without a complete hardening regression summary", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
