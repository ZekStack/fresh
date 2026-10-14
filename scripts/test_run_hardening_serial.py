"""Fail-closed parsing tests for the hardware serial regression checker."""

import unittest

from run_hardening_serial import EXPECTED_TESTS, HardeningRun


def successful_lines(backend="SDMMC"):
    # The sketch starts the first test before initTestDatabase() reports its backend.
    first = "direct ID operations retain journal semantics"
    lines = [f"[TEST] {first}", f"Hardening storage backend: {backend}", "  PASS"]
    for name in sorted(EXPECTED_TESTS - {first}):
        lines.extend((f"[TEST] {name}", "  PASS"))
    lines.append(f"Hardening regression summary: {len(EXPECTED_TESTS)} passed, 0 failed")
    return lines


def parse(lines, backend="SDMMC"):
    run = HardeningRun(backend)
    for line in lines:
        if run.feed(line):
            return run
    return run


class HardeningSerialTests(unittest.TestCase):
    def test_complete_run_passes(self):
        run = parse(successful_lines())
        self.assertTrue(run.complete)
        self.assertEqual(len(run.results), len(EXPECTED_TESTS))

    def test_missing_test_fails(self):
        lines = successful_lines()
        del lines[3:5]
        with self.assertRaisesRegex(ValueError, "incomplete regression suite"):
            parse(lines)

    def test_duplicate_test_fails(self):
        lines = successful_lines()
        lines[3:3] = (lines[0], lines[2])
        with self.assertRaisesRegex(ValueError, "duplicate regression test"):
            parse(lines)

    def test_wrong_backend_fails(self):
        with self.assertRaisesRegex(ValueError, "wrong mounted backend"):
            parse(successful_lines("LittleFS"))

    def test_missing_outcome_fails(self):
        lines = successful_lines()
        del lines[4]
        with self.assertRaisesRegex(ValueError, "missing result"):
            parse(lines)

    def test_summary_mismatch_fails(self):
        lines = successful_lines()
        lines[-1] = "Hardening regression summary: 11 passed, 0 failed"
        with self.assertRaisesRegex(ValueError, "summary mismatch"):
            parse(lines)

    def test_reported_failure_fails(self):
        lines = successful_lines()
        lines[2] = "  FAIL"
        lines[-1] = f"Hardening regression summary: {len(EXPECTED_TESTS)-1} passed, 1 failed"
        with self.assertRaisesRegex(ValueError, "hardware regression failures"):
            parse(lines)

    def test_unknown_test_fails(self):
        lines = successful_lines()
        lines[0] = "[TEST] unexpected test name"
        with self.assertRaisesRegex(ValueError, "unexpected regression test"):
            parse(lines)

    def test_backend_before_first_test_still_passes(self):
        lines = successful_lines()
        lines.insert(0, lines.pop(1))
        self.assertTrue(parse(lines).complete)

    def test_requires_mounted_backend_before_result(self):
        lines = successful_lines()
        del lines[1]
        with self.assertRaisesRegex(ValueError, "result before mounted backend confirmation"):
            parse(lines)


if __name__ == "__main__":
    unittest.main()
