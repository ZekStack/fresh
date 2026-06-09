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
	LittleFS.remove(joinPath(modelPath.c_str(), "snapshot.msgpack").c_str());
	LittleFS.remove(joinPath(modelPath.c_str(), "snapshot.tmp").c_str());
	LittleFS.rmdir(modelPath.c_str());
}

void resetTestPath(const char *path) {
	LittleFS.remove(joinPath(path, "manifest.msgpack").c_str());
	LittleFS.remove(joinPath(path, "manifest.tmp").c_str());
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
		FreshResult status = db.backupStatus();
		if (status.message == "backup running") {
			return true;
		}
		if (!status && status.status != FreshStatus::Cancelled) {
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

bool corruptSnapshotFile(const char *modelName) {
	const std::string path = joinPath(joinPath(TestPath, modelName).c_str(), "snapshot.msgpack");
	File output = LittleFS.open(path.c_str(), "w");
	if (!output) {
		return false;
	}
	const uint8_t corrupt[] = {0xde, 0xad, 0xbe, 0xef};
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

bool testCorruptSnapshotDiagnostics() {
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

	JsonDocument base;
	base["_id"] = "snapshot-base";
	base["value"] = 1;
	if (!assertResult(usersResult.model.create(base), "create base user")) {
		return false;
	}
	if (!assertResult(db.forceSync(), "force snapshot")) {
		return false;
	}

	JsonDocument extra;
	extra["_id"] = "journal-after-snapshot";
	extra["value"] = 2;
	if (!assertResult(usersResult.model.create(extra), "create post-snapshot user")) {
		return false;
	}
	const std::string journalPath = joinPath(joinPath(TestPath, "User").c_str(), "journal.log");
	if (!assertTrue(waitForFile(journalPath.c_str()), "post-snapshot journal was not written")) {
		return false;
	}
	delay(100);
	if (!assertTrue(corruptSnapshotFile("User"), "failed to corrupt snapshot")) {
		return false;
	}
	db.deinit(FreshDeinitOptions{.sync = false});

	Fresh loaded;
	FreshResult initResult = loaded.init(TestPath);
	const FreshDiagnostics diagnostics = loaded.diagnostics();
	const bool controlled = initResult || initResult.status == FreshStatus::CorruptData;
	const bool ok = assertTrue(controlled, "reload did not fail in a controlled way") &&
	                assertTrue(
	                    hasDegradedLoad(diagnostics, FreshLoadStatus::LoadedWithCorruptSnapshot) ||
	                        diagnostics.degradedModelCount > 0,
	                    "corrupt snapshot diagnostic missing"
	                );
	loaded.deinit(FreshDeinitOptions{.sync = false});
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
		FreshResult status = source.backupStatus();
		if (!status && status.status != FreshStatus::Cancelled) {
			break;
		}
		if (status.message == "backup finished") {
			backupFinished = true;
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
	FreshResult status = db.backupStatus();
	return assertResult(result, "deinit during backup") &&
	       assertTrue(elapsed < 3000, "deinit did not return within bounded time") &&
	       assertTrue(
	           status.status == FreshStatus::BackupNotRunning || status.status == FreshStatus::Cancelled,
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
	runTest("corrupt final journal op -> recovered reload", testCorruptFinalJournalOp);
	runTest("corrupt snapshot -> diagnostics", testCorruptSnapshotDiagnostics);
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
