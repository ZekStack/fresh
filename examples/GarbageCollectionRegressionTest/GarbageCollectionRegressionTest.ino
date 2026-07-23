#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

#if defined(FRESH_TESTING)
#include <FreshGarbageCollectionTesting.h>
#endif

#include <cstring>
#include <string>

namespace {

constexpr const char *StartupPath = "/fresh_gc_startup";
constexpr const char *NoManifestPath = "/fresh_gc_no_manifest";
constexpr const char *FailurePath = "/fresh_gc_failure";

constexpr const char *StartupOrphanId = "1111111111111111";
constexpr const char *ManualOrphanId = "2222222222222222";
constexpr const char *NoManifestOrphanId = "3333333333333333";
constexpr const char *FailureOrphanId = "4444444444444444";
constexpr const char *UnknownDirectory = "application-data";
constexpr const char *UnknownFile = "5555555555555555";

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
		if (childPath.empty() || childPath.front() != '/') {
			childPath = joinPath(path, childPath);
		}
		const bool directory = child.isDirectory();
		child.close();
		if (directory) removeTree(childPath);
		else LittleFS.remove(childPath.c_str());
		child = root.openNextFile();
	}
	root.close();
	LittleFS.rmdir(path.c_str());
}

bool ensureDirectory(const std::string &path) {
	return LittleFS.exists(path.c_str()) || LittleFS.mkdir(path.c_str());
}

bool writeFile(const std::string &path, const char *content = "orphan") {
	File file = LittleFS.open(path.c_str(), "w");
	if (!file) return false;
	const size_t length = strlen(content);
	const size_t written = file.write(
	    reinterpret_cast<const uint8_t *>(content),
	    length
	);
	file.flush();
	file.close();
	return written == length;
}

bool createCandidateDirectory(const char *rootPath, const char *storageId) {
	const std::string models = joinPath(rootPath, "models");
	const std::string candidate = joinPath(models, storageId);
	return ensureDirectory(rootPath) &&
	       ensureDirectory(models) &&
	       ensureDirectory(candidate) &&
	       writeFile(joinPath(candidate, "payload.bin"));
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

bool createPersistentModel(Fresh &db) {
	FreshModelResult model = db.createModel("Config");
	if (!model) return false;
	JsonDocument document;
	document["_id"] = "config";
	document["value"] = "preserved";
	return expectResult(model.model.create(document), "create persistent document") &&
	       expectResult(db.forceSync(), "persist database");
}

bool verifyPersistentModel(Fresh &db) {
	FreshResult found = db.model("Config").findById("config");
	return expectResult(found, "active model was removed") &&
	       expect(std::string(found.doc["value"] | "") == "preserved",
	              "active model content changed");
}

bool testStartupAndManualCollection() {
	Fresh initial;
	if (!expectResult(initial.init(StartupPath), "startup database init")) return false;
	if (!createPersistentModel(initial)) return false;
	initial.deinit(FreshDeinitOptions{.sync = false});

	const std::string models = joinPath(StartupPath, "models");
	const std::string startupOrphan = joinPath(models, StartupOrphanId);
	const std::string unknownDirectory = joinPath(models, UnknownDirectory);
	const std::string unknownFile = joinPath(models, UnknownFile);
	if (!expect(createCandidateDirectory(StartupPath, StartupOrphanId),
	            "create startup orphan") ||
	    !expect(ensureDirectory(unknownDirectory), "create unknown directory") ||
	    !expect(writeFile(joinPath(unknownDirectory, "keep.txt"), "keep"),
	            "write unknown directory file") ||
	    !expect(writeFile(unknownFile, "keep"), "create storage-shaped file")) {
		return false;
	}

	Fresh reopened;
	FreshResult initialized = reopened.init(StartupPath);
	if (!expectResult(initialized, "reopen database")) return false;
	FreshDiagnostics diagnostics = reopened.diagnostics();
	if (!verifyPersistentModel(reopened) ||
	    !expect(!LittleFS.exists(startupOrphan.c_str()),
	            "startup orphan was not removed") ||
	    !expect(LittleFS.exists(unknownDirectory.c_str()),
	            "unknown directory was removed") ||
	    !expect(LittleFS.exists(unknownFile.c_str()),
	            "storage-shaped file was removed") ||
	    !expect(diagnostics.garbageCollection.attempted,
	            "startup collection was not attempted") ||
	    !expect(!diagnostics.garbageCollection.degraded,
	            "startup collection was degraded") ||
	    !expect(diagnostics.garbageCollection.result.removedDirectories == 1,
	            "startup removed count mismatch") ||
	    !expect(diagnostics.garbageCollection.result.reclaimedBytes > 0,
	            "startup reclaimed-byte count is empty")) {
		reopened.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	const std::string manualOrphan = joinPath(models, ManualOrphanId);
	if (!expect(createCandidateDirectory(StartupPath, ManualOrphanId),
	            "create manual orphan")) {
		reopened.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	FreshGarbageCollectionResult manual;
	FreshResult collected = reopened.collectGarbage(manual);
	if (!expectResult(collected, "manual garbage collection") ||
	    !expect(manual.removedDirectories == 1, "manual removed count mismatch") ||
	    !expect(manual.failedDirectories == 0, "manual collection reported failure") ||
	    !expect(manual.reclaimedBytes > 0, "manual reclaimed-byte count is empty") ||
	    !expect(!LittleFS.exists(manualOrphan.c_str()), "manual orphan survived") ||
	    !verifyPersistentModel(reopened)) {
		reopened.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	FreshGarbageCollectionResult second;
	FreshResult secondResult = reopened.collectGarbage(second);
	const bool ok = expectResult(secondResult, "idempotent garbage collection") &&
	                expect(second.removedDirectories == 0,
	                       "idempotent collection removed storage") &&
	                expect(second.failedDirectories == 0,
	                       "idempotent collection reported failure");
	reopened.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testNoManifestSafety() {
	const std::string candidate = joinPath(
	    joinPath(NoManifestPath, "models"),
	    NoManifestOrphanId
	);
	if (!expect(createCandidateDirectory(NoManifestPath, NoManifestOrphanId),
	            "create no-manifest candidate")) {
		return false;
	}

	Fresh db;
	FreshResult initialized = db.init(NoManifestPath);
	if (!expectResult(initialized, "no-manifest database init")) return false;
	FreshDiagnostics diagnostics = db.diagnostics();
	const bool ok = expect(LittleFS.exists(candidate.c_str()),
	                       "candidate without manifest was removed") &&
	                expect(!diagnostics.garbageCollection.attempted,
	                       "no-manifest collection was marked attempted") &&
	                expect(!diagnostics.garbageCollection.degraded,
	                       "no-manifest collection was degraded");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testFailureDiagnostics() {
#if defined(FRESH_TESTING)
	Fresh initial;
	if (!expectResult(initial.init(FailurePath), "failure database init")) return false;
	if (!createPersistentModel(initial)) return false;
	initial.deinit(FreshDeinitOptions{.sync = false});

	const std::string candidate = joinPath(
	    joinPath(FailurePath, "models"),
	    FailureOrphanId
	);
	if (!expect(createCandidateDirectory(FailurePath, FailureOrphanId),
	            "create failure candidate")) {
		return false;
	}

	FreshTestConfigureGarbageCollectionFailure(
	    FreshGarbageCollectionTestFailurePoint::BeforeCandidateRemoval
	);
	Fresh reopened;
	FreshResult initialized = reopened.init(FailurePath);
	FreshTestResetGarbageCollectionFailure();
	if (!expectResult(initialized, "init with cleanup failure")) return false;

	FreshDiagnostics diagnostics = reopened.diagnostics();
	if (!expect(diagnostics.garbageCollection.attempted,
	            "failed collection was not attempted") ||
	    !expect(diagnostics.garbageCollection.degraded,
	            "cleanup failure was not reported") ||
	    !expect(diagnostics.garbageCollection.result.failedDirectories == 1,
	            "failed-directory count mismatch") ||
	    !expect(LittleFS.exists(candidate.c_str()),
	            "injected failure removed candidate") ||
	    !verifyPersistentModel(reopened)) {
		reopened.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	FreshGarbageCollectionResult retry;
	FreshResult retried = reopened.collectGarbage(retry);
	const bool ok = expectResult(retried, "retry garbage collection") &&
	                expect(retry.removedDirectories == 1,
	                       "retry did not remove candidate") &&
	                expect(!LittleFS.exists(candidate.c_str()),
	                       "candidate survived retry");
	reopened.deinit(FreshDeinitOptions{.sync = false});
	return ok;
#else
	return true;
#endif
}

void runTest(const char *name, bool (*test)()) {
	Serial.print("  ");
	Serial.print(name);
	Serial.print(": ");
	if (test()) {
		Serial.println("PASS");
		passed++;
	} else {
		Serial.println("FAIL");
		failed++;
	}
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(500);

	if (!LittleFS.begin(true)) {
		Serial.println("LittleFS mount failed");
		return;
	}
	removeTree(StartupPath);
	removeTree(NoManifestPath);
	removeTree(FailurePath);

	Serial.println("Fresh garbage collection regression");
	runTest("startup and manual collection", testStartupAndManualCollection);
	runTest("no-manifest safety", testNoManifestSafety);
	runTest("failure diagnostics", testFailureDiagnostics);

	Serial.printf("Result: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
