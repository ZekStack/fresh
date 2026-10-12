#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fresh.h>

#if defined(FRESH_TESTING)
#include <internal/FreshMemory.h>
#include <internal/FreshPersistenceTesting.h>
#endif

#include <atomic>
#include <cstdint>
#include <string>
#include <esp_system.h>

#if defined(FRESH_HARDENING_SDMMC)
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#error "FRESH_HARDENING_SDMMC requires an ESP32-P4 with the Core SDMMC wiring"
#endif
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#endif

namespace {

int passed = 0;
int failed = 0;
uint32_t testSequence = 0;
uint64_t testRunId = 0;
bool backendReported = false;

std::string testPath(const char* name) {
	std::string path = "/fresh_hardening_";
	path += name;
	path += "_";
	path += std::to_string(testRunId);
	path += "_";
	path += std::to_string(++testSequence);
	return path;
}


// All reinitializations within a test use the same actual storage backend.
// With FRESH_HARDENING_SDMMC, this matches the Core ESP32-P4 SDMMC setup.
FreshResult initTestDatabase(Fresh &db, const char *path, FreshConfig config = FreshConfig()) {
#if defined(FRESH_HARDENING_SDMMC)
	config.memory.allocation = Strata::Placement::PreferExternal;
	config.memory.taskStack = Strata::Placement::PreferExternal;

	FreshSDConfig storageConfig;
	storageConfig.interface = FreshSDInterface::SDMMC;
	storageConfig.mountPath = "/sd";
	storageConfig.maxOpenFiles = 16;
	storageConfig.allocationUnitSize = 16 * 1024;
	storageConfig.formatOnMountFailure = false;
	storageConfig.sdmmc.slot = SDMMC_HOST_SLOT_0;
	storageConfig.sdmmc.oneBitMode = false;
	storageConfig.sdmmc.frequencyHz = 20'000'000;
	storageConfig.sdmmc.slotFlags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
	storageConfig.sdmmc.powerMode = FreshSDMMCPowerMode::OnChipLDO;
	storageConfig.sdmmc.ldoChannel = 4;
	storageConfig.sdmmc.clockPin = GPIO_NUM_43;
	storageConfig.sdmmc.commandPin = GPIO_NUM_44;
	storageConfig.sdmmc.data0Pin = GPIO_NUM_39;
	storageConfig.sdmmc.data1Pin = GPIO_NUM_40;
	storageConfig.sdmmc.data2Pin = GPIO_NUM_41;
	storageConfig.sdmmc.data3Pin = GPIO_NUM_42;

	FreshResult result = db.init(path, config, FreshSDStorage(storageConfig));
	const char *expectedBackend = "SDMMC";
#else
	FreshResult result = db.init(path, config);
	const char *expectedBackend = "LittleFS";
#endif

	if (result && !backendReported) {
		FreshStorageInfo info = db.storage().info();
		if (info.state != FreshStorageState::Mounted || info.name != expectedBackend) {
			(void)db.deinit(FreshDeinitOptions{.sync = false});
			return FreshResult::failure(FreshStatus::InternalError, "unexpected hardening storage backend");
		}
		Serial.print("Hardening storage backend: ");
		Serial.println(info.name.c_str());
		backendReported = true;
	}
	return result;
}

bool expect(bool condition, const char *message) {
	if (!condition) {
		Serial.print("    ");
		Serial.println(message);
	}
	return condition;
}

bool expectResult(const FreshResult &result, const char *message) {
	if (result) return true;
	Serial.print("    ");
	Serial.print(message);
	Serial.print(": ");
	Serial.println(result.message.c_str());
	return false;
}

FreshModelResult prepareModel(Fresh &db, const char* path) {
	FreshConfig config;
	config.syncIntervalMS = 60000;
	config.snapshotRecordThreshold = 1000;
	FreshResult init = initTestDatabase(db, path, config);
	if (!init) {
		Serial.print("    init failed: ");
		Serial.println(init.message.c_str());
		return {};
	}
	FreshModelResult model = db.createModel("Items");
	if (!model) return model;
	JsonDocument doc;
	doc["_id"] = "item-1";
	doc["value"] = 1;
	FreshResult created = model.model.create(doc);
	if (!created) {
		return {
		    .result = false,
		    .status = created.status,
		    .message = created.message
		};
	}
	return model;
}

bool testDirectIdMutationWithOtherDocuments() {
    const std::string path = testPath("direct_id");
    Fresh db;
    FreshModelResult items = prepareModel(db, path.c_str());
    if (!items) return false;
    for (int i = 0; i < 12; ++i) {
        JsonDocument doc;
        doc["_id"] = std::string("other-") + std::to_string(i);
        doc["payload"] = std::string(100, 'z');
        if (!expectResult(items.model.create(doc), "create neighboring record")) return false;
    }
    JsonDocument patch;
    patch["value"] = 42;
    FreshResult updated = items.model.updateById("item-1", patch, FreshReturn::ChangedDocs);
    FreshResult notUpdated = items.model.updateById("missing-id", patch, FreshReturn::None);
    FreshResult removed = items.model.deleteById("other-3");
    FreshResult missing = items.model.findById("other-3");
    FreshResult untouched = items.model.findById("other-4");
    FreshResult changed = items.model.findById("item-1");
    bool ok = expectResult(notUpdated, "missing ID update is successful no-op") &&
        expect(notUpdated.affectedCount == 0, "missing ID update count") &&
        expectResult(updated, "direct ID update") &&
        expect(updated.affectedCount == 1 && updated.doc.is<JsonArrayConst>() &&
               updated.doc.as<JsonArrayConst>().size() == 1, "changed-doc return contract") &&
        expectResult(removed, "direct ID delete") &&
        expect(removed.affectedCount == 1, "direct ID delete affected count") &&
        expect(!missing && missing.status == FreshStatus::DocumentNotFound, "deleted record remains") &&
        expectResult(untouched, "neighbor survived") &&
        expectResult(changed, "target survived") &&
        expect((changed.doc["value"] | 0) == 42, "update was not applied");
    FreshResult durable = db.forceSync();
    ok = expectResult(durable, "persist direct ID operations") && ok;
    (void)db.deinit(FreshDeinitOptions{.sync = false});
    if (!expectResult(initTestDatabase(db, path.c_str()), "reopen direct ID operations")) return false;
    FreshModel reloaded = db.model("Items");
    FreshResult afterReboot = reloaded.findById("item-1");
    FreshResult removedAfterReboot = reloaded.findById("other-3");
    ok = expectResult(afterReboot, "reloaded updated record") &&
        expect((afterReboot.doc["value"] | 0) == 42, "reloaded value mismatch") &&
        expect(!removedAfterReboot && removedAfterReboot.status == FreshStatus::DocumentNotFound,
               "deleted record reappeared") && ok;
    (void)db.deinit(FreshDeinitOptions{.sync = false});
    return ok;
}

bool testInvalidPatchIsAtomic() {
	const std::string path = testPath("invalid_patch");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items) return false;

	JsonDocument scalarPatch;
	scalarPatch.set(42);
	FreshResult update = items.model.updateById("item-1", scalarPatch);
	FreshResult found = items.model.findById("item-1");
	const bool ok = expect(!update && update.status == FreshStatus::InvalidArgument,
	                       "scalar patch was accepted") &&
	                expectResult(found, "find after rejected patch") &&
	                expect((found.doc["value"] | 0) == 1,
	                       "rejected patch changed the document");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testPredicateReentrancyReturnsBusy() {
	const std::string path = testPath("predicate");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items) return false;

	FreshResult nested;
	JsonDocument patch;
	patch["value"] = 2;
	FreshResult outer = items.model.update(
	    [&](const JsonDocument &) {
		    nested = db.renameModel("Items", "RenamedItems");
		    return true;
	    },
	    patch
	);
	FreshResult found = items.model.findById("item-1");
	const bool ok = expectResult(nested, "reentrant rename") &&
	                expect(!outer && outer.status == FreshStatus::Busy,
	                       "outer update did not report a revision conflict") &&
	                expectResult(found, "find after predicate conflict") &&
	                expect((found.doc["value"] | 0) == 1,
	                       "conflicting update partially committed") &&
	                expect(static_cast<bool>(db.model("RenamedItems")),
	                       "reentrant rename did not commit");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testFailedFinalSyncIsRetryable() {
	const std::string path = testPath("storage_full");
	FreshConfig config;
	config.syncIntervalMS = 60000;
	config.minFreeBytes = SIZE_MAX;
	Fresh db;
	if (!expectResult(initTestDatabase(db, path.c_str(), config), "init storage-full test")) return false;
	FreshModelResult items = db.createModel("Items");
	if (!items) return false;
	JsonDocument doc;
	doc["_id"] = "retryable";
	doc["value"] = 7;
	if (!expectResult(items.model.create(doc), "create retryable document")) return false;

	FreshResult firstDeinit = db.deinit(FreshDeinitOptions{.sync = true, .timeoutMS = 2000});
	FreshResult found = items.model.findById("retryable");
	const bool preserved = expect(!firstDeinit && firstDeinit.status == FreshStatus::StorageFull,
	                              "failed final sync did not return storage-full") &&
	                       expectResult(found, "RAM state was discarded after failed final sync") &&
	                       expect((found.doc["value"] | 0) == 7,
	                              "retryable RAM state changed after failed final sync");
	FreshResult forcedClose = db.deinit(FreshDeinitOptions{.sync = false, .timeoutMS = 2000});
	return preserved && expectResult(forcedClose, "retry deinit without sync");
}

bool testTimedOutFinalSyncIntentIsPreserved() {
	const std::string path = testPath("timeout");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items) return false;
	if (!expectResult(db.forceSync(), "persist shutdown timeout baseline")) return false;

	std::atomic<bool> blockFirstSync{true};
	std::atomic<bool> callbackEntered{false};
	std::atomic<bool> releaseCallback{false};
	db.onSync([&](FreshResult) {
		if (!blockFirstSync.exchange(false)) return;
		callbackEntered.store(true);
		while (!releaseCallback.load()) delay(1);
	});

	JsonDocument firstPatch;
	firstPatch["value"] = 2;
	if (!expectResult(items.model.updateById("item-1", firstPatch), "prepare first async sync batch")) {
		return false;
	}
	if (!expectResult(db.forceSyncAsync(), "start blocked background sync")) return false;

	const uint32_t callbackDeadline = millis() + 2000;
	while (!callbackEntered.load() && static_cast<int32_t>(callbackDeadline - millis()) > 0) {
		delay(1);
	}
	if (!expect(callbackEntered.load(), "background sync callback did not start")) {
		releaseCallback.store(true);
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	JsonDocument secondPatch;
	secondPatch["value"] = 3;
	if (!expectResult(items.model.updateById("item-1", secondPatch), "create data after sync capture")) {
		releaseCallback.store(true);
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	FreshResult timedOut = db.deinit(FreshDeinitOptions{.sync = true, .timeoutMS = 50});
	const bool timeoutObserved = expect(
	    !timedOut && timedOut.status == FreshStatus::Timeout,
	    "final-sync shutdown did not time out at the sync barrier"
	);
	releaseCallback.store(true);

	FreshResult retry = db.deinit(FreshDeinitOptions{.sync = false, .timeoutMS = 5000});
	const bool retrySucceeded = expectResult(
	    retry,
	    "sync=false retry did not preserve pending final sync"
	);
	if (!timeoutObserved || !retrySucceeded) return false;

	FreshConfig config;
	config.syncIntervalMS = 60000;
	if (!expectResult(initTestDatabase(db, path.c_str(), config), "reinitialize after timed-out final sync")) return false;
	FreshResult found = db.model("Items").findById("item-1");
	const bool persisted = expectResult(found, "reload data after timed-out final sync") &&
	                       expect((found.doc["value"] | 0) == 3,
	                              "sync=false retry discarded pending final sync");
	db.deinit(FreshDeinitOptions{.sync = false});
	return persisted;
}

bool testSnapshotPreflightIsReportedAndRetryable() {
	const std::string path = testPath("snapshot_preflight");
	FreshConfig config;
	config.syncIntervalMS = 60000;
	config.maxDocumentBytes = 256;
	config.maxJournalRecordBytes = 512;
	config.maxSnapshotBytes = 768;
	Fresh db;
	if (!expectResult(initTestDatabase(db, path.c_str(), config), "init snapshot preflight")) return false;
	FreshModelResult model = db.createModel("Oversized");
	if (!expect(static_cast<bool>(model), "create oversized model")) return false;

	std::string largeValue(120, 'x');
	for (int index = 0; index < 6; ++index) {
		JsonDocument doc;
		doc["_id"] = std::string("record-") + std::to_string(index);
		doc["data"] = largeValue;
		if (!expectResult(model.model.create(doc), "create snapshot record")) return false;
	}

	int legacyCallbacks = 0;
	int detailedCallbacks = 0;
	bool callbackCanInspect = false;
	FreshSyncReport last;
	db.onSync([&](FreshResult) {
		legacyCallbacks++;
		callbackCanInspect = db.diagnostics().syncAttempts > 0;
	});
	db.onSyncDetailed([&](const FreshSyncReport &report) {
		detailedCallbacks++;
		last = report;
	});
	FreshResult failed = db.forceSync();
	FreshDiagnostics diagnostics = db.diagnostics();
	const bool correctlyReported = expect(!failed && failed.status == FreshStatus::SizeLimitExceeded,
	                                      "oversized snapshot was not rejected") &&
	    expect(legacyCallbacks == 1 && detailedCallbacks == 1,
	           "preflight failure did not dispatch exactly one callback of each type") &&
	    expect(callbackCanInspect, "callback could not inspect database diagnostics") &&
	    expect(last.stage == FreshSyncStage::Preflight &&
	           last.payload == FreshSyncPayload::Snapshot &&
	           last.modelName == "Oversized" &&
	           last.actualBytes > config.maxSnapshotBytes &&
	           last.limitBytes == config.maxSnapshotBytes &&
	           !last.attemptedWrite,
	           "preflight report did not identify the oversized model") &&
	    expect(diagnostics.syncFailures == 1 && diagnostics.consecutiveSyncFailures == 1,
	           "sync diagnostics did not count preflight failure");

	for (int index = 1; index < 6; ++index) {
		const std::string id = std::string("record-") + std::to_string(index);
		if (!expectResult(model.model.deleteById(id.c_str()), "remove oversized snapshot record")) return false;
	}
	FreshResult recovered = db.forceSync();
	FreshDiagnostics recoveredDiagnostics = db.diagnostics();
	const bool ok = correctlyReported && expectResult(recovered, "retry after reducing snapshot") &&
	    expect(recoveredDiagnostics.syncSuccesses == 1 &&
	           recoveredDiagnostics.consecutiveSyncFailures == 0,
	           "successful retry did not reset failure streak");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

#if defined(FRESH_TESTING)
bool testPersistedSizeMismatchIsRejected() {
	const std::string path = testPath("size_mismatch");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items) return false;

	FreshSyncReport report;
	int callbacks = 0;
	db.onSyncDetailed([&](const FreshSyncReport &value) {
		report = value;
		callbacks++;
	});
	FreshTestInjectPersistedSizeMismatch();
	FreshResult failed = db.forceSync();
	const bool rejected = expect(!failed && failed.status == FreshStatus::InternalError,
	                             "injected serialization size mismatch was accepted") &&
	    expect(callbacks == 1 && report.stage == FreshSyncStage::SnapshotWrite &&
	           report.payload == FreshSyncPayload::Snapshot &&
	           report.modelName == "Items" &&
	           report.actualBytes != report.expectedBytes &&
	           !report.attemptedWrite,
	           "size mismatch failure was not reported with observed bytes");
	FreshResult retry = db.forceSync();
	const bool recovered = expectResult(retry, "retry after injected size mismatch");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	FreshConfig config;
	config.syncIntervalMS = 60000;
	if (!expectResult(initTestDatabase(db, path.c_str(), config), "reopen after mismatch retry")) return false;
	FreshResult found = db.model("Items").findById("item-1");
	const bool durable = expectResult(found, "recover item after injected size mismatch") &&
	                     expect((found.doc["value"] | 0) == 1, "recovered document is incorrect");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	return rejected && recovered && durable;
}


bool testFailedNewModelSnapshotPreservesCommittedManifest() {
	const std::string path = testPath("manifest_baseline");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items || !expectResult(db.forceSync(), "persist baseline manifest")) return false;

	FreshModelResult staged = db.createModel("Staged");
	if (!expect(static_cast<bool>(staged), "create staged model")) return false;
	FreshSyncReport report;
	db.onSyncDetailed([&](const FreshSyncReport &value) { report = value; });
	FreshTestInjectPersistedSizeMismatch();
	FreshResult failed = db.forceSync();
	const bool failureCorrect = expect(!failed && failed.status == FreshStatus::InternalError,
	                                  "new-model snapshot mismatch was not rejected") &&
	    expect(report.stage == FreshSyncStage::SnapshotWrite &&
	           report.modelName == "Staged" && !report.attemptedWrite,
	           "new model was written despite serialization mismatch");
	(void)db.deinit(FreshDeinitOptions{.sync = false});

	FreshConfig config;
	config.syncIntervalMS = 60000;
	if (!expectResult(initTestDatabase(db, path.c_str(), config), "reopen previous manifest")) return false;
	FreshResult baseline = db.model("Items").findById("item-1");
	const bool recovered = expectResult(baseline, "recovered previously committed model") &&
	    expect((baseline.doc["value"] | 0) == 1, "committed document changed on failed snapshot") &&
	    expect(!static_cast<bool>(db.model("Staged")), "uncommitted model appeared after reboot");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	return failureCorrect && recovered;
}

bool testJournalReplayAfterFailedSnapshotWithoutRetry() {
	const std::string path = testPath("journal_replay");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items || !expectResult(db.forceSync(), "persist journal replay baseline")) return false;

	JsonDocument patch;
	patch["value"] = 2;
	if (!expectResult(items.model.updateById("item-1", patch), "stage journal replay update")) return false;
	FreshSyncReport report;
	db.onSyncDetailed([&](const FreshSyncReport &value) { report = value; });
	FreshTestInjectPersistedSizeMismatch();
	FreshResult failed = db.forceSync();
	const bool failureCorrect = expect(!failed && failed.status == FreshStatus::InternalError,
	                                  "failed snapshot was not reported") &&
	    expect(report.stage == FreshSyncStage::SnapshotWrite &&
	           report.payload == FreshSyncPayload::Snapshot &&
	           report.modelName == "Items" && !report.attemptedWrite,
	           "failed snapshot was incorrectly reported as a completed write");
	(void)db.deinit(FreshDeinitOptions{.sync = false});

	FreshConfig config;
	config.syncIntervalMS = 60000;
	if (!expectResult(initTestDatabase(db, path.c_str(), config), "reopen durable journal")) return false;
	FreshResult replayed = db.model("Items").findById("item-1");
	const bool durable = expectResult(replayed, "replay journal after snapshot failure") &&
	    expect((replayed.doc["value"] | 0) == 2,
	           "durable journal record was not replayed after snapshot failure");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	return failureCorrect && durable;
}


bool testVerificationFailureIsReportedAfterWrite() {
	const std::string path = testPath("verification_failure");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items || !expectResult(db.forceSync(), "persist verification baseline")) return false;

	JsonDocument patch;
	patch["value"] = 4;
	if (!expectResult(items.model.updateById("item-1", patch), "stage verification update")) return false;
	FreshSyncReport report;
	db.onSyncDetailed([&](const FreshSyncReport &value) { report = value; });
	FreshTestInjectDurableVerificationFailure();
	FreshResult failed = db.forceSync();
	const bool correctlyReported = expect(!failed && failed.status == FreshStatus::FileSystemError,
	                                      "verification fault did not fail synchronization") &&
	    expect(report.stage == FreshSyncStage::Verification &&
	           report.payload == FreshSyncPayload::Snapshot &&
	           report.modelName == "Items" && report.attemptedWrite,
	           "verification failure was not classified after durable write");
	(void)db.deinit(FreshDeinitOptions{.sync = false});

	FreshConfig config;
	config.syncIntervalMS = 60000;
	if (!expectResult(initTestDatabase(db, path.c_str(), config), "reopen after verification fault")) return false;
	FreshResult found = db.model("Items").findById("item-1");
	const bool recovered = expectResult(found, "read verified snapshot after injected failure") &&
	    expect((found.doc["value"] | 0) == 4, "persisted update missing after verification fault");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	return correctlyReported && recovered;
}

bool testOutOfMemorySyncDiagnostics() {
	const std::string path = testPath("sync_allocation");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items) return false;

	int legacyCallbacks = 0;
	int detailedCallbacks = 0;
	FreshStatus observed = FreshStatus::Ok;
	db.onSync([&](FreshResult result) {
		legacyCallbacks++;
		observed = result.status;
	});
	db.onSyncDetailed([&](const FreshSyncReport &report) {
		detailedCallbacks++;
		if (report.status != FreshStatus::OutOfMemory) observed = report.status;
	});
	FreshTestConfigureAllocationFailure(1, FreshAllocationCategory::DurableSlotPayload, 0, true);
	FreshResult failure = db.forceSync();
	FreshTestResetAllocationFailure();
	FreshDiagnostics diagnostics = db.diagnostics();
	const bool diagnosed = expect(!failure && failure.status == FreshStatus::OutOfMemory,
	                             "durable payload allocation failure was not reported") &&
	    expect(legacyCallbacks == 1 && detailedCallbacks == 1 && observed == FreshStatus::OutOfMemory,
	           "allocation failure was not delivered once to both callbacks") &&
	    expect(diagnostics.syncFailures == 1 && diagnostics.consecutiveSyncFailures == 1 &&
	           diagnostics.lastSyncFailure.status == FreshStatus::OutOfMemory &&
	           diagnostics.lastSyncFailure.modelName == "Items",
	           "allocation failure diagnostics were lost");
	FreshResult retry = db.forceSync();
	const bool retried = expectResult(retry, "retry after durable buffer allocation failure") &&
	    expect(db.diagnostics().consecutiveSyncFailures == 0,
	           "successful retry did not clear out-of-memory failure streak");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	return diagnosed && retried;
}

bool testAllocationFailureIsRetryable() {
	const std::string path = testPath("allocation");
	Fresh db;
	FreshModelResult items = prepareModel(db, path.c_str());
	if (!items) return false;

	JsonDocument patch;
	patch["value"] = 9;
	FreshTestConfigureAllocationFailure(
	    1,
	    FreshAllocationCategory::JsonCloneBuffer,
	    0,
	    true
	);
	FreshResult failedUpdate = items.model.updateById("item-1", patch);
	FreshTestResetAllocationFailure();
	FreshResult unchanged = items.model.findById("item-1");
	FreshResult retry = items.model.updateById("item-1", patch);
	FreshResult changed = items.model.findById("item-1");
	const bool ok = expect(!failedUpdate && failedUpdate.status == FreshStatus::OutOfMemory,
	                       "injected allocation failure was not reported") &&
	                expect(failedUpdate.message.find("update target clone buffer (bytes=") != std::string::npos,
	                       "clone error did not identify failed stage and payload size") &&
	                expectResult(unchanged, "find after injected failure") &&
	                expect((unchanged.doc["value"] | 0) == 1,
	                       "injected failure partially committed") &&
	                expectResult(retry, "retry after allocation failure") &&
	                expectResult(changed, "find after allocation retry") &&
	                expect((changed.doc["value"] | 0) == 9,
	                       "retry did not commit the update");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}
#endif

void runTest(const char *name, bool (*test)()) {
	Serial.print("[TEST] ");
	Serial.println(name);
	if (test()) {
		passed++;
		Serial.println("  PASS");
	} else {
		failed++;
		Serial.println("  FAIL");
	}
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(500);
#if defined(FRESH_HARDENING_SDMMC)
	// Match Core's active-low TF-card power gate. Never autoformat a test card.
	if (gpio_set_level(GPIO_NUM_45, 0) != ESP_OK ||
	    gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT) != ESP_OK ||
	    gpio_set_level(GPIO_NUM_45, 0) != ESP_OK) {
		Serial.println("Failed to enable the SDMMC TF-card power gate");
		return;
	}
	if (!psramFound()) {
		Serial.println("Core-equivalent ESP32-P4 PSRAM is unavailable");
		return;
	}
#endif
	// Stable for reopen tests, unpredictable across boots: never reuse prior test data.
	testRunId = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
	if (testRunId == 0) testRunId = 1;
	Serial.printf("Hardening run ID: %llu\n", static_cast<unsigned long long>(testRunId));

	runTest("direct ID operations retain journal semantics", testDirectIdMutationWithOtherDocuments);
	runTest("invalid patch is atomic", testInvalidPatchIsAtomic);
	runTest("predicate reentrancy returns busy", testPredicateReentrancyReturnsBusy);
	runTest("failed final sync is retryable", testFailedFinalSyncIsRetryable);
	runTest("snapshot preflight is reported and retryable", testSnapshotPreflightIsReportedAndRetryable);
	runTest("timed-out final sync intent is preserved", testTimedOutFinalSyncIntentIsPreserved);
#if defined(FRESH_TESTING)
	runTest("persisted size mismatch fails closed", testPersistedSizeMismatchIsRejected);
	runTest("failed new model preserves manifest", testFailedNewModelSnapshotPreservesCommittedManifest);
	runTest("journal replays after failed snapshot", testJournalReplayAfterFailedSnapshotWithoutRetry);
	runTest("OOM synchronization diagnostics", testOutOfMemorySyncDiagnostics);
	runTest("verification failure stage and recovery", testVerificationFailureIsReportedAfterWrite);
	runTest("allocation failure is retryable", testAllocationFailureIsRetryable);
#endif

	Serial.printf("Hardening regression summary: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
