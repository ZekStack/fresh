#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fresh.h>
#include <LittleFS.h>

#if defined(FRESH_TESTING)
#include <internal/FreshMemory.h>
#endif

#include <string>

namespace {

constexpr const char *TestPath = "/fresh_hardening_regression";
int passed = 0;
int failed = 0;

std::string joinPath(const std::string &base, const std::string &name) {
	if (base.empty() || base == "/") return "/" + name;
	return base.back() == '/' ? base + name : base + "/" + name;
}

void removeTree(const std::string &path) {
	File root = LittleFS.open(path.c_str(), "r");
	if (!root) return;
	if (!root.isDirectory()) {
		root.close();
		LittleFS.remove(path.c_str());
		return;
	}
	File child = root.openNextFile();
	while (child) {
		std::string childPath = child.name();
		if (childPath.empty() || childPath.front() != '/') childPath = joinPath(path, childPath);
		const bool directory = child.isDirectory();
		child.close();
		if (directory) removeTree(childPath);
		else LittleFS.remove(childPath.c_str());
		child = root.openNextFile();
	}
	root.close();
	LittleFS.rmdir(path.c_str());
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

FreshModelResult prepareModel(Fresh &db) {
	FreshConfig config;
	config.syncIntervalMS = 60000;
	config.snapshotRecordThreshold = 1000;
	FreshResult init = db.init(TestPath, config);
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
	removeTree(TestPath);
	Fresh db;
	FreshModelResult items = prepareModel(db);
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
	removeTree(TestPath);
	Fresh db;
	FreshModelResult items = prepareModel(db);
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
	removeTree(TestPath);
	FreshConfig config;
	config.syncIntervalMS = 60000;
	config.minFreeBytes = LittleFS.totalBytes() + 1;
	Fresh db;
	if (!expectResult(db.init(TestPath, config), "init storage-full test")) return false;
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

#if defined(FRESH_TESTING)
bool testAllocationFailureIsRetryable() {
	removeTree(TestPath);
	Fresh db;
	FreshModelResult items = prepareModel(db);
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
	LittleFS.begin(true);

	runTest("invalid patch is atomic", testInvalidPatchIsAtomic);
	runTest("predicate reentrancy returns busy", testPredicateReentrancyReturnsBusy);
	runTest("failed final sync is retryable", testFailedFinalSyncIsRetryable);
#if defined(FRESH_TESTING)
	runTest("allocation failure is retryable", testAllocationFailureIsRetryable);
#endif

	Serial.printf("Hardening regression summary: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
