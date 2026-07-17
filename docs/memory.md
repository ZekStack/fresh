# Fresh memory behavior

Fresh stores long-lived records in ArduinoJson `JsonDocument` values. On ESP32 targets with PSRAM, those document pools now use a process-lifetime allocator that prefers external RAM and falls back to internal 8-bit capable memory.

This first memory-optimization milestone covers:

- long-lived JSON clones created by Fresh model operations;
- the backup ring and persisted-byte buffers through the same allocation helper;
- removal of the serialize/deserialize round trip previously used by `FreshCloneJson()`.

The allocator has process lifetime so result documents remain valid after the originating `Fresh` instance is destroyed.

## Fallback behavior

Fresh checks whether a PSRAM heap is present. Allocation order is:

1. `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`;
2. `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`;
3. any `MALLOC_CAP_8BIT` heap.

Boards without PSRAM therefore retain the previous internal-memory behavior.

## Remaining work

The following optimizations are intentionally kept as follow-up work on the draft PR branch:

- per-model memory-placement policies;
- removing the duplicate `JsonDocument` from pending journal records;
- PSRAM-backed checkpoint and retrieval documents throughout every storage path;
- bounded visitor/streaming APIs;
- public memory diagnostics and sync-task stack measurements.
