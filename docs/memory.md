# Fresh memory behavior

Fresh 0.2.0 routes Fresh-owned placement-sensitive memory and FreeRTOS ownership through Strata v0.1.2. Applications configure the default policy through `FreshConfig::memory`.

```cpp
FreshConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

The default policy is:

```cpp
Strata::MemoryPolicy{
    .allocation = Strata::Placement::PreferExternal,
    .taskStack = Strata::Placement::Internal,
};
```

`allocation` controls Fresh-owned bulk allocations that carry an instance policy. `taskStack` is the requested placement for the Fresh synchronization task. Strata owns the actual allocations and reports their observed memory region.

## JSON lifetime

Fresh stores long-lived records in ArduinoJson `JsonDocument` values. Fresh provides process-lifetime ArduinoJson allocators backed by Strata so returned documents remain valid even after the originating `Fresh` instance is destroyed.

The existing deterministic `FRESH_TESTING` allocation-failure categories remain in place. The Fresh allocation layer is therefore still responsible for classifying allocations and injecting failures in tests, while Strata is responsible for placement and ownership.

## Storage-aware sync-task placement

The synchronization task has an additional storage safety rule. `FreshConfig::memory.taskStack` expresses the requested placement, but the active storage backend may constrain the effective placement.

`FreshStorage` exposes `FreshTaskStackRequirement`:

- `Any`: the configured task-stack placement may be used;
- `Internal`: Fresh must create the sync-task stack in internal RAM.

Built-in behavior:

| Storage backend | Stack requirement | Result |
| --- | --- | --- |
| `FreshLittleFSStorage` | `Internal` | sync-task stack is always internal |
| `FreshSDStorage` | `Any` | configured placement is honored |
| `FreshEMMCStorage` | `Any` | configured placement is honored |
| custom storage | `Any` by default | backend may override the requirement |

LittleFS is deliberately constrained because internal-flash operations must not depend on a PSRAM-backed task stack while flash access is active. The safety constraint takes precedence even when the application requests `Strata::Placement::RequireExternal`; Fresh uses an internal stack instead of failing initialization.

This allows SD/eMMC applications to reclaim internal RAM safely. For example, a system using SD storage may request:

```cpp
FreshConfig config;
config.memory.taskStack = Strata::Placement::PreferExternal;

FreshSDStorage storage(sdConfig);
db.init("/fresh", config, std::move(storage));
```

## Diagnostics

`Fresh::diagnostics()` distinguishes policy from observed placement:

- `allocationPlacement`;
- `requestedSyncTaskStackPlacement`;
- `effectiveSyncTaskStackPlacement`;
- `syncTaskStackRegion`;
- `syncTaskStackConstraint`;
- `syncTaskStackHighWaterMarkBytes`;
- `backupBufferPlacement`;
- `backupBufferRegion`.

For LittleFS, a request for external task-stack memory therefore reports an internal effective placement plus `FreshTaskStackConstraint::StorageRequiresInternal`. For an unconstrained SD/eMMC backend, requested and effective placements match.

## Ownership

Fresh-owned recursive mutex control blocks, the sync-task exit semaphore, and the sync task are owned by Strata. Task shutdown keeps Fresh's existing handoff: the sync task signals its exit semaphore and suspends itself, then the controlling task releases the Strata task object. This preserves retryable timed `deinit()` and safe format/restart behavior.

The pluggable `FreshStorage` object itself remains a standard `std::unique_ptr<FreshStorage>` ownership boundary. Custom storage backends are application-facing polymorphic objects rather than Fresh's placement-sensitive internal data plane.

## Remaining optimization work

Strata integration establishes a common ownership and placement layer, but it does not require changing every public STL type. Potential later optimizations include:

- per-model memory-placement policies;
- removing the duplicate `JsonDocument` from pending journal records;
- broader instance-policy threading through temporary checkpoint/retrieval helpers;
- bounded visitor/streaming APIs.
