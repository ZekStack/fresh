# Fresh framed backup container v2

Fresh writes new backups as a bounded-memory binary container. The container is independent of the manifest, snapshot, and journal formats.

## Goals

- serialize one model record at a time;
- avoid constructing a database-sized `JsonDocument`;
- detect truncation and frame corruption before changing live state;
- keep the existing `startBackup()` / `readBackup()` streaming API;
- retain import compatibility with legacy monolithic MessagePack backup archives;
- leave room for selective model export without another format revision.

## Byte order

Every fixed-width integer is encoded little-endian. C++ structs are never written directly.

## Container header

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic bytes `FRBK` |
| 4 | 2 | Container version, currently `2` |
| 6 | 2 | Flags, currently `0` |
| 8 | 8 | Generation timestamp |
| 16 | 4 | Model count |
| 20 | 8 | Record count across every model |
| 28 | 8 | Exact total container byte count |
| 36 | 4 | FNV-1a checksum of bytes 0 through 35 |

The fixed header size is 40 bytes.

## Frame layout

Every frame has this layout:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Frame type |
| 1 | 1 | Flags, currently `0` |
| 2 | 2 | Reserved, currently `0` |
| 4 | 4 | Payload byte count |
| 8 | N | Payload |
| 8 + N | 4 | FNV-1a checksum of the frame header and payload |

Unknown frame types, flags, or reserved values are rejected.

## Frame types

### ModelBegin (`1`)

Payload:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Model type: `0` general, `1` stream |
| 1 | 1 | Reserved |
| 2 | 2 | Model-name byte count |
| 4 | 8 | Declared model record count |
| 12 | N | Model-name bytes |

### Record (`2`)

The payload is one MessagePack-encoded Fresh document or stream entry. Its frame size is checked against `FreshConfig::maxDocumentBytes` before allocation and again after decoding.

### ModelEnd (`3`)

The payload is one `uint64_t` record count. It must match both `ModelBegin` and the records actually decoded.

### ArchiveEnd (`4`)

The payload contains a `uint32_t` model count followed by a `uint64_t` total record count. A backup is incomplete unless this terminal frame is present and all header counts and the exact byte count match.

## Export consistency

Fresh first measures the exact framed size while holding the database mutex. During serialization it clones only the next record, releases the mutex, and writes that record through the configured backup ring buffer.

Model and database revisions are checked before, during, and after serialization. A concurrent mutation returns `FreshStatus::Busy` and leaves an intentionally incomplete archive without an `ArchiveEnd` frame.

Temporary export memory is bounded by:

- the configured backup ring buffer;
- one cloned record;
- model descriptors and small codec state.

## Import behavior

The importer reads and validates one frame at a time. Record payload memory is released before the next frame is read. Imported model state is prepared separately from live state, destination validators are executed, and the live registry is changed only after the complete archive has passed structural, checksum, size, count, and revision checks.

The current restore behavior replaces models present in the archive and preserves unrelated existing models.

## Legacy compatibility

If the first four bytes are not `FRBK`, Fresh attempts to decode the input as the legacy monolithic MessagePack archive with schema version `2`. Legacy imports still require archive and per-model count metadata.

Fresh never emits the legacy format after this change.
