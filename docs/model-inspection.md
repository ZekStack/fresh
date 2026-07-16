# Model inspection

Fresh exposes thread-safe model and record snapshots for administrative tooling and diagnostics.

## List models

Use `Fresh::listModels()` to enumerate active models without reading Fresh internals or filesystem metadata.

```cpp
FreshModelListResult result = db.listModels();
if (!result) {
    Serial.println(result.message.c_str());
    return;
}

for (const FreshModelInfo &model : result.models) {
    Serial.printf(
        "%s type=%s records=%u\n",
        model.name.c_str(),
        model.type == FreshModelType::Stream ? "stream" : "general",
        static_cast<unsigned>(model.recordCount)
    );
}
```

`FreshModelInfo` contains:

| Field | Meaning |
| --- | --- |
| `name` | Logical model name. |
| `type` | `FreshModelType::General` or `FreshModelType::Stream`. |
| `recordCount` | Current number of general documents or stream entries held by the model. |

`FreshModelListResult` contains:

| Field | Meaning |
| --- | --- |
| `result` | `true` when the registry snapshot was created. |
| `status` | Machine-readable `FreshStatus`. |
| `message` | Human-readable result message. |
| `models` | Model metadata ordered by model name. |
| `affectedCount` | Number of returned models. |

Dropped models are excluded. The returned list is a snapshot of the RAM-first database state taken while Fresh holds its database mutex. The method does not touch flash and does not trigger synchronization.

`listModels()` returns `FreshStatus::NotInitialized` before `init()`, `FreshStatus::Busy` while the database is stopping, and `FreshStatus::InternalError` if the database mutex cannot be acquired.

## Browse records

`FreshModel::listRecords()` returns records from both general and stream models. General documents are ordered by `_id`; stream records keep their append order.

```cpp
FreshRecordRetrieveOptions options;
options.offset = 0;
options.limit = 50;
options.reverse = false;

FreshResult page = model.listRecords(options);
if (page) {
    serializeJson(page.doc, Serial);
}
```

`FreshRecordRetrieveOptions` contains:

| Field | Meaning |
| --- | --- |
| `offset` | Number of records to skip. |
| `limit` | Maximum number of records to return. `0` means unbounded. |
| `reverse` | Traverse newest-to-oldest for streams and descending `_id` order for general models. |

The legacy `FreshStreamRetrieveOptions` name remains available as an alias of `FreshRecordRetrieveOptions`.

Use a bounded `limit` in administrative interfaces. The returned JSON array is built in memory while the model snapshot is protected by the Fresh database mutex.

## Replace a general document

`FreshModel::replaceById()` replaces the user-controlled contents of a general document. Fresh always preserves `_id` and `createdAt`, and writes a new `updatedAt` value.

```cpp
JsonDocument replacement;
replacement["name"] = "Updated name";
replacement["enabled"] = true;

FreshResult replaced = users.replaceById(documentId, replacement);
```

Replacement runs the model validator and the configured document and journal size checks. It returns the resulting document in `FreshResult::doc`.

`replaceById()` is intentionally unavailable for stream models. Stream entries remain append-only in the current storage format; changing or removing individual stream records requires a separate journal and persistence-format design.
