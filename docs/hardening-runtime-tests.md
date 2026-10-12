# Hardening regression runtime validation

The CI build matrix compiles `examples/HardeningRegressionTest` for supported
targets and compiles the fault-injection variant for both ESP32 and ESP32-P4
(Core-compatible SDMMC). **Compilation is not execution.** Recovery tests must
run on real mounted storage. The serial checker requires all 12 named
`FRESH_TESTING` regressions and a runtime-confirmed backend.

## Hardware prerequisites

- An ESP32-P4 with the same SDMMC slot, card power wiring and PSRAM as Core,
  or an ESP32 with a disposable LittleFS test partition for generic coverage.
- A writable **disposable** FAT32 SD card for SDMMC testing. Do not use a card
  that contains user data. Mount-time formatting is disabled.
- Fresh and Strata at the versions specified for this branch; ArduinoJson,
  Arduino ESP32 core and C++20 according to `.github/workflows/ci.yml`.
- Python 3 and `pyserial` for the serial result checker.

## Core ESP32-P4 SDMMC qualification

Build `examples/HardeningRegressionTest` with **both**
`-DFRESH_TESTING` and `-DFRESH_HARDENING_SDMMC`. The latter switches every
initialization, including reopen/replay checks, to `FreshSDStorage` configured
for Core's ESP32-P4 hardware:

- SDMMC host slot 0, four-bit bus, 20 MHz, internal pull-up flags.
- GPIO 43 (clock), 44 (command) and 39–42 (data 0–3).
- On-chip LDO channel 4 and the active-low GPIO 45 card-power gate.
- `/sd` mount, 16 open files and 16 KiB allocation units.
- `Strata::Placement::PreferExternal` for model allocations and sync-task stack,
  as in Core. Ensure the board actually has operational PSRAM.
- `formatOnMountFailure = false`; a mount failure must fail the test, not erase
  or reformat a device.

On hardware with **different** wiring, adapt the test-only SDMMC configuration
before running; do not claim Core-equivalent validation for a different setup.

With the required Arduino CLI installation and sketchbook dependencies:

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32p4 \
  --build-property "compiler.cpp.extra_flags=-std=gnu++20 -DFRESH_TESTING -DFRESH_HARDENING_SDMMC" \
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
  --expected-backend SDMMC
```

Start the checker before resetting the device when opening the port does not
reset it. The sketch prints the backend name from `db.storage().info()` after
a successful mount. The checker rejects LittleFS results when SDMMC is expected,
as well as missing/duplicate tests, missing results, counters that disagree
with observed outcomes, failures and timeouts.

## Generic LittleFS testing

For an ESP32 with a dedicated LittleFS test partition, build with
`-DFRESH_TESTING` only (omit `-DFRESH_HARDENING_SDMMC`). The checker defaults
to `--expected-backend LittleFS`. LittleFS results are useful additional
coverage but **do not** qualify Core's production SDMMC setup.

The sketch generates a fresh 64-bit randomized run ID at boot, included in
every test database root, to avoid contamination by previously committed
snapshots or journals. Reopens **within the same test** reuse that root.
This isolation does not free old directories: repeated runs consume capacity.
Use a dedicated disposable card/partition, and clear or reformat it **manually
only after verifying it contains no other data**. Fresh never automatically
formats the media for these tests.

## Recovery scenarios

The 12 `FRESH_TESTING` regressions cover:

- Direct-ID update/delete and reopen durability
- Invalid-patch atomicity and predicate revision conflicts
- Failed final sync retry and timed-out shutdown preservation
- Snapshot-size preflight rejection and recovery
- Preflight callbacks, counters and in-memory diagnostics
- Serialize-size mismatch before file open
- Reopen without retry after a failed new-model snapshot
- Journal replay without retry after a failed snapshot
- Durable payload allocation failure and diagnostics
- Post-write verification failure classification and recovery
- Memory allocation failure and mutation retry

Record the board, Arduino core version, backend and media details, PSRAM status,
test log and outcome for each hardware qualification. The repository has no
provisioned hardware CI runner; a green build does not replace this release gate.

## Interpretation

`attemptedWrite=false` applies to the reported failing phase; a journal may
already have been durably appended before a later snapshot fails. A valid
journal record must survive reopening and replay. `Verification` with
`attemptedWrite=true` means bytes were written and synchronized but read-back
validation failed or could not be completed. Do not infer rollback from the
synchronization failure alone.
