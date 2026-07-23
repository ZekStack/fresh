# Selective backups and exact estimation

Fresh can generate either a complete framed backup or an allowlisted subset of active models.

## Options

```cpp
FreshBackupOptions options;
options.modelNames = {"User", "Hardware"};
```

`modelNames` has the following semantics:

- an empty list includes every active model;
- a non-empty list includes exactly the named models;
- names are validated when `estimateBackup()` or `startBackup()` is called;
- duplicate names return `FreshStatus::InvalidArgument`;
- missing or dropped models return `FreshStatus::ModelNotFound`;
- output order remains deterministic because selected models are serialized in registry name order;
- `startBackup()` copies the options before queueing, so later caller changes cannot alter the backup scope.

## Exact estimate

```cpp
FreshBackupEstimate estimate;
FreshResult result = db.estimateBackup(options, estimate);
if (!result) {
    Serial.println(result.message.c_str());
    return;
}

Serial.printf(
    "models=%u records=%u bytes=%u\n",
    static_cast<unsigned>(estimate.modelCount),
    static_cast<unsigned>(estimate.recordCount),
    static_cast<unsigned>(estimate.totalBytes)
);
```

`FreshBackupEstimate` contains:

| Field | Meaning |
| --- | --- |
| `totalBytes` | Exact framed archive byte count for the captured database revision. |
| `modelCount` | Number of selected active models. |
| `recordCount` | Total documents and stream entries across selected models. |

The estimate includes the container header, every frame header and checksum, model metadata, record payloads, model trailers, and the terminal archive frame.

An estimate describes the database revision observed during that call. `startBackup()` prepares a new plan when generation begins, so a mutation between estimation and backup may produce a different exact size. The `onBackupStart` callback reports the exact size of the plan actually being generated.

## Start and drain

```cpp
FreshResult started = db.startBackup(options);
if (!started) {
    Serial.println(started.message.c_str());
    return;
}

uint8_t buffer[256];
while (true) {
    size_t read = db.readBackup(buffer, sizeof(buffer), 50);
    // Persist or transmit the `read` bytes here.

    FreshBackupStatus status = db.backupStatus();
    const bool terminal = status.state == FreshBackupState::Finished ||
                          status.state == FreshBackupState::Cancelled ||
                          status.state == FreshBackupState::Error;
    if (terminal && read == 0) break;
}
```

Continue draining until a terminal state is reached and `readBackup()` returns zero. A caller that stops consuming a backup must call `cancelBackup()` so the sync task cannot remain blocked by a full ring buffer.

## Consistency

Selection does not weaken the framed archive consistency guarantees:

- all selected model names and counts are fixed in the backup plan;
- the exact byte count is measured before output;
- model and database revisions are checked during serialization;
- a conflicting mutation returns `FreshStatus::Busy` and prevents the terminal `ArchiveEnd` frame from being emitted;
- unselected model records are never measured, cloned, or serialized.
