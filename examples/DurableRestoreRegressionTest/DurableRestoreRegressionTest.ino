#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

#if defined(FRESH_TESTING)
#include <FreshRestoreTesting.h>
#endif

#include <string>
#include <vector>

namespace {

constexpr const char *SourcePath = "/fresh_durable_source";
constexpr const char *EmptySourcePath = "/fresh_durable_empty_source";
constexpr const char *DurablePath = "/fresh_durable_destination";
constexpr const char *ValidationPath = "/fresh_durable_validation";
constexpr const char *EmptyPath = "/fresh_durable_empty";
constexpr const char *BeforeCommitPath = "/fresh_durable_before_commit";
constexpr const char *AfterCommitPath = "/fresh_durable_after_commit";

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

bool addDocument(Fresh &db, const char *modelName, const char *id, const char *value, bool allowed = true) {
	FreshModelResult model = db.createModel(modelName);
	if (!model) return false;
	JsonDocument document;
	document["_id"] = id;
	document["value"] = value;
	document["allowed"] = allowed;
	return expectResult(model.model.create(document), "create document");
}

bool drainBackup(Fresh &db, std::vector<uint8_t> &archive, uint32_t timeoutMS = 5000) {
	archive.clear();
	uint8_t buffer[97];
	const uint32_t started = millis();
	while (millis() - started < timeoutMS) {
		const size_t read = db.readBackup(buffer, sizeof(buffer), 25);
		archive.insert(archive.end(), buffer, buffer + read);
		const FreshBackupStatus status = db.backupStatus();
		const bool terminal = status.state == FreshBackupState::Finished ||
		                      status.state == FreshBackupState::Cancelled ||
		                      status.state == FreshBackupState::Error;
		if (terminal && read == 0) {
			return status.state == FreshBackupState::Finished && static_cast<bool>(status.result);
		}
	}
	db.cancelBackup();
	return false;
}

bool collectBackup(Fresh &db, const std::vector<std::string> &models, std::vector<uint8_t> &archive) {
	FreshBackupOptions options;
	options.modelNames = models;
	return expectResult(db.startBackup(options), "start backup") &&
	       expect(drainBackup(db, archive), "backup did not finish");
}

bool prepareArchives(std::vector<uint8_t> &archive, std::vector<uint8_t> &emptyArchive) {
	FreshConfig config;
	config.backupBufferSize = 128;
	config.syncIntervalMS = 60000;

	Fresh source;
	if (!expectResult(source.init(SourcePath, config), "source init")) return false;
	if (!addDocument(source, "Config", "config-new", "new", false)) return false;
	FreshModelResult stream = source.createModel("EventLog", FreshModelType::Stream);
	if (!stream) return false;
	JsonDocument entry;
	entry["message"] = "restored event";
	if (!expectResult(stream.model.append(entry), "append source stream")) return false;
	const bool archiveReady = collectBackup(source, {"Config", "EventLog"}, archive);
	source.deinit(FreshDeinitOptions{.sync = false});
	if (!archiveReady) return false;

	Fresh empty;
	if (!expectResult(empty.init(EmptySourcePath, config), "empty source init")) return false;
	const bool emptyReady = collectBackup(empty, {}, emptyArchive);
	empty.deinit(FreshDeinitOptions{.sync = false});
	return emptyReady;
}

bool verifyRestored(Fresh &db, bool expectProtected) {
	FreshResult config = db.model("Config").findById("config-new");
	FreshResult events = db.model("EventLog").retrieve();
	const bool protectedOk = !expectProtected ||
	                         static_cast<bool>(db.model("Protected").findById("protected-local"));
	return expectResult(config, "restored Config missing") &&
	       expectResult(events, "restored stream missing") &&
	       expect(events.affectedCount == 1, "restored stream count mismatch") &&
	       expect(protectedOk, "protected model missing") &&
	       expect(!static_cast<bool>(db.model("RemoveMe")), "removed model survived durable restore") &&
	       expect(!db.model("Config").findById("config-old"), "old Config record survived replacement");
}

bool verifyOriginal(Fresh &db) {
	return expectResult(db.model("Config").findById("config-old"), "original Config missing") &&
	       expectResult(db.model("RemoveMe").findById("remove-local"), "original RemoveMe missing") &&
	       expect(!db.model("Config").findById("config-new"), "uncommitted Config became visible");
}

bool seedDestination(Fresh &db, const char *path, bool withProtected = true) {
	if (!expectResult(db.init(path), "destination init")) return false;
	if (!addDocument(db, "Config", "config-old", "old") ||
	    !addDocument(db, "RemoveMe", "remove-local", "remove")) {
		return false;
	}
	return !withProtected || addDocument(db, "Protected", "protected-local", "local");
}

FreshRestoreOptions replaceAllOptions(bool protect = true) {
	FreshRestoreOptions options;
	options.mode = FreshRestoreMode::ReplaceAll;
	if (protect) options.protectedModels = {"Protected"};
	return options;
}

bool testDurableReplaceAll(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!seedDestination(db, DurablePath)) return false;
	FreshResult restored = db.restoreBackup(archive.data(), archive.size(), replaceAllOptions());
	if (!expectResult(restored, "durable ReplaceAll") ||
	    !expect(restored.affectedCount == 3, "durable affected count mismatch") ||
	    !verifyRestored(db, true)) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	// No forceSync is required: restoreBackup committed the manifest and snapshots.
	db.deinit(FreshDeinitOptions{.sync = false});
	Fresh reopened;
	if (!expectResult(reopened.init(DurablePath), "reopen durable database")) return false;
	const bool ok = verifyRestored(reopened, true);
	reopened.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testValidatorFailure(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!expectResult(db.init(ValidationPath), "validation init")) return false;
	FreshModelResult config = db.createModel("Config");
	if (!config) return false;
	if (!expectResult(
	        config.model.setValidator([](const JsonDocument &document) {
		        return FreshValidationResult{
		            .result = document["allowed"] | false,
		            .message = "allowed must be true",
		        };
	        }),
	        "set validator"
	    )) return false;
	JsonDocument old;
	old["_id"] = "config-old";
	old["value"] = "old";
	old["allowed"] = true;
	if (!expectResult(config.model.create(old), "create validated record") ||
	    !addDocument(db, "RemoveMe", "remove-local", "remove")) {
		return false;
	}

	FreshResult restored = db.restoreBackup(archive.data(), archive.size(), replaceAllOptions(false));
	const bool ok = expect(!restored && restored.status == FreshStatus::ValidationFailed,
	                       "validator accepted durable restore") &&
	                verifyOriginal(db);
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testEmptyReplaceAll(const std::vector<uint8_t> &emptyArchive) {
	Fresh db;
	if (!seedDestination(db, EmptyPath)) return false;
	FreshResult restored = db.restoreBackup(emptyArchive.data(), emptyArchive.size(), replaceAllOptions());
	if (!expectResult(restored, "empty durable ReplaceAll") ||
	    !expect(restored.affectedCount == 2, "empty durable affected count mismatch") ||
	    !expect(!static_cast<bool>(db.model("Config")), "Config survived empty ReplaceAll") ||
	    !expect(!static_cast<bool>(db.model("RemoveMe")), "RemoveMe survived empty ReplaceAll") ||
	    !expectResult(db.model("Protected").findById("protected-local"), "protected model was removed")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});

	Fresh reopened;
	if (!expectResult(reopened.init(EmptyPath), "reopen empty durable database")) return false;
	const bool ok = expect(!static_cast<bool>(reopened.model("Config")), "reopened Config survived") &&
	                expect(!static_cast<bool>(reopened.model("RemoveMe")), "reopened RemoveMe survived") &&
	                expectResult(reopened.model("Protected").findById("protected-local"),
	                             "reopened protected model missing");
	reopened.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

#if defined(FRESH_TESTING)
bool testFailureBeforeManifest(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!seedDestination(db, BeforeCommitPath, false)) return false;
	FreshTestConfigureRestoreFailure(FreshRestoreTestFailurePoint::BeforeManifestCommit);
	FreshResult restored = db.restoreBackup(archive.data(), archive.size(), replaceAllOptions(false));
	if (!expect(!restored, "injected pre-manifest restore succeeded") || !verifyOriginal(db)) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});
	Fresh reopened;
	if (!expectResult(reopened.init(BeforeCommitPath), "reopen pre-manifest database")) return false;
	const bool ok = verifyOriginal(reopened);
	reopened.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testFailureAfterManifest(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!seedDestination(db, AfterCommitPath, false)) return false;
	FreshTestConfigureRestoreFailure(FreshRestoreTestFailurePoint::AfterManifestCommit);
	FreshResult restored = db.restoreBackup(archive.data(), archive.size(), replaceAllOptions(false));
	if (!expect(!restored, "injected post-manifest restore succeeded")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	// Simulate a reset without allowing the old RAM registry to persist again.
	db.deinit(FreshDeinitOptions{.sync = false});
	Fresh reopened;
	if (!expectResult(reopened.init(AfterCommitPath), "reopen post-manifest database")) return false;
	const bool ok = verifyRestored(reopened, false);
	reopened.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}
#endif

void runTest(const char *name, bool result) {
	if (result) {
		passed++;
		Serial.print("[PASS] ");
	} else {
		failed++;
		Serial.print("[FAIL] ");
	}
	Serial.println(name);
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(1000);
	Serial.println();
	Serial.println("Fresh DurableRestoreRegressionTest starting");
	if (!LittleFS.begin(true)) {
		Serial.println("[FAIL] LittleFS mount");
		return;
	}

	for (const char *path : {
	         SourcePath,
	         EmptySourcePath,
	         DurablePath,
	         ValidationPath,
	         EmptyPath,
	         BeforeCommitPath,
	         AfterCommitPath,
	     }) {
		removeTree(path);
	}

	std::vector<uint8_t> archive;
	std::vector<uint8_t> emptyArchive;
	if (!prepareArchives(archive, emptyArchive)) {
		Serial.println("[FAIL] prepare archives");
		return;
	}

	runTest("durable ReplaceAll and restart", testDurableReplaceAll(archive));
	runTest("validator failure preserves original database", testValidatorFailure(archive));
	runTest("empty durable ReplaceAll", testEmptyReplaceAll(emptyArchive));
#if defined(FRESH_TESTING)
	runTest("failure before manifest keeps original database", testFailureBeforeManifest(archive));
	runTest("failure after manifest loads restored database", testFailureAfterManifest(archive));
#endif

	Serial.printf("DurableRestoreRegressionTest complete: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
