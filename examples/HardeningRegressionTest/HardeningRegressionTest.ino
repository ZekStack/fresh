#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fresh.h>

#if defined(FRESH_TESTING)
#include <internal/FreshMemory.h>
#include <internal/FreshPersistenceTesting.h>
#endif

#include <atomic>
#include <string>

namespace {

int passed = 0;
int failed = 0;
uint32_t testSequence = 0;

std::string testPath(const char* name) {
	std::string path = "/fresh_hardening_";
	path += name;
	path += "_";
	path += std::to_string(++testSequence);
	return path;
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
	FreshResult init = db.init(path, config);
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
	if (!expectResult(db.init(path.c_str(), config), "init storage-full test")) return false;
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
	if (!expectResult(db.init(path.c_str(), config), "reinitialize after timed-out final sync")) return false;
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
	if (!expectResult(db.init(path.c_str(), config), "init snapshot preflight")) return false;
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
	           last.limitBytes == config.maxSnapshotBytes,
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
	           report.actualBytes != report.expectedBytes,
	           "size mismatch failure was not reported with observed bytes");
	FreshResult retry = db.forceSync();
	const bool recovered = expectResult(retry, "retry after injected size mismatch");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	FreshConfig config;
	config.syncIntervalMS = 60000;
	if (!expectResult(db.init(path.c_str(), config), "reopen after mismatch retry")) return false;
	FreshResult found = db.model("Items").findById("item-1");
	const bool durable = expectResult(found, "recover item after injected size mismatch") &&
	                     expect((found.doc["value"] | 0) == 1, "recovered document is incorrect");
	(void)db.deinit(FreshDeinitOptions{.sync = false});
	return rejected && recovered && durable;
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

	runTest("invalid patch is atomic", testInvalidPatchIsAtomic);
	runTest("predicate reentrancy returns busy", testPredicateReentrancyReturnsBusy);
	runTest("failed final sync is retryable", testFailedFinalSyncIsRetryable);
	runTest("snapshot preflight is reported and retryable", testSnapshotPreflightIsReportedAndRetryable);
	runTest("timed-out final sync intent is preserved", testTimedOutFinalSyncIntentIsPreserved);
#if defined(FRESH_TESTING)
	runTest("persisted size mismatch fails closed", testPersistedSizeMismatchIsRejected);
	runTest("allocation failure is retryable", testAllocationFailureIsRetryable);
#endif

	Serial.printf("Hardening regression summary: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
