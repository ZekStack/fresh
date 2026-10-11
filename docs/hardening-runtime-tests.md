# Hardening regression runtime validation

The CI build matrix compiles `examples/HardeningRegressionTest` for supported
targets and builds it again with `-DFRESH_TESTING`. Compilation is **not**
execution. Persisted-data recovery tests require a real mounted storage backend.

## Hardware prerequisites

- ESP32 test device with the same flash/SDMMC/eMMC storage type used in production.
- Fresh and its Strata dependency installed at the versions used by this branch.
- ArduinoJson, Arduino ESP32 core, and C++20 support as configured in CI.
- A writable, disposable test volume. Regression sketches create model data.
- Python 3 and `pyserial` for the serial result checker.

## Execute the regression sketch

Build and upload `examples/HardeningRegressionTest` with `-DFRESH_TESTING`
using the board and dependencies specified in `.github/workflows/ci.yml`.
For a sketchbook configured like the CI Arduino CLI environment:

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32p4 \
  --build-property "compiler.cpp.extra_flags=-std=gnu++20 -DFRESH_TESTING" \
  --output-dir ./build-hardening \
  examples/HardeningRegressionTest

arduino-cli upload \
  --fqbn esp32:esp32:esp32p4 \
  --port /dev/ttyACM0 \
  --input-dir ./build-hardening \
  examples/HardeningRegressionTest

python3 -m pip install pyserial
python3 scripts/run_hardening_serial.py \
  --port /dev/ttyACM0 \
  --minimum-tests 11
```

Replace the board and serial port for the connected hardware. Start the serial
checker before resetting the board if opening the serial port does not reset it.
The checker requires the complete `Hardening regression summary` and returns
nonzero for failures, missing tests, serial errors, and timeouts. A build-only
CI check must never be presented as a runtime pass.

## Recovery scenarios

The `FRESH_TESTING` build covers:

- Snapshot-size preflight rejection and successful retry
- Preflight and retry callbacks, counters and in-memory diagnostics
- Serialize-size mismatch before file open
- Reopen without retry after a failed new model snapshot
- Journal replay without retry after a failed snapshot
- Durable payload allocation failure and diagnostic preservation
- Post-write verification failure classification and reopening
- Existing final-sync shutdown and mutation-atomicity regressions

For release qualification, run on an ESP32-P4 with the production-equivalent
SDMMC and PSRAM configuration, and record the board, Arduino core version,
storage backend, test log, and result. Where production uses other storage
backends, repeat on representative hardware. This is an explicit release gate;
the repository does not currently have a provisioned hardware CI runner.

## Interpretation

`attemptedWrite=false` applies to the reported failing phase; a journal may
already have been durably appended before a later snapshot fails. A valid journal
record is expected to survive reopening and be replayed. `Verification`
with `attemptedWrite=true` means bytes were written and synchronized but
read-back validation failed or could not be completed. Do not infer rollback
from the synchronization failure alone.
