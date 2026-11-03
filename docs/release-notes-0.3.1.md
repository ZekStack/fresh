# Fresh v0.3.1 release notes

Fresh v0.3.1 is a patch release following v0.3.0. It addresses false
`FreshStatus::Busy` results when mutations of unrelated models interleave.

## Fixes

- Reserve journal sequence numbers at the RAM commit boundary under the database
  mutex, rather than comparing a global pending sequence captured before
  validation. Cross-model writes no longer invalidate unrelated mutations.
- Continue to reject genuine same-model revision conflicts with
  `FreshStatus::Busy`.
- Prevalidate complete mutation batches before changing RAM, assign contiguous
  journal sequence ranges, and account for MessagePack integer-width growth
  before commit. Oversized records fail without partially committing a batch.
- Keep normal mutations RAM-first. Journal persistence continues in the
  background sync task without waiting for flash or SDMMC writes.

## Compatibility

- Public APIs and persisted journal/snapshot formats are unchanged.
- The Strata dependency remains pinned to `v0.1.4`.
- Existing storage backend support is unchanged.

## Verification

PR #26 added regression coverage for interleaved cross-model writes,
same-model conflicts, MessagePack sequence-width boundaries, and persistence
by the background synchronization task without an explicit application flush.

The CI release workflow builds examples for ESP32, ESP32-S3, ESP32-C3, and
ESP32-P4 using PIOArduino and Arduino CLI. Hardware runtime qualification
remains a separate step; successful hardware execution is not claimed here.
