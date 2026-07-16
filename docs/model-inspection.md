# Model inspection

Fresh exposes a thread-safe model registry snapshot through `Fresh::listModels()`.

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

## Result types

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
