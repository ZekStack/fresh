#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *SourcePath = "/fresh_selective_source";
constexpr const char *DestinationPath = "/fresh_selective_destination";

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

bool containsText(const std::vector<uint8_t> &bytes, const char *text) {
	const uint8_t *begin = reinterpret_cast<const uint8_t *>(text);
	const uint8_t *end = begin + strlen(text);
	return std::search(bytes.begin(), bytes.end(), begin, end) != bytes.end();
}

bool addDocument(FreshModel &model, const char *id, const char *label) {
	JsonDocument doc;
	doc["_id"] = id;
	doc["label"] = label;
	return expectResult(model.create(doc), "create document");
}

bool prepareSource(Fresh &source, FreshModel &hardware, FreshModel &logs) {
	FreshConfig config;
	config.backupBufferSize = 128;
	config.syncIntervalMS = 60000;
	if (!expectResult(source.init(SourcePath, config), "source init")) return false;

	FreshModelResult usersResult = source.createModel("User");
	FreshModelResult hardwareResult = source.createModel("Hardware");
	FreshModelResult logsResult = source.createModel("Log", FreshModelType::Stream);
	FreshModelResult droppedResult = source.createModel("Dropped");
	if (!usersResult || !hardwareResult || !logsResult || !droppedResult) return false;

	FreshModel users = usersResult.model;
	hardware = hardwareResult.model;
	logs = logsResult.model;
	if (!addDocument(users, "user-secret", "sensitive-user-payload") ||
	    !addDocument(hardware, "hardware-1", "relay") ||
	    !addDocument(hardware, "hardware-2", "dimmer")) {
		return false;
	}

	JsonDocument log;
	log["message"] = "hardware online";
	if (!expectResult(logs.append(log), "append log")) return false;
	return expectResult(source.dropModel("Dropped"), "drop test model");
}

bool testOptionValidation(Fresh &source) {
	FreshBackupEstimate estimate;

	FreshBackupOptions duplicate;
	duplicate.modelNames = {"Hardware", "Hardware"};
	FreshResult duplicateEstimate = source.estimateBackup(duplicate, estimate);
	FreshResult duplicateStart = source.startBackup(duplicate);

	FreshBackupOptions missing;
	missing.modelNames = {"Missing"};
	FreshResult missingEstimate = source.estimateBackup(missing, estimate);
	FreshResult missingStart = source.startBackup(missing);

	FreshBackupOptions dropped;
	dropped.modelNames = {"Dropped"};
	FreshResult droppedEstimate = source.estimateBackup(dropped, estimate);
	FreshResult droppedStart = source.startBackup(dropped);

	return expect(!duplicateEstimate && duplicateEstimate.status == FreshStatus::InvalidArgument,
	              "duplicate estimate was accepted") &&
	       expect(!duplicateStart && duplicateStart.status == FreshStatus::InvalidArgument,
	              "duplicate start was accepted") &&
	       expect(!missingEstimate && missingEstimate.status == FreshStatus::ModelNotFound,
	              "missing estimate was accepted") &&
	       expect(!missingStart && missingStart.status == FreshStatus::ModelNotFound,
	              "missing start was accepted") &&
	       expect(!droppedEstimate && droppedEstimate.status == FreshStatus::ModelNotFound,
	              "dropped estimate was accepted") &&
	       expect(!droppedStart && droppedStart.status == FreshStatus::ModelNotFound,
	              "dropped start was accepted");
}

bool testSelectiveExport(Fresh &source) {
	FreshBackupOptions selected;
	selected.modelNames = {"Log", "Hardware"};
	FreshBackupEstimate estimate;
	if (!expectResult(source.estimateBackup(selected, estimate), "estimate selected backup")) return false;
	if (!expect(estimate.modelCount == 2, "selected model count mismatch") ||
	    !expect(estimate.recordCount == 3, "selected record count mismatch")) {
		return false;
	}

	if (!expectResult(source.startBackup(selected), "start selected backup")) return false;
	selected.modelNames = {"User"};
	std::vector<uint8_t> archive;
	if (!expect(drainBackup(source, archive), "selected backup did not finish")) return false;
	if (!expect(archive.size() == estimate.totalBytes, "estimate did not match archive size") ||
	    !expect(!containsText(archive, "sensitive-user-payload"), "unselected user payload leaked into archive")) {
		return false;
	}

	Fresh destination;
	if (!expectResult(destination.init(DestinationPath), "destination init")) return false;
	FreshModelResult keepResult = destination.createModel("Keep");
	if (!keepResult || !addDocument(keepResult.model, "keep-1", "preserved")) return false;
	if (!expectResult(destination.backupImport(archive.data(), archive.size()), "import selected backup")) return false;

	FreshResult hardware = destination.model("Hardware").findById("hardware-2");
	FreshResult keep = destination.model("Keep").findById("keep-1");
	FreshResult logRecords = destination.model("Log").retrieve();
	const bool ok = expectResult(hardware, "find imported hardware") &&
	                expectResult(keep, "find preserved model") &&
	                expectResult(logRecords, "retrieve imported log") &&
	                expect(logRecords.affectedCount == 1, "imported log count mismatch") &&
	                expect(!static_cast<bool>(destination.model("User")), "unselected user model was imported");
	destination.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testEstimateRefreshAndReuse(Fresh &source, FreshModel &hardware) {
	FreshBackupOptions selected;
	selected.modelNames = {"Hardware"};
	FreshBackupEstimate before;
	if (!expectResult(source.estimateBackup(selected, before), "estimate before mutation")) return false;
	if (!addDocument(hardware, "hardware-3", "contact")) return false;

	std::vector<uint8_t> first;
	if (!expectResult(source.startBackup(selected), "start after mutation") ||
	    !expect(drainBackup(source, first), "backup after mutation did not finish")) {
		return false;
	}
	FreshBackupEstimate after;
	if (!expectResult(source.estimateBackup(selected, after), "estimate after mutation")) return false;
	if (!expect(after.recordCount == 3, "updated estimate record count mismatch") ||
	    !expect(first.size() == after.totalBytes, "startBackup did not remeasure current data") ||
	    !expect(first.size() > before.totalBytes, "archive size did not grow after mutation")) {
		return false;
	}

	std::vector<uint8_t> second;
	return expectResult(source.startBackup(selected), "reuse backup options") &&
	       expect(drainBackup(source, second), "reused backup did not finish") &&
	       expect(second.size() == after.totalBytes, "reused options changed estimate");
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
	Serial.println("Fresh SelectiveBackupRegressionTest starting");
	if (!LittleFS.begin(true)) {
		Serial.println("[FAIL] LittleFS mount");
		return;
	}
	removeTree(SourcePath);
	removeTree(DestinationPath);

	Fresh source;
	FreshModel hardware;
	FreshModel logs;
	if (!prepareSource(source, hardware, logs)) {
		Serial.println("[FAIL] prepare source");
		return;
	}

	runTest("option validation", testOptionValidation(source));
	runTest("selective export and exact estimate", testSelectiveExport(source));
	removeTree(DestinationPath);
	runTest("estimate refresh and option reuse", testEstimateRefreshAndReuse(source, hardware));

	FreshBackupOptions all;
	FreshBackupEstimate allEstimate;
	runTest(
	    "empty selection means all active models",
	    expectResult(source.estimateBackup(all, allEstimate), "estimate all") &&
	        expect(allEstimate.modelCount == 3, "all-model estimate count mismatch")
	);

	source.deinit(FreshDeinitOptions{.sync = false});
	Serial.printf("SelectiveBackupRegressionTest complete: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
