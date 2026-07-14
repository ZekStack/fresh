#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/*
 * Fresh SelfTest
 *
 * WARNING: This sketch is destructive inside /fresh_selftest,
 * /fresh_selftest_src, and /fresh_selftest_dst. Run it only on a test device
 * or test partition.
 *
 * SelfTest intentionally touches Fresh internal storage files. It is meant for
 * Fresh development and may need updates when the storage format changes.
 */

namespace {

constexpr const char *TestPath = "/fresh_selftest";
constexpr const char *BackupSourcePath = "/fresh_selftest_src";
constexpr const char *BackupDestPath = "/fresh_selftest_dst";

int passed = 0;
int failed = 0;

std::string joinPath(const char *base, const char *name) {
	std::string result = base;
	if (!result.empty() && result.back() != '/') {
		result += "/";
	}
	result += name;
	return result;
}

void removeModelFiles(const char *path, const char *modelName) {
	const std::string modelPath = joinPath(path, modelName);
	LittleFS.remove(joinPath(modelPath.c_str(), "journal.log").c_str());
	LittleFS.remove(joinPath(modelPath.c_str(), "snapshot.a.msgpack").c_str());
	LittleFS.remove(joinPath(modelPath.c_str(), "snapshot.b.msgpack").c_str());
	LittleFS.rmdir(modelPath.c_str());
}

void resetTestPath(const char *path) {
	LittleFS.remove(joinPath(path, "manifest.a.msgpack").c_str());
	LittleFS.remove(joinPath(path, "manifest.b.msgpack").c_str());
	removeModelFiles(path, "User");
	removeModelFiles(path, "Log");
	LittleFS.rmdir(path);
}

bool assertTrue(bool condition, const char *message) {
	if (!condition) {
		Serial.print("    ");
		Serial.println(message);
	}
	return condition;
}

bool assertResult(FreshResult result, const char *message) {
	if (result) {
		return true;
	}
	Serial.print("    ");
	Serial.print(message);
	Serial.print(": ");
	Serial.println(result.message.c_str());
	return false;
}

bool assertModelResult(FreshModelResult result, const char *message) {
	if (result) {
		return true;
	}
	Serial.print("    ");
	Serial.print(message);
	Serial.print(": ");
	Serial.println(result.message.c_str());
	return false;
}

bool waitForFile(const char *path, uint32_t timeoutMS = 1000) {
	const uint32_t started = millis();
	while (millis() - started < timeoutMS) {
		if (LittleFS.exists(path)) {
			File file = LittleFS.open(path, "r");
			if (file && file.size() > 0) {
				file.close();
				return true;
			}
			if (file) {
				file.close();
			}
		}
		delay(10);
	}
	return false;
}

bool readFileBytes(const char *path, std::vector<uint8_t> &bytes) {
	File input = LittleFS.open(path, "r");
	if (!input) {
		return false;
	}
	bytes.resize(input.size());
	const int read = input.read(bytes.data(), bytes.size());
	input.close();
	return read == static_cast<int>(bytes.size());
}

bool writeFileBytes(const char *path, const std::vector<uint8_t> &bytes) {
	File output = LittleFS.open(path, "w");
	if (!output) {
		return false;
	}
	const size_t written = output.write(bytes.data(), bytes.size());
	output.flush();
	output.close();
	return written == bytes.size();
}

bool hasDegradedLoad(const FreshDiagnostics &diagnostics, FreshLoadStatus status) {
	for (const FreshModelLoadInfo &info : diagnostics.modelLoads) {
		if (info.degraded && info.status == status) {
			return true;
		}
	}
	return false;
}

bool waitForBackupRunning(Fresh &db, uint32_t timeoutMS) {
	const uint32_t started = millis();
	while (millis() - started < timeoutMS) {
		FreshBackupStatus status = db.backupStatus();
		if (status.state == FreshBackupState::Running) {
			return true;
		}
		if (!status && status.state != FreshBackupState::Cancelled) {
			return false;
		}
		delay(5);
	}
	return false;
}

bool corruptFinalJournalOpByte(const char *modelName) {
	const std::string path = joinPath(joinPath(TestPath, modelName).c_str(), "journal.log");
	File input = LittleFS.open(path.c_str(), "r");
	if (!input) {
		return false;
	}

	std::vector<uint8_t> bytes(input.size());
	const int read = input.read(bytes.data(), bytes.size());
	input.close();
	if (read != static_cast<int>(bytes.size())) {
		return false;
	}

	constexpr size_t HeaderSize = 16;
	constexpr size_t OpOffset = 6;
	constexpr size_t PayloadSizeOffset = 8;

	size_t cursor = 0;
	size_t lastOpOffset = bytes.size();
	while (cursor + HeaderSize <= bytes.size()) {
		const uint32_t payloadSize = static_cast<uint32_t>(bytes[cursor + PayloadSizeOffset]) |
		                             (static_cast<uint32_t>(bytes[cursor + PayloadSizeOffset + 1]) << 8) |
		                             (static_cast<uint32_t>(bytes[cursor + PayloadSizeOffset + 2]) << 16) |
		                             (static_cast<uint32_t>(bytes[cursor + PayloadSizeOffset + 3]) << 24);
		const size_t next = cursor + HeaderSize + payloadSize;
		if (next > bytes.size()) {
			break;
		}
		lastOpOffset = cursor + OpOffset;
		cursor = next;
	}
	if (lastOpOffset >= bytes.size()) {
		return false;
	}

	bytes[lastOpOffset] = 0xff;
	File output = LittleFS.open(path.c_str(), "w");
	if (!output) {
		return false;
	}
	const size_t written = output.write(bytes.data(), bytes.size());
	output.close();
	return written == bytes.size();
}

bool corruptSnapshotSlot(const char *modelName) {
	const std::string path = joinPath(joinPath(TestPath, modelName).c_str(), "snapshot.a.msgpack");
	if (!LittleFS.exists(path.c_str())) {
		return false;
	}
	File output = LittleFS.open(path.c_str(), "w");
	if (!output) {
		return false;
	}
	const uint8_t corrupt[] = {0xde, 0xad, 0xbe, 0xef};
	const size_t written = output.write(corrupt, sizeof(corrupt));
	output.close();
	return written == sizeof(corrupt);
}

bool corruptManifestSlot() {
	const std::string path = joinPath(TestPath, "manifest.a.msgpack");
	if (!LittleFS.exists(path.c_str())) {
		return false;
	}
	File output = LittleFS.open(path.c_str(), "w");
	if (!output) {
		return false;
	}
	const uint8_t corrupt[] = {0xba, 0xad, 0xf0, 0x0d};
	const size_t written = output.write(corrupt, sizeof(corrupt));
	output.close();
	return written == sizeof(corrupt);
}

bool testCreateReload() {
	resetTestPath(TestPath);

	Fresh db;
	if (!assertResult(db.init(TestPath), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		return false;
	}

	JsonDocument user;
	user["_id"] = "user-create";
	user["name"] = "Panna";
	if (!assertResult(usersResult.model.create(user), "create user")) {
		return false;
	}
	if (!assertResult(db.forceSync(), "force sync")) {
		return false;
	}
	if (!assertResult(db.deinit(), "deinit")) {
		return false;
	}

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload")) {
		return false;
	}
	FreshResult found = loaded.model("User").findById("user-create");
	const bool ok = assertResult(found, "find reloaded user") &&
	                assertTrue(strcmp(found.doc["name"] | "", "Panna") == 0, "reloaded user name mismatch");
	loaded.deinit();
	return ok;
}

bool testUpdateReload() {
	resetTestPath(TestPath);

	Fresh db;
	if (!assertResult(db.init(TestPath), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		return false;
	}

	JsonDocument user;
	user["_id"] = "user-update";
	user["name"] = "Panna";
	user["age"] = 19;
	if (!assertResult(usersResult.model.create(user), "create user")) {
		return false;
	}
	JsonDocument patch;
	patch["age"] = 20;
	if (!assertResult(usersResult.model.updateById("user-update", patch), "update user")) {
		return false;
	}
	if (!assertResult(db.forceSync(), "force sync")) {
		return false;
	}
	db.deinit();

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload")) {
		return false;
	}
	FreshResult found = loaded.model("User").findById("user-update");
	const bool ok = assertResult(found, "find updated user") &&
	                assertTrue((found.doc["age"] | 0) == 20, "reloaded user age mismatch");
	loaded.deinit();
	return ok;
}

bool testDeleteReload() {
	resetTestPath(TestPath);

	Fresh db;
	if (!assertResult(db.init(TestPath), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		return false;
	}

	JsonDocument user;
	user["_id"] = "user-delete";
	user["name"] = "Panna";
	if (!assertResult(usersResult.model.create(user), "create user")) {
		return false;
	}
	if (!assertResult(usersResult.model.deleteById("user-delete"), "delete user")) {
		return false;
	}
	if (!assertResult(db.forceSync(), "force sync")) {
		return false;
	}
	db.deinit();

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload")) {
		return false;
	}
	FreshResult found = loaded.model("User").findById("user-delete");
	const bool ok = assertTrue(!found && found.status == FreshStatus::DocumentNotFound, "deleted user was found");
	loaded.deinit();
	return ok;
}

bool testStreamSnapshotReload() {
	resetTestPath(TestPath);

	FreshConfig config;
	config.snapshotRecordThreshold = 2;
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
	if (!assertModelResult(logsResult, "create stream model")) {
		return false;
	}

	for (int i = 1; i <= 3; ++i) {
		JsonDocument entry;
		entry["value"] = i;
		if (!assertResult(logsResult.model.append(entry), "append stream entry")) {
			return false;
		}
	}
	if (!assertResult(db.forceSync(), "force sync")) {
		return false;
	}
	db.deinit();

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload")) {
		return false;
	}
	FreshResult entries = loaded.model("Log").retrieve();
	const bool ok = assertResult(entries, "retrieve stream entries") &&
	                assertTrue(entries.affectedCount == 3, "stream entry count mismatch");
	loaded.deinit();
	return ok;
}

bool testBoundedStreamFlushReload() {
	resetTestPath(TestPath);

	FreshConfig config;
	config.snapshotRecordThreshold = 1000;
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
	if (!assertModelResult(logsResult, "create stream model")) {
		return false;
	}

	FreshStreamAppendOptions options;
	options.maxEntries = 3;
	for (int i = 1; i <= 6; ++i) {
		JsonDocument entry;
		entry["value"] = i;
		if (!assertResult(logsResult.model.append(entry, options), "append bounded stream entry")) {
			return false;
		}
	}
	if (!assertResult(db.flush(), "flush bounded stream")) {
		return false;
	}

	const std::string modelPath = joinPath(TestPath, "Log");
	const std::string journalPath = joinPath(modelPath.c_str(), "journal.log");
	const bool flushShapeOk = assertTrue(LittleFS.exists(journalPath.c_str()), "flush did not write journal") &&
	                          assertTrue(
	                              !LittleFS.exists(joinPath(modelPath.c_str(), "snapshot.a.msgpack").c_str()) &&
	                                  !LittleFS.exists(joinPath(modelPath.c_str(), "snapshot.b.msgpack").c_str()),
	                              "flush unexpectedly forced a snapshot"
	                          );
	db.deinit(FreshDeinitOptions{.sync = false});
	if (!flushShapeOk) {
		return false;
	}

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath, config), "reload")) {
		return false;
	}
	FreshResult entries = loaded.model("Log").retrieve();
	JsonArrayConst array = entries.doc.as<JsonArrayConst>();
	const bool ok = assertResult(entries, "retrieve bounded stream") &&
	                assertTrue(entries.affectedCount == 3, "bounded stream entry count mismatch") &&
	                assertTrue((array[0]["value"] | 0) == 4, "bounded stream oldest entry mismatch") &&
	                assertTrue((array[1]["value"] | 0) == 5, "bounded stream middle entry mismatch") &&
	                assertTrue((array[2]["value"] | 0) == 6, "bounded stream newest entry mismatch");
	loaded.deinit();
	return ok;
}

bool testCheckpointSkipsRetainedJournal() {
	resetTestPath(TestPath);

	FreshConfig config;
	config.snapshotRecordThreshold = 1000;
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
	if (!assertModelResult(logsResult, "create stream model")) {
		return false;
	}

	FreshStreamAppendOptions options;
	options.maxEntries = 3;
	for (int i = 1; i <= 3; ++i) {
		JsonDocument entry;
		entry["value"] = i;
		if (!assertResult(logsResult.model.append(entry, options), "append checkpoint stream entry")) {
			return false;
		}
	}
	if (!assertResult(db.flush(), "flush journal before checkpoint")) {
		return false;
	}

	const std::string journalPath = joinPath(joinPath(TestPath, "Log").c_str(), "journal.log");
	std::vector<uint8_t> retainedJournal;
	if (!assertTrue(readFileBytes(journalPath.c_str(), retainedJournal), "failed to save journal")) {
		return false;
	}
	if (!assertResult(db.forceSync(), "force checkpoint")) {
		return false;
	}
	if (!assertTrue(writeFileBytes(journalPath.c_str(), retainedJournal), "failed to restore retained journal")) {
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath, config), "reload checkpoint with retained journal")) {
		return false;
	}
	FreshResult entries = loaded.model("Log").retrieve();
	JsonArrayConst array = entries.doc.as<JsonArrayConst>();
	const bool ok = assertResult(entries, "retrieve checkpointed stream") &&
	                assertTrue(entries.affectedCount == 3, "checkpoint replay duplicated stream entries") &&
	                assertTrue((array[0]["value"] | 0) == 1, "checkpoint first entry mismatch") &&
	                assertTrue((array[2]["value"] | 0) == 3, "checkpoint last entry mismatch");
	loaded.deinit();
	return ok;
}

bool testCorruptFinalJournalOp() {
	resetTestPath(TestPath);

	FreshConfig config;
	config.syncIntervalMS = 50;
	config.snapshotRecordThreshold = 1000;
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		return false;
	}

	JsonDocument first;
	first["_id"] = "valid-before-corruption";
	first["value"] = 1;
	JsonDocument second;
	second["_id"] = "corrupted-final-record";
	second["value"] = 2;
	if (!assertResult(usersResult.model.create(first), "create first user") ||
	    !assertResult(usersResult.model.create(second), "create second user")) {
		return false;
	}

	const std::string journalPath = joinPath(joinPath(TestPath, "User").c_str(), "journal.log");
	if (!assertTrue(waitForFile(journalPath.c_str()), "journal was not written")) {
		return false;
	}
	delay(100);
	if (!assertTrue(corruptFinalJournalOpByte("User"), "failed to corrupt final journal op")) {
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload")) {
		return false;
	}
	const FreshDiagnostics diagnostics = loaded.diagnostics();
	FreshResult found = loaded.model("User").findById("valid-before-corruption");
	const bool ok = assertResult(found, "valid journal record was not replayed") &&
	                assertTrue(
	                    hasDegradedLoad(diagnostics, FreshLoadStatus::LoadedWithRecoveredJournal),
	                    "recovered journal diagnostic missing"
	                );
	loaded.deinit();
	return ok;
}

bool testSnapshotNewerSlotRecovery() {
	resetTestPath(TestPath);

	Fresh db;
	if (!assertResult(db.init(TestPath), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		return false;
	}

	JsonDocument oldDoc;
	oldDoc["_id"] = "snapshot-old";
	oldDoc["value"] = 1;
	if (!assertResult(usersResult.model.create(oldDoc), "create old snapshot user")) {
		return false;
	}
	if (!assertResult(db.forceSync(), "force snapshot generation 1")) {
		return false;
	}

	JsonDocument newDoc;
	newDoc["_id"] = "snapshot-new";
	newDoc["value"] = 2;
	if (!assertResult(usersResult.model.create(newDoc), "create new snapshot user")) {
		return false;
	}
	const std::string journalPath = joinPath(joinPath(TestPath, "User").c_str(), "journal.log");
	if (!assertResult(db.forceSync(), "force snapshot generation 2")) {
		return false;
	}
	if (!assertTrue(!LittleFS.exists(journalPath.c_str()), "journal remained after second snapshot")) {
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});

	if (!assertTrue(corruptSnapshotSlot("User"), "failed to corrupt newer snapshot slot")) {
		return false;
	}

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload")) {
		return false;
	}
	const FreshDiagnostics diagnostics = loaded.diagnostics();
	FreshResult oldFound = loaded.model("User").findById("snapshot-old");
	FreshResult newFound = loaded.model("User").findById("snapshot-new");
	const bool ok = assertResult(oldFound, "old snapshot not recovered") &&
	                assertTrue(!newFound, "corrupt newer snapshot unexpectedly loaded") &&
	                assertTrue(
	                    hasDegradedLoad(diagnostics, FreshLoadStatus::LoadedWithCorruptSnapshot),
	                    "recovered snapshot diagnostic missing"
	                );
	loaded.deinit();
	return ok;
}

bool testSizeLimits() {
	resetTestPath(TestPath);

	FreshConfig config;
	config.maxDocumentBytes = 256;
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
	if (!assertModelResult(usersResult, "create user model") ||
	    !assertModelResult(logsResult, "create stream model")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	JsonDocument oversizedCreate;
	oversizedCreate["payload"] =
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
	FreshResult createResult = usersResult.model.create(oversizedCreate);

	JsonDocument oversizedAppend;
	oversizedAppend["payload"] =
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
	FreshResult appendResult = logsResult.model.append(oversizedAppend);

	JsonDocument first;
	first["_id"] = "size-first";
	first["group"] = "same";
	first["payload"] = "small";
	JsonDocument second;
	second["_id"] = "size-second";
	second["group"] = "same";
	second["payload"] = "small";
	if (!assertResult(usersResult.model.create(first), "create first small doc") ||
	    !assertResult(usersResult.model.create(second), "create second small doc")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	JsonDocument patch;
	patch["payload"] =
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
	FreshResult updateResult = usersResult.model.update(
	    [](const JsonDocument &doc) { return strcmp(doc["group"] | "", "same") == 0; },
	    patch
	);
	FreshResult firstAfter = usersResult.model.findById("size-first");
	FreshResult secondAfter = usersResult.model.findById("size-second");

	const bool ok =
	    assertTrue(!createResult && createResult.status == FreshStatus::SizeLimitExceeded, "create size limit failed") &&
	    assertTrue(!appendResult && appendResult.status == FreshStatus::SizeLimitExceeded, "append size limit failed") &&
	    assertTrue(!updateResult && updateResult.status == FreshStatus::SizeLimitExceeded, "update size limit failed") &&
	    assertResult(firstAfter, "find first after failed update") &&
	    assertResult(secondAfter, "find second after failed update") &&
	    assertTrue(strcmp(firstAfter.doc["payload"] | "", "small") == 0, "first doc changed after failed update") &&
	    assertTrue(strcmp(secondAfter.doc["payload"] | "", "small") == 0, "second doc changed after failed update");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testBackupImportSizeLimit() {
	resetTestPath(BackupDestPath);

	FreshConfig config;
	config.maxDocumentBytes = 256;
	Fresh db;
	if (!assertResult(db.init(BackupDestPath, config), "init")) {
		return false;
	}

	JsonDocument archive;
	archive["version"] = 1;
	JsonArray models = archive["models"].to<JsonArray>();
	JsonObject model = models.add<JsonObject>();
	model["name"] = "User";
	model["type"] = "general";
	JsonArray docs = model["docs"].to<JsonArray>();
	JsonObject doc = docs.add<JsonObject>();
	doc["_id"] = "oversized-import";
	doc["payload"] =
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";

	std::vector<uint8_t> bytes(measureMsgPack(archive));
	serializeMsgPack(archive, bytes.data(), bytes.size());
	FreshResult importResult = db.backupImport(bytes.data(), bytes.size());
	const bool ok =
	    assertTrue(!importResult && importResult.status == FreshStatus::SizeLimitExceeded, "backup import size limit failed");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

void overwriteBuffer(char *buffer, size_t size) {
	if (buffer == nullptr || size == 0) {
		return;
	}
	memset(buffer, 'X', size - 1);
	buffer[size - 1] = '\0';
}

bool verifyOwnedJsonDocuments(Fresh &db) {
	FreshResult found = db.model("User").findById("owned-create");
	if (!assertResult(found, "find owned document")) {
		return false;
	}

	const double ratio = found.doc["ratio"] | 0.0;
	const bool userOk =
	    assertTrue(strcmp(found.doc["name"] | "", "Updated One") == 0, "owned updateOne string changed") &&
	    assertTrue(strcmp(found.doc["status"] | "", "Updated By Id") == 0, "owned updateById string changed") &&
	    assertTrue(strcmp(found.doc["profile"]["label"] | "", "Patch Nested") == 0, "owned nested string changed") &&
	    assertTrue(strcmp(found.doc["tags"][0] | "", "Create Array") == 0, "owned array string changed") &&
	    assertTrue((found.doc["enabled"] | false), "owned bool changed") &&
	    assertTrue((found.doc["count"] | 0) == 42, "owned integer changed") &&
	    assertTrue(ratio > 12.49 && ratio < 12.51, "owned float changed") &&
	    assertTrue(found.doc["nothing"].isNull(), "owned null changed");
	if (!userOk) {
		return false;
	}

	FreshResult entries = db.model("Log").retrieve();
	if (!assertResult(entries, "retrieve owned stream entry")) {
		return false;
	}
	JsonArrayConst streamEntries = entries.doc.as<JsonArrayConst>();
	JsonObjectConst entry = streamEntries[0].as<JsonObjectConst>();
	const double streamRatio = entry["ratio"] | 0.0;
	return assertTrue(entries.affectedCount == 1, "owned stream count mismatch") &&
	       assertTrue(strcmp(entry["message"] | "", "Stream Message") == 0, "owned stream string changed") &&
	       assertTrue(strcmp(entry["details"]["label"] | "", "Stream Nested") == 0, "owned stream nested string changed") &&
	       assertTrue(strcmp(entry["values"][0] | "", "Stream Array") == 0, "owned stream array string changed") &&
	       assertTrue((entry["ok"] | false), "owned stream bool changed") &&
	       assertTrue((entry["count"] | 0) == 7, "owned stream integer changed") &&
	       assertTrue(streamRatio > 3.24 && streamRatio < 3.26, "owned stream float changed") &&
	       assertTrue(entry["nothing"].isNull(), "owned stream null changed");
}

bool testOwnedJsonStackStrings() {
	resetTestPath(TestPath);

	Fresh db;
	if (!assertResult(db.init(TestPath), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
	if (!assertModelResult(usersResult, "create user model") ||
	    !assertModelResult(logsResult, "create stream model")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	char createName[] = "Create Name";
	char createNested[] = "Create Nested";
	char createArray[] = "Create Array";
	JsonDocument user;
	user["_id"] = "owned-create";
	user["name"] = JsonString(createName, true);
	user["enabled"] = true;
	user["count"] = 42;
	user["ratio"] = 12.5;
	user["nothing"] = nullptr;
	user["profile"]["label"] = JsonString(createNested, true);
	JsonArray tags = user["tags"].to<JsonArray>();
	tags.add(JsonString(createArray, true));
	tags.add(99);
	if (!assertResult(usersResult.model.create(user), "create owned document")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	char updateName[] = "Updated One";
	char updateNested[] = "Patch Nested";
	JsonDocument updateOnePatch;
	updateOnePatch["name"] = JsonString(updateName, true);
	updateOnePatch["profile"]["label"] = JsonString(updateNested, true);
	if (!assertResult(
	        usersResult.model.updateOne(
	            [](const JsonDocument &doc) {
		            return strcmp(doc["_id"] | "", "owned-create") == 0;
	            },
	            updateOnePatch
	        ),
	        "updateOne owned document"
	    )) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	char updateByIdStatus[] = "Updated By Id";
	JsonDocument updateByIdPatch;
	updateByIdPatch["status"] = JsonString(updateByIdStatus, true);
	if (!assertResult(usersResult.model.updateById("owned-create", updateByIdPatch), "updateById owned document")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	char streamMessage[] = "Stream Message";
	char streamNested[] = "Stream Nested";
	char streamArray[] = "Stream Array";
	JsonDocument streamEntry;
	streamEntry["message"] = JsonString(streamMessage, true);
	streamEntry["ok"] = true;
	streamEntry["count"] = 7;
	streamEntry["ratio"] = 3.25;
	streamEntry["nothing"] = nullptr;
	streamEntry["details"]["label"] = JsonString(streamNested, true);
	JsonArray streamValues = streamEntry["values"].to<JsonArray>();
	streamValues.add(JsonString(streamArray, true));
	streamValues.add(11);
	if (!assertResult(logsResult.model.append(streamEntry), "append owned stream entry")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	overwriteBuffer(createName, sizeof(createName));
	overwriteBuffer(createNested, sizeof(createNested));
	overwriteBuffer(createArray, sizeof(createArray));
	overwriteBuffer(updateName, sizeof(updateName));
	overwriteBuffer(updateNested, sizeof(updateNested));
	overwriteBuffer(updateByIdStatus, sizeof(updateByIdStatus));
	overwriteBuffer(streamMessage, sizeof(streamMessage));
	overwriteBuffer(streamNested, sizeof(streamNested));
	overwriteBuffer(streamArray, sizeof(streamArray));

	if (!verifyOwnedJsonDocuments(db)) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	if (!assertResult(db.forceSync(), "force sync owned documents")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	if (!assertResult(db.deinit(), "deinit owned documents")) {
		return false;
	}

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload owned documents")) {
		return false;
	}
	const bool ok = verifyOwnedJsonDocuments(loaded);
	loaded.deinit();
	return ok;
}

bool testStorageFullPreflight() {
	resetTestPath(TestPath);

	FreshConfig config;
	config.minFreeBytes = LittleFS.totalBytes();
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	JsonDocument user;
	user["_id"] = "storage-full";
	user["name"] = "Panna";
	if (!assertResult(usersResult.model.create(user), "create user")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	FreshResult syncResult = db.forceSync();
	const bool ok = assertTrue(!syncResult && syncResult.status == FreshStatus::StorageFull, "storage full preflight failed");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testEmptyDatabaseMissingManifestSlots() {
	resetTestPath(TestPath);
	Fresh db;
	FreshResult initResult = db.init(TestPath);
	const bool ok = assertResult(initResult, "init empty database");
	db.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testManifestNewerSlotRecovery() {
	resetTestPath(TestPath);

	Fresh db;
	if (!assertResult(db.init(TestPath), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	JsonDocument user;
	user["_id"] = "some-user";
	user["name"] = "Panna";
	if (!assertResult(usersResult.model.create(user), "create user") ||
	    !assertResult(db.forceSync(), "force sync generation 1")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	FreshModelResult logsResult = db.createModel("Log", FreshModelType::Stream);
	if (!assertModelResult(logsResult, "create generation 2 model") ||
	    !assertResult(db.forceSync(), "force sync generation 2")) {
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});

	if (!assertTrue(corruptManifestSlot(), "failed to corrupt newer manifest slot")) {
		return false;
	}

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath), "reload")) {
		return false;
	}
	FreshResult found = loaded.model("User").findById("some-user");
	const bool ok = assertResult(found, "find after older manifest slot recovery");
	loaded.deinit();
	return ok;
}

bool testBackupExportImport() {
	resetTestPath(BackupSourcePath);
	resetTestPath(BackupDestPath);

	Fresh source;
	if (!assertResult(source.init(BackupSourcePath), "source init")) {
		return false;
	}
	FreshModelResult usersResult = source.createModel("User");
	if (!assertModelResult(usersResult, "create source model")) {
		return false;
	}
	JsonDocument user;
	user["_id"] = "backup-user";
	user["name"] = "Panna";
	user["age"] = 20;
	if (!assertResult(usersResult.model.create(user), "create source user")) {
		return false;
	}
	if (!assertResult(source.startBackup(), "start backup")) {
		return false;
	}

	std::vector<uint8_t> archive;
	uint8_t buffer[128];
	const uint32_t started = millis();
	bool backupFinished = false;
	while (millis() - started < 3000) {
		const size_t read = source.readBackup(buffer, sizeof(buffer), 50);
		archive.insert(archive.end(), buffer, buffer + read);
		FreshBackupStatus status = source.backupStatus();
		if (status.state == FreshBackupState::Finished) {
			backupFinished = true;
			break;
		}
		if (status.state == FreshBackupState::Cancelled || status.state == FreshBackupState::Error) {
			break;
		}
	}
	if (!assertTrue(backupFinished, "backup did not finish") ||
	    !assertTrue(!archive.empty(), "backup archive was empty")) {
		source.deinit();
		return false;
	}
	source.deinit();

	Fresh dest;
	if (!assertResult(dest.init(BackupDestPath), "destination init")) {
		return false;
	}
	if (!assertResult(dest.backupImport(archive.data(), archive.size()), "backup import")) {
		return false;
	}
	if (!assertResult(dest.forceSync(), "destination force sync")) {
		return false;
	}
	FreshResult found = dest.model("User").findById("backup-user");
	const bool ok = assertResult(found, "find imported user") &&
	                assertTrue(strcmp(found.doc["name"] | "", "Panna") == 0, "imported user name mismatch") &&
	                assertTrue((found.doc["age"] | 0) == 20, "imported user age mismatch");
	dest.deinit();
	return ok;
}

bool testDeinitDuringBackup() {
	resetTestPath(TestPath);

	FreshConfig config;
	config.backupBufferSize = 512;
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult usersResult = db.createModel("User");
	if (!assertModelResult(usersResult, "create model")) {
		return false;
	}

	for (int i = 0; i < 80; ++i) {
		JsonDocument user;
		char id[32];
		snprintf(id, sizeof(id), "backup-user-%d", i);
		user["_id"] = id;
		user["index"] = i;
		user["payload"] = "abcdefghijklmnopqrstuvwxyz0123456789";
		if (!assertResult(usersResult.model.create(user), "create backup user")) {
			return false;
		}
	}
	if (!assertResult(db.startBackup(), "start backup")) {
		return false;
	}
	if (!assertTrue(waitForBackupRunning(db, 1000), "backup did not enter running state")) {
		db.cancelBackup();
		db.deinit(FreshDeinitOptions{.sync = false});
		return false;
	}

	const uint32_t started = millis();
	FreshResult result = db.deinit(FreshDeinitOptions{.sync = true, .timeoutMS = 2000});
	const uint32_t elapsed = millis() - started;
	FreshBackupStatus status = db.backupStatus();
	return assertResult(result, "deinit during backup") &&
	       assertTrue(elapsed < 3000, "deinit did not return within bounded time") &&
	       assertTrue(
	           status.state == FreshBackupState::NotRunning || status.state == FreshBackupState::Cancelled,
	           "backup was still running after deinit"
	       );
}

void runTest(const char *name, bool (*test)()) {
	const bool ok = test();
	if (ok) {
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
	Serial.println("Fresh SelfTest starting");
	if (!LittleFS.begin(true)) {
		Serial.println("[FAIL] LittleFS mount");
		Serial.println("SelfTest complete: 0 passed, 1 failed");
		return;
	}

	runTest("create -> forceSync -> reload", testCreateReload);
	runTest("update -> forceSync -> reload", testUpdateReload);
	runTest("delete -> forceSync -> reload", testDeleteReload);
	runTest("stream append -> snapshot threshold -> reload", testStreamSnapshotReload);
	runTest("bounded stream -> durable flush -> reload", testBoundedStreamFlushReload);
	runTest("checkpoint skips retained journal records", testCheckpointSkipsRetainedJournal);
	runTest("corrupt final journal op -> recovered reload", testCorruptFinalJournalOp);
	runTest("snapshot newer slot corrupt -> older slot recovery", testSnapshotNewerSlotRecovery);
	runTest("size limits reject oversized writes", testSizeLimits);
	runTest("backup import size limit", testBackupImportSizeLimit);
	runTest("owned JSON survives stack buffer overwrite", testOwnedJsonStackStrings);
	runTest("storage full preflight", testStorageFullPreflight);
	runTest("empty database missing manifest slots", testEmptyDatabaseMissingManifestSlots);
	runTest("manifest newer slot corrupt -> older slot recovery", testManifestNewerSlotRecovery);
	runTest("backup export -> import -> model equality", testBackupExportImport);
	runTest("deinit while backup is running", testDeinitDuringBackup);

	Serial.print("SelfTest complete: ");
	Serial.print(passed);
	Serial.print(" passed, ");
	Serial.print(failed);
	Serial.println(" failed");
}

void loop() {
	delay(1000);
}
