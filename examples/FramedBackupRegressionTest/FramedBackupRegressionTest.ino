#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fresh.h>
#include <LittleFS.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *SourcePath = "/fresh_framed_source";
constexpr const char *RestorePath = "/fresh_framed_restore";
constexpr const char *CorruptPath = "/fresh_framed_corrupt";
constexpr const char *LegacyPath = "/fresh_framed_legacy";
constexpr const char *ValidationPath = "/fresh_framed_validation";

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

bool collectBackup(Fresh &db, std::vector<uint8_t> &archive, uint32_t timeoutMs = 5000) {
	archive.clear();
	if (!expectResult(db.startBackup(), "start backup")) return false;
	uint8_t chunk[97];
	const uint32_t started = millis();
	while (millis() - started < timeoutMs) {
		const size_t count = db.readBackup(chunk, sizeof(chunk), 50);
		archive.insert(archive.end(), chunk, chunk + count);
		FreshBackupStatus status = db.backupStatus();
		if (status.state == FreshBackupState::Finished) {
			while (true) {
				const size_t tail = db.readBackup(chunk, sizeof(chunk), 0);
				if (tail == 0) break;
				archive.insert(archive.end(), chunk, chunk + tail);
			}
			return expect(static_cast<bool>(status.result), "backup status failed") &&
			       expect(status.result.affectedCount == archive.size(), "backup byte count mismatch");
		}
		if (status.state == FreshBackupState::Cancelled || status.state == FreshBackupState::Error) {
			Serial.print("    backup failed: ");
			Serial.println(status.result.message.c_str());
			return false;
		}
	}
	db.cancelBackup();
	return expect(false, "backup timed out");
}

bool prepareSource(Fresh &source) {
	FreshConfig config;
	config.backupBufferSize = 256;
	config.syncIntervalMS = 60000;
	if (!expectResult(source.init(SourcePath, config), "source init")) return false;
	FreshModelResult users = source.createModel("Users");
	FreshModelResult logs = source.createModel("Logs", FreshModelType::Stream);
	FreshModelResult validated = source.createModel("Validated");
	if (!users || !logs || !validated) return expect(false, "source model creation failed");

	for (int i = 0; i < 24; ++i) {
		JsonDocument user;
		char id[24];
		snprintf(id, sizeof(id), "user-%02d", i);
		user["_id"] = id;
		user["name"] = String("User ") + i;
		user["index"] = i;
		user["payload"] = "abcdefghijklmnopqrstuvwxyz0123456789";
		if (!expectResult(users.model.create(user), "create source user")) return false;

		JsonDocument log;
		log["index"] = i;
		log["message"] = String("entry-") + i;
		if (!expectResult(logs.model.append(log), "append source log")) return false;
	}

	JsonDocument invalidForDestination;
	invalidForDestination["_id"] = "validation-record";
	invalidForDestination["allowed"] = false;
	return expectResult(validated.model.create(invalidForDestination), "create validation source record");
}

bool verifyRestored(Fresh &db) {
	FreshResult user = db.model("Users").findById("user-17");
	FreshResult logs = db.model("Logs").retrieve();
	FreshResult kept = db.model("Keep").findById("keep-me");
	return expectResult(user, "find restored user") &&
	       expect((user.doc["index"] | -1) == 17, "restored user mismatch") &&
	       expectResult(logs, "retrieve restored stream") &&
	       expect(logs.affectedCount == 24, "restored stream count mismatch") &&
	       expectResult(kept, "unselected existing model was invalidated");
}

bool testFramedExportImport(const std::vector<uint8_t> &archive) {
	removeTree(RestorePath);
	Fresh restore;
	if (!expectResult(restore.init(RestorePath), "restore init")) return false;
	FreshModelResult keep = restore.createModel("Keep");
	if (!keep) return false;
	JsonDocument existing;
	existing["_id"] = "keep-me";
	existing["value"] = 42;
	if (!expectResult(keep.model.create(existing), "create preserved record")) return false;

	const bool headerOk = archive.size() >= 4 && archive[0] == 'F' && archive[1] == 'R' &&
	                      archive[2] == 'B' && archive[3] == 'K';
	FreshResult imported = restore.backupImport(archive.data(), archive.size());
	const bool ok = expect(headerOk, "new backup is not an FRBK container") &&
	                expectResult(imported, "framed backup import") &&
	                expectResult(restore.forceSync(), "restore force sync") &&
	                verifyRestored(restore);
	restore.deinit();
	return ok;
}

bool testCorruptionAndTruncation(const std::vector<uint8_t> &archive) {
	removeTree(CorruptPath);
	Fresh db;
	if (!expectResult(db.init(CorruptPath), "corrupt-test init")) return false;

	std::vector<uint8_t> corrupt = archive;
	if (corrupt.size() > 52) corrupt[52] ^= 0x5a;
	FreshResult corruptResult = db.backupImport(corrupt.data(), corrupt.size());

	std::vector<uint8_t> truncated = archive;
	if (truncated.size() > 8) truncated.resize(truncated.size() - 7);
	FreshResult truncatedResult = db.backupImport(truncated.data(), truncated.size());

	const bool ok = expect(!corruptResult && corruptResult.status == FreshStatus::CorruptData,
	                       "corrupt frame was accepted") &&
	                expect(!truncatedResult && truncatedResult.status == FreshStatus::CorruptData,
	                       "truncated archive was accepted");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testDestinationValidator(const std::vector<uint8_t> &archive) {
	removeTree(ValidationPath);
	Fresh db;
	if (!expectResult(db.init(ValidationPath), "validation init")) return false;
	FreshModelResult model = db.createModel("Validated");
	if (!model) return false;
	if (!expectResult(
	        model.model.setValidator([](const JsonDocument &doc) {
		        return FreshValidationResult{
		            .result = doc["allowed"] | false,
		            .message = "allowed must be true",
		        };
	        }),
	        "set destination validator"
	    )) return false;
	JsonDocument existing;
	existing["_id"] = "existing";
	existing["allowed"] = true;
	if (!expectResult(model.model.create(existing), "create validated destination record")) return false;

	FreshResult imported = db.backupImport(archive.data(), archive.size());
	FreshResult preserved = model.model.findById("existing");
	const bool ok = expect(!imported && imported.status == FreshStatus::ValidationFailed,
	                       "destination validator did not reject import") &&
	                expectResult(preserved, "failed import changed live model");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testLegacyImport() {
	removeTree(LegacyPath);
	JsonDocument legacy;
	legacy["version"] = 2;
	legacy["generatedAt"] = 0;
	legacy["modelCount"] = 1;
	JsonArray models = legacy["models"].to<JsonArray>();
	JsonObject model = models.add<JsonObject>();
	model["name"] = "LegacyUsers";
	model["type"] = "general";
	model["recordCount"] = 1;
	JsonArray docs = model["docs"].to<JsonArray>();
	JsonObject doc = docs.add<JsonObject>();
	doc["_id"] = "legacy-user";
	doc["name"] = "Legacy";

	std::vector<uint8_t> bytes(measureMsgPack(legacy));
	serializeMsgPack(legacy, bytes.data(), bytes.size());

	Fresh db;
	if (!expectResult(db.init(LegacyPath), "legacy init")) return false;
	FreshResult imported = db.backupImport(bytes.data(), bytes.size());
	FreshResult found = db.model("LegacyUsers").findById("legacy-user");
	const bool ok = expectResult(imported, "legacy backup import") &&
	                expectResult(found, "find legacy imported record");
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
	Serial.println("Fresh framed backup regression starting");
	if (!LittleFS.begin(true)) {
		Serial.println("[FAIL] LittleFS mount");
		return;
	}

	removeTree(SourcePath);
	removeTree(RestorePath);
	removeTree(CorruptPath);
	removeTree(LegacyPath);
	removeTree(ValidationPath);

	Fresh source;
	std::vector<uint8_t> archive;
	const bool sourceReady = prepareSource(source);
	const bool backupReady = sourceReady && collectBackup(source, archive);
	runTest("framed export and exact byte count", backupReady);
	if (backupReady) {
		runTest("framed import preserves unrelated models", testFramedExportImport(archive));
		runTest("frame corruption and truncation rejection", testCorruptionAndTruncation(archive));
		runTest("destination validators run during import", testDestinationValidator(archive));
	}
	source.deinit(FreshDeinitOptions{.sync = false});
	runTest("legacy monolithic backup import", testLegacyImport());

	Serial.printf("Framed backup regression complete: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
