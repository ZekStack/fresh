# Segmented snapshots: follow-up design roadmap

This is a **future format migration**, not part of the backward-compatible
persistence diagnostics change. Fresh v0.2.1 serializes every record in a model
into one bounded checkpoint slot (256 KiB by default, 1 MiB absolute), so valid
small documents can exceed the model checkpoint limit as a collection grows.

## Goals

- Bounded memory and I/O per checkpoint segment; never require materializing
  a complete large model checkpoint as a single MessagePack buffer.
- Preserve the acknowledged-durable journal operations through interruptions.
- Keep the old snapshot generation until a complete replacement is committed.
- Support both General and Stream models without unbounded memory growth.
- Make failures actionable: model, segment, size, generation and recovery stage.

## Proposed on-disk evolution

1. Introduce a versioned checkpoint descriptor with generation, storage ID,
   model type, applied-through journal sequence, record count, segment count,
   per-segment checksums and a checksum for the descriptor itself.
2. Write bounded immutable segment files using a new generation ID. The format
   must define deterministic order and retain stable record identifiers.
3. Sync and verify every segment **before** publishing the descriptor. Keep
   both the previous committed descriptor and current journal replay path until
   the new descriptor is durably committed and verified.
4. After commit, remove only journal records covered by the committed sequence;
   garbage-collect unused segments in a separately retryable step.
5. On startup, select the newest fully valid generation; ignore incomplete
   generations, then replay the journal from its checkpoint sequence. Recover
   using the prior generation if a later descriptor or segment is corrupt.
6. Support the v0.2.1 single-slot snapshot reader for migration; never delete
   its snapshot until the new generation is verified. Version the descriptor
   and test both downgrade policy and forward compatibility explicitly.

## Before implementation

- Specify exact segment filenames, size accounting, transaction boundaries,
  sequence-number semantics and crash-recovery state transitions.
- Decide the limit for a single document and how an oversized single record is
  rejected before it is acknowledged as durable.
- Define reader/writer behavior under concurrent create/update/delete/append.
- Audit the RAM-first pending-journal semantics under checkpoint failures.
- Add an executable runtime test target, not only compile-only Arduino examples.

## Required fault-injection tests

Test power loss after each segment write, sync, descriptor write, descriptor
verification, manifest commit, journal truncation and garbage-collection step.
Also test checksum corruption, short writes, out-of-memory, storage-full,
concurrent mutation, format-version migration, failed rollback and restart.
Use production-compatible SDMMC/PSRAM hardware for release qualification.

Do not raise the 1 MiB format guard as a shortcut. Increasing the bound alone
would increase peak allocations and still leave large models uncheckpointable.
