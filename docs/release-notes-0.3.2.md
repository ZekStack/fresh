# Fresh v0.3.2 release notes

Fresh v0.3.2 is a patch release following v0.3.1. It fixes a journal-preparation
regression that caused valid mutations to fail with
`FreshStatus::InternalError: invalid prepared journal record`.

## Fixes

- Consolidate `FreshModel::FreshBuildJournalRecord` into one implementation that
  retains the validated MessagePack size in `preparedJournalBytes` before
  commit. The old inline implementation checked the size but did not store it,
  causing the commit-time preflight to reject valid writes.
- Reuse the same journal-preparation helper for `replaceById`, and apply the
  same sequence-range and prepared-size checks to direct-ID deletion.
- Preserve complete-batch prevalidation and commit-time MessagePack sequence-width
  checks without reconstructing journal JSON under the database mutex.
- Keep ordinary model and stream mutations RAM-first and persistence asynchronous;
  callers are not required to flush after each mutation.

## Regression prevention

- Add an executable native POSIX regression against production Fresh sources
  and ArduinoJson 7.4.3, covering create, update, replace, batch updates and
  deletes, stream appends, cross-model interleaving, journal size/sequence
  boundaries, background sync, and restart recovery.
- Require the native host regression in CI, including the tag-triggered release
  workflow, rather than relying only on compile-time example validation.
- Extend the hardware hardening suite to 16 named regressions, including
  mutation journal durability after restart, and update the serial test inventory.

## Compatibility

- Public APIs and persisted journal, snapshot, and backup formats are unchanged.
- Strata remains pinned to `v0.1.4`; storage backend support is unchanged.
- No per-mutation synchronous flash or SDMMC writes are introduced.

## Verification and release gate

- The bug fix and regression tests were merged in [PR #29](https://github.com/ZekStack/fresh/pull/29),
  resolving [issue #28](https://github.com/ZekStack/fresh/issues/28).
- PR #29's CI passed, including the new executable host regression.
- Compile checks and native host execution do not constitute physical hardware
  qualification. Run the documented hardening suite on real, disposable media
  and verify Core integration before publishing v0.3.2.
- Tag the merged release-preparation commit, not the earlier main commit that
  still declares v0.3.1. Do not modify or reuse an incorrectly targeted tag.
