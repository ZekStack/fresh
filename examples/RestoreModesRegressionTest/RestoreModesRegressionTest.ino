#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

#include <string>
#include <vector>

namespace {

constexpr const char *SourcePath = "/fresh_restore_source";
constexpr const char *EmptySourcePath = "/fresh_restore_empty_source";
constexpr const char *DefaultPath = "/fresh_restore_default";
constexpr const char *FullPath = "/fresh_restore_full";
constexpr const char *CollisionPath = "/fresh_restore_collision";
constexpr const char *OptionsPath = "/fresh_restore_options";
constexpr const char *ValidationPath = "/fresh_restore_validation";
constexpr const char *EmptySelectedPath = "/fresh_restore_empty_selected";
constexpr const char *EmptyAllPath = "/fresh_restore_empty_all";
constexpr const char *LegacyPath = "/fresh_restore_legacy";
constexpr const char *ArchiveFile = "/fresh_restore_modes_archive.bin";

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

bool drainBackup(Fresh &db, std::vector<uint8_t> &archive, uint32_t timeoutMS = 5000) {
	archive.clear();
	uint8_t buffer[101];
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

bool collectBackup(
    Fresh &db,
    const std::vector<std::string> &modelNames,
    std::vector<uint8_t> &archive
) {
	FreshBackupOptions options;
	options.modelNames = modelNames;
	return expectResult(db.startBackup(options), "start backup") &&
	       expect(drainBackup(db, archive), "backup did not finish");
}

bool createGeneral(
    Fresh &db,
    const char *modelName,
    const char *id,
    const char *value,
    bool allowed = true
) {
	FreshModelResult model = db.createModel(modelName);
	if (!model) return false;
	JsonDocument doc;
	doc["_id"] = id;
	doc["value"] = value;
	doc["allowed"] = allowed;
	return expectResult(model.model.create(doc), "create general document");
}

bool prepareArchives(
    std::vector<uint8_t> &selectedArchive,
    std::vector<uint8_t> &protectedArchive,
    std::vector<uint8_t> &emptyArchive
) {
	FreshConfig config;
	config.backupBufferSize = 128;
	config.syncIntervalMS = 60000;

	Fresh source;
	if (!expectResult(source.init(SourcePath, config), "source init")) return false;
	if (!createGeneral(source, "Config", "config-new", "new", false)) return false;
	FreshModelResult logResult = source.createModel("EventLog", FreshModelType::Stream);
	if (!logResult) return false;
	JsonDocument entry;
	entry["message"] = "restored event";
	if (!expectResult(logResult.model.append(entry), "append source event")) return false;
	if (!createGeneral(source, "Protected", "protected-archive", "archive")) return false;

	const bool selectedReady = collectBackup(source, {"Config", "EventLog"}, selectedArchive);
	const bool protectedReady = selectedReady && collectBackup(source, {"Protected"}, protectedArchive);
	source.deinit(FreshDeinitOptions{.sync = false});
	if (!protectedReady) return false;

	Fresh empty;
	if (!expectResult(empty.init(EmptySourcePath, config), "empty source init")) return false;
	const bool emptyReady = collectBackup(empty, {}, emptyArchive);
	empty.deinit(FreshDeinitOptions{.sync = false});
	return emptyReady;
}

bool verifySelectedState(Fresh &db) {
	FreshResult config = db.model("Config").findById("config-new");
	FreshResult keep = db.model("Keep").findById("keep-local");
	FreshResult events = db.model("EventLog").retrieve();
	return expectResult(config, "restored Config missing") &&
	       expectResult(keep, "unrelated Keep model missing") &&
	       expectResult(events, "restored EventLog missing") &&
	       expect(events.affectedCount == 1, "restored stream count mismatch") &&
	       expect(!db.model("Config").findById("config-old"), "old Config record survived replacement");
}

bool testDefaultReplaceSelected(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!expectResult(db.init(DefaultPath), "default destination init")) return false;
	if (!createGeneral(db, "Config", "config-old", "old") ||
	    !createGeneral(db, "Keep", "keep-local", "keep")) {
		return false;
	}

	FreshResult restored = db.backupImport(archive.data(), archive.size());
	const bool ok = expectResult(restored, "default restore") &&
	                expect(restored.affectedCount == 2, "default affected count mismatch") &&
	                verifySelectedState(db);
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool writeArchiveFile(const std::vector<uint8_t> &archive) {
	LittleFS.remove(ArchiveFile);
	File file = LittleFS.open(ArchiveFile, "w");
	if (!file) return false;
	const size_t written = file.write(archive.data(), archive.size());
	file.flush();
	file.close();
	return written == archive.size();
}

bool verifyFullState(Fresh &db) {
	return expectResult(db.model("Config").findById("config-new"), "full Config missing") &&
	       expectResult(db.model("Protected").findById("protected-local"), "protected model missing") &&
	       expectResult(db.model("EventLog").retrieve(), "full EventLog missing") &&
	       expect(!static_cast<bool>(db.model("RemoveMe")), "unprotected model survived ReplaceAll");
}

bool testReplaceAllProtectedAndPersistence(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!expectResult(db.init(FullPath), "full destination init")) return false;
	if (!createGeneral(db, "Config", "config-old", "old") ||
	    !createGeneral(db, "RemoveMe", "remove-local", "remove") ||
	    !createGeneral(db, "Protected", "protected-local", "local")) {
		return false;
	}
	if (!expect(writeArchiveFile(archive), "failed to write archive file")) return false;
	File file = LittleFS.open(ArchiveFile, "r");
	if (!expect(static_cast<bool>(file), "failed to reopen archive file")) return false;

	FreshRestoreOptions options;
	options.mode = FreshRestoreMode::ReplaceAll;
	options.protectedModels = {"Protected"};
	FreshResult restored = db.backupImport(file, options);
	file.close();
	if (!expectResult(restored, "ReplaceAll restore") ||
	    !expect(restored.affectedCount == 3, "ReplaceAll affected count mismatch") ||
	    !verifyFullState(db) ||
	    !expectResult(db.forceSync(), "persist restored registry")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});

	Fresh reopened;
	if (!expectResult(reopened.init(FullPath), "reopen restored database")) return false;
	const bool ok = verifyFullState(reopened);
	reopened.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testProtectedCollision(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!expectResult(db.init(CollisionPath), "collision destination init")) return false;
	if (!createGeneral(db, "Protected", "protected-local", "local") ||
	    !createGeneral(db, "Keep", "keep-local", "keep")) {
		return false;
	}

	FreshRestoreOptions options;
	options.mode = FreshRestoreMode::ReplaceAll;
	options.protectedModels = {"Protected"};
	FreshResult restored = db.backupImport(archive.data(), archive.size(), options);
	const bool ok = expect(!restored && restored.status == FreshStatus::InvalidArgument,
	                       "protected archive collision was accepted") &&
	                expectResult(db.model("Protected").findById("protected-local"),
	                             "protected state changed after collision") &&
	                expectResult(db.model("Keep").findById("keep-local"),
	                             "unrelated state changed after collision");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testOptionValidation(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!expectResult(db.init(OptionsPath), "options destination init")) return false;
	if (!createGeneral(db, "Protected", "protected-local", "local") ||
	    !createGeneral(db, "Keep", "keep-local", "keep")) {
		return false;
	}

	FreshRestoreOptions invalidName;
	invalidName.protectedModels = {"bad/name"};
	FreshRestoreOptions duplicate;
	duplicate.protectedModels = {"Protected", "Protected"};
	FreshRestoreOptions missing;
	missing.protectedModels = {"Missing"};
	FreshRestoreOptions invalidMode;
	invalidMode.mode = static_cast<FreshRestoreMode>(99);

	FreshResult invalidNameResult = db.backupImport(archive.data(), archive.size(), invalidName);
	FreshResult duplicateResult = db.backupImport(archive.data(), archive.size(), duplicate);
	FreshResult missingResult = db.backupImport(archive.data(), archive.size(), missing);
	FreshResult invalidModeResult = db.backupImport(archive.data(), archive.size(), invalidMode);

	const bool ok = expect(!invalidNameResult && invalidNameResult.status == FreshStatus::InvalidArgument,
	                       "invalid protected name was accepted") &&
	                expect(!duplicateResult && duplicateResult.status == FreshStatus::InvalidArgument,
	                       "duplicate protected name was accepted") &&
	                expect(!missingResult && missingResult.status == FreshStatus::ModelNotFound,
	                       "missing protected model was accepted") &&
	                expect(!invalidModeResult && invalidModeResult.status == FreshStatus::InvalidArgument,
	                       "invalid restore mode was accepted") &&
	                expectResult(db.model("Keep").findById("keep-local"),
	                             "option failure changed destination state");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testValidatorRollback(const std::vector<uint8_t> &archive) {
	Fresh db;
	if (!expectResult(db.init(ValidationPath), "validation destination init")) return false;
	FreshModelResult model = db.createModel("Config");
	if (!model) return false;
	if (!expectResult(
	        model.model.setValidator([](const JsonDocument &doc) {
		        return FreshValidationResult{
		            .result = doc["allowed"] | false,
		            .message = "allowed must be true",
		        };
	        }),
	        "set restore validator"
	    )) return false;
	JsonDocument existing;
	existing["_id"] = "config-old";
	existing["value"] = "old";
	existing["allowed"] = true;
	if (!expectResult(model.model.create(existing), "create validated existing record")) return false;
	if (!createGeneral(db, "Keep", "keep-local", "keep")) return false;

	FreshResult restored = db.backupImport(archive.data(), archive.size());
	const bool ok = expect(!restored && restored.status == FreshStatus::ValidationFailed,
	                       "validator did not reject restore") &&
	                expectResult(db.model("Config").findById("config-old"),
	                             "failed restore replaced validated model") &&
	                expectResult(db.model("Keep").findById("keep-local"),
	                             "failed restore changed unrelated model") &&
	                expect(!static_cast<bool>(db.model("EventLog")),
	                       "failed restore created later archive model");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testEmptyArchiveModes(const std::vector<uint8_t> &archive) {
	Fresh selected;
	if (!expectResult(selected.init(EmptySelectedPath), "empty selected init")) return false;
	if (!createGeneral(selected, "Keep", "keep-local", "keep")) return false;
	FreshResult selectedResult = selected.backupImport(archive.data(), archive.size());
	const bool selectedOk = expectResult(selectedResult, "empty ReplaceSelected") &&
	                        expect(selectedResult.affectedCount == 0, "empty selected affected count mismatch") &&
	                        expectResult(selected.model("Keep").findById("keep-local"),
	                                     "empty ReplaceSelected removed model");
	selected.deinit(FreshDeinitOptions{.sync = false});
	if (!selectedOk) return false;

	Fresh all;
	if (!expectResult(all.init(EmptyAllPath), "empty all init")) return false;
	if (!createGeneral(all, "Keep", "keep-local", "keep") ||
	    !createGeneral(all, "Protected", "protected-local", "local")) {
		return false;
	}
	FreshRestoreOptions options;
	options.mode = FreshRestoreMode::ReplaceAll;
	options.protectedModels = {"Protected"};
	FreshResult allResult = all.backupImport(archive.data(), archive.size(), options);
	const bool allOk = expectResult(allResult, "empty ReplaceAll") &&
	                   expect(allResult.affectedCount == 1, "empty ReplaceAll affected count mismatch") &&
	                   expect(!static_cast<bool>(all.model("Keep")), "empty ReplaceAll preserved unprotected model") &&
	                   expectResult(all.model("Protected").findById("protected-local"),
	                                "empty ReplaceAll removed protected model");
	all.deinit(FreshDeinitOptions{.sync = false});
	return allOk;
}

std::vector<uint8_t> makeLegacyArchive() {
	JsonDocument legacy;
	legacy["version"] = 2;
	legacy["generatedAt"] = 0;
	legacy["modelCount"] = 1;
	JsonArray models = legacy["models"].to<JsonArray>();
	JsonObject model = models.add<JsonObject>();
	model["name"] = "Legacy";
	model["type"] = "general";
	model["recordCount"] = 1;
	JsonArray docs = model["docs"].to<JsonArray>();
	JsonObject doc = docs.add<JsonObject>();
	doc["_id"] = "legacy-new";
	doc["value"] = "legacy";

	std::vector<uint8_t> bytes(measureMsgPack(legacy));
	serializeMsgPack(legacy, bytes.data(), bytes.size());
	return bytes;
}

bool testLegacyReplaceAll() {
	const std::vector<uint8_t> archive = makeLegacyArchive();
	Fresh db;
	if (!expectResult(db.init(LegacyPath), "legacy destination init")) return false;
	if (!createGeneral(db, "RemoveMe", "remove-local", "remove")) return false;

	FreshRestoreOptions options;
	options.mode = FreshRestoreMode::ReplaceAll;
	FreshResult restored = db.backupImport(archive.data(), archive.size(), options);
	const bool ok = expectResult(restored, "legacy ReplaceAll") &&
	                expect(restored.affectedCount == 2, "legacy affected count mismatch") &&
	                expectResult(db.model("Legacy").findById("legacy-new"), "legacy model missing") &&
	                expect(!static_cast<bool>(db.model("RemoveMe")), "legacy ReplaceAll preserved old model");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

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
	Serial.println("Fresh RestoreModesRegressionTest starting");
	if (!LittleFS.begin(true)) {
		Serial.println("[FAIL] LittleFS mount");
		return;
	}

	const char *paths[] = {
	    SourcePath,
	    EmptySourcePath,
	    DefaultPath,
	    FullPath,
	    CollisionPath,
	    OptionsPath,
	    ValidationPath,
	    EmptySelectedPath,
	    EmptyAllPath,
	    LegacyPath,
	};
	for (const char *path : paths) removeTree(path);
	LittleFS.remove(ArchiveFile);

	std::vector<uint8_t> selectedArchive;
	std::vector<uint8_t> protectedArchive;
	std::vector<uint8_t> emptyArchive;
	if (!prepareArchives(selectedArchive, protectedArchive, emptyArchive)) {
		Serial.println("[FAIL] prepare restore archives");
		return;
	}

	runTest("default import remains ReplaceSelected", testDefaultReplaceSelected(selectedArchive));
	runTest("ReplaceAll protects models and persists", testReplaceAllProtectedAndPersistence(selectedArchive));
	runTest("protected archive collision is atomic", testProtectedCollision(protectedArchive));
	runTest("restore option validation", testOptionValidation(selectedArchive));
	runTest("destination validator rollback", testValidatorRollback(selectedArchive));
	runTest("empty archive mode semantics", testEmptyArchiveModes(emptyArchive));
	runTest("legacy ReplaceAll semantics", testLegacyReplaceAll());

	LittleFS.remove(ArchiveFile);
	Serial.printf("RestoreModesRegressionTest complete: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
