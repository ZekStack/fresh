# Backup inspection and validation

Fresh can consume a complete backup archive, validate it, and return metadata without changing live database state.

## API

```cpp
FreshBackupMetadata metadata;
FreshResult result = db.inspectBackup(input, metadata);
```

Memory-backed input is also supported:

```cpp
FreshResult result = db.inspectBackup(data, length, metadata);
```

The database must be initialized and running. Inspection uses the configured `FreshConfig::maxDocumentBytes` limit.

## Returned metadata

`FreshBackupMetadata` contains:

| Field | Meaning |
| --- | --- |
| `containerVersion` | Framed or legacy backup schema version. |
| `generatedAt` | Timestamp stored by the archive. |
| `totalBytes` | Exact number of consumed archive bytes. |
| `modelCount` | Number of models in the archive. |
| `recordCount` | Total records across all models. |
| `legacyFormat` | `true` for the legacy monolithic MessagePack archive. |
| `models` | Ordered model name, type, and record-count metadata. |

Metadata is assigned only after successful inspection. A failed inspection clears the output object so callers cannot accidentally use partial results.

## What inspection validates

For framed `FRBK` archives, Fresh validates:

- magic, version, flags, exact byte count, and header checksum;
- frame types, flags, reserved fields, payload sizes, and frame checksums;
- strict `ModelBegin` / `Record` / `ModelEnd` / `ArchiveEnd` ordering;
- valid and unique model names;
- declared and observed model and record counts;
- terminal archive frame presence;
- MessagePack decoding for every record;
- object-shaped records;
- non-empty unique `_id` values for general-model documents;
- configured document-size limits;
- immediately available trailing bytes after the declared archive.

Legacy monolithic schema-version-2 archives receive the equivalent metadata, count, model, record, document-ID, object-shape, size-limit, and trailing-byte validation.

## Stream behavior

`inspectBackup(Stream&)` consumes the supplied stream and does not rewind it. A typical upload workflow is:

1. write the upload to a temporary file;
2. open the file and call `inspectBackup()`;
3. show the returned metadata to the user;
4. close and reopen the file after restore confirmation;
5. pass the reopened stream to `backupImport()`.

Do not assume arbitrary Arduino `Stream` implementations are seekable.

## Memory behavior

Framed inspection does not create imported model states. Temporary memory is bounded by:

- the metadata model list;
- one frame payload;
- one decoded `JsonDocument`;
- a set of document IDs for the current general model;
- small parser and checksum state.

Legacy compatibility still requires deserializing the legacy monolithic archive because that format has no independently framed records.

## Shared parser

Inspection and import use the same internal visitor-based archive parser. The inspection visitor discards decoded records after validation. The import visitor transfers validated records into pending replacement model states and performs the existing atomic live-registry commit only after the entire archive succeeds.

Inspection never modifies models, validators, revisions, manifests, journals, snapshots, or storage files.
