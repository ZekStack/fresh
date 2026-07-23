#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *SourcePath = "/fresh_inspect_source";
constexpr const char *InspectorPath = "/fresh_inspect_target";
constexpr const char *SmallInspectorPath = "/fresh_inspect_small";
constexpr const char *EmptyPath = "/fresh_inspect_empty";

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

uint32_t checksum(const uint8_t *data, size_t length) {
	uint32_t value = 2166136261u;
	for (size_t i = 0; i < length; ++i) {
		value ^= data[i];
		value *= 16777619u;
	}
	return value;
}

uint32_t readU32(const uint8_t *data) {
	return static_cast<uint32_t>(data[0]) |
	       (static_cast<uint32_t>(data[1]) << 8) |
	       (static_cast<uint32_t>(data[2]) << 16) |
	       (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readU64(const uint8_t *data) {
	uint64_t value = 0;
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		value |= static_cast<uint64_t>(data[shift / 8]) << shift;
	}
	return value;
}

void writeU32(uint8_t *data, uint32_t value) {
	data[0] = static_cast<uint8_t>(value & 0xff);
	data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
	data[2] = static_cast<uint8_t>((value >> 16) & 0xff);
	data[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void writeU64(uint8_t *data, uint64_t value) {
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		data[shift / 8] = static_cast<uint8_t>((value >> shift) & 0xff);
	}
}

void refreshHeaderChecksum(std::vector<uint8_t> &archive) {
	writeU32(archive.data() + 36, checksum(archive.data(), 36));
}

bool findFrame(const std::vector<uint8_t> &archive, uint8_t type, size_t &offset, size_t &payloadSize) {
	size_t cursor = 40;
	while (cursor + 12 <= archive.size()) {
		const size_t framePayload = readU32(archive.data() + cursor + 4);
		const size_t frameBytes = 8 + framePayload + 4;
		if (frameBytes > archive.size() - cursor) return false;
		if (archive[cursor] == type) {
			offset = cursor;
			payloadSize = framePayload;
			return true;
		}
		cursor += frameBytes;
	}
	return false;
}

void refreshFrameChecksum(std::vector<uint8_t> &archive, size_t offset, size_t payloadSize) {
	const size_t checkedBytes = 8 + payloadSize;
	writeU32(archive.data() + offset + checkedBytes, checksum(archive.data() + offset, checkedBytes));
}

bool collectBackup(
    Fresh &db,
    const FreshBackupOptions &options,
    std::vector<uint8_t> &archive,
    uint32_t timeoutMS = 5000
) {
	archive.clear();
	if (!expectResult(db.startBackup(options), "start backup")) return false;
	uint8_t buffer[91];
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

bool addGeneral(FreshModel &model, const char *id, const char *payload) {
	JsonDocument document;
	document["_id"] = id;
	document["payload"] = payload;
	return expectResult(model.create(document), "create general document");
}

bool prepareSource(Fresh &source, std::vector<uint8_t> &full, std::vector<uint8_t> &selected) {
	FreshConfig config;
	config.backupBufferSize = 128;
	config.syncIntervalMS = 60000;
	if (!expectResult(source.init(SourcePath, config), "source init")) return false;

	FreshModelResult usersResult = source.createModel("User");
	FreshModelResult logsResult = source.createModel("Log", FreshModelType::Stream);
	if (!usersResult || !logsResult) return false;
	FreshModel users = usersResult.model;
	FreshModel logs = logsResult.model;
	if (!addGeneral(
	        users,
	        "user-1",
	        "this payload is intentionally larger than the small inspection limit"
	    )) return false;
	JsonDocument log;
	log["level"] = "info";
	log["message"] = "controller ready";
	if (!expectResult(logs.append(log), "append stream record")) return false;

	if (!collectBackup(source, FreshBackupOptions(), full)) return false;
	FreshBackupOptions selectedOptions;
	selectedOptions.modelNames = {"User"};
	return collectBackup(source, selectedOptions, selected);
}

bool testMetadataAndNoMutation(
    Fresh &inspector,
    const std::vector<uint8_t> &full,
    const std::vector<uint8_t> &selected
) {
	FreshModelResult existingResult = inspector.createModel("Existing");
	if (!existingResult || !addGeneral(existingResult.model, "existing-1", "preserved")) return false;

	FreshBackupMetadata fullMetadata;
	FreshResult fullResult = inspector.inspectBackup(full.data(), full.size(), fullMetadata);
	FreshBackupMetadata selectedMetadata;
	FreshResult selectedResult = inspector.inspectBackup(selected.data(), selected.size(), selectedMetadata);
	FreshBackupMetadata repeatedMetadata;
	FreshResult repeatedResult = inspector.inspectBackup(full.data(), full.size(), repeatedMetadata);
	FreshResult existing = inspector.model("Existing").findById("existing-1");

	return expectResult(fullResult, "inspect full backup") &&
	       expect(!fullMetadata.legacyFormat, "framed backup reported as legacy") &&
	       expect(fullMetadata.containerVersion == 2, "container version mismatch") &&
	       expect(fullMetadata.totalBytes == full.size(), "full byte count mismatch") &&
	       expect(fullMetadata.modelCount == 2, "full model count mismatch") &&
	       expect(fullMetadata.recordCount == 2, "full record count mismatch") &&
	       expect(fullMetadata.models.size() == 2, "full model metadata missing") &&
	       expectResult(selectedResult, "inspect selective backup") &&
	       expect(selectedMetadata.modelCount == 1, "selective model count mismatch") &&
	       expect(selectedMetadata.recordCount == 1, "selective record count mismatch") &&
	       expect(selectedMetadata.models.size() == 1 && selectedMetadata.models[0].name == "User",
	              "selective model metadata mismatch") &&
	       expectResult(repeatedResult, "repeat inspection") &&
	       expect(repeatedMetadata.totalBytes == fullMetadata.totalBytes, "repeat metadata mismatch") &&
	       expectResult(existing, "inspection mutated live database");
}

bool expectInspectionFailure(
    Fresh &inspector,
    const std::vector<uint8_t> &archive,
    FreshStatus expectedStatus,
    const char *message
) {
	FreshBackupMetadata metadata;
	FreshResult result = inspector.inspectBackup(archive.data(), archive.size(), metadata);
	return expect(!result && result.status == expectedStatus, message) &&
	       expect(metadata.models.empty(), "failed inspection returned partial metadata");
}

bool testFramedFailures(Fresh &inspector, const std::vector<uint8_t> &valid) {
	std::vector<uint8_t> headerChecksum = valid;
	headerChecksum[8] ^= 0x01;

	std::vector<uint8_t> recordChecksum = valid;
	size_t recordOffset = 0;
	size_t recordPayload = 0;
	if (!findFrame(recordChecksum, 2, recordOffset, recordPayload) || recordPayload == 0) return false;
	recordChecksum[recordOffset + 8] ^= 0x01;

	std::vector<uint8_t> invalidMsgPack = valid;
	if (!findFrame(invalidMsgPack, 2, recordOffset, recordPayload) || recordPayload == 0) return false;
	std::fill(
	    invalidMsgPack.begin() + static_cast<std::ptrdiff_t>(recordOffset + 8),
	    invalidMsgPack.begin() + static_cast<std::ptrdiff_t>(recordOffset + 8 + recordPayload),
	    static_cast<uint8_t>(0xc1)
	);
	refreshFrameChecksum(invalidMsgPack, recordOffset, recordPayload);

	std::vector<uint8_t> truncated = valid;
	truncated.pop_back();
	std::vector<uint8_t> trailing = valid;
	trailing.push_back(0xaa);

	std::vector<uint8_t> wrongModels = valid;
	writeU32(wrongModels.data() + 16, readU32(wrongModels.data() + 16) + 1);
	refreshHeaderChecksum(wrongModels);

	std::vector<uint8_t> wrongRecords = valid;
	writeU64(wrongRecords.data() + 20, readU64(wrongRecords.data() + 20) + 1);
	refreshHeaderChecksum(wrongRecords);

	return expectInspectionFailure(
	           inspector,
	           headerChecksum,
	           FreshStatus::CorruptData,
	           "header checksum corruption accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           recordChecksum,
	           FreshStatus::CorruptData,
	           "record checksum corruption accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           invalidMsgPack,
	           FreshStatus::CorruptData,
	           "invalid MessagePack accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           truncated,
	           FreshStatus::CorruptData,
	           "truncated archive accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           trailing,
	           FreshStatus::CorruptData,
	           "trailing bytes accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           wrongModels,
	           FreshStatus::CorruptData,
	           "wrong model count accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           wrongRecords,
	           FreshStatus::CorruptData,
	           "wrong record count accepted"
	       );
}

std::vector<uint8_t> serializeLegacy(JsonDocument &archive) {
	std::vector<uint8_t> bytes(measureMsgPack(archive));
	serializeMsgPack(archive, bytes.data(), bytes.size());
	return bytes;
}

JsonDocument makeLegacyBase() {
	JsonDocument archive;
	archive["version"] = 2;
	archive["generatedAt"] = 1234;
	archive["modelCount"] = 1;
	JsonArray models = archive["models"].to<JsonArray>();
	JsonObject model = models.add<JsonObject>();
	model["name"] = "Legacy";
	model["type"] = "general";
	model["recordCount"] = 1;
	JsonArray docs = model["docs"].to<JsonArray>();
	JsonObject document = docs.add<JsonObject>();
	document["_id"] = "legacy-1";
	document["value"] = 7;
	return archive;
}

bool testLegacy(Fresh &inspector) {
	JsonDocument validArchive = makeLegacyBase();
	std::vector<uint8_t> valid = serializeLegacy(validArchive);
	FreshBackupMetadata metadata;
	FreshResult inspected = inspector.inspectBackup(valid.data(), valid.size(), metadata);

	JsonDocument duplicateArchive = makeLegacyBase();
	JsonObject duplicateModel = duplicateArchive["models"][0];
	duplicateModel["recordCount"] = 2;
	JsonArray duplicateDocs = duplicateModel["docs"];
	JsonObject duplicate = duplicateDocs.add<JsonObject>();
	duplicate["_id"] = "legacy-1";
	duplicate["value"] = 8;
	std::vector<uint8_t> duplicateBytes = serializeLegacy(duplicateArchive);

	JsonDocument scalarArchive = makeLegacyBase();
	JsonObject scalarModel = scalarArchive["models"][0];
	JsonArray scalarDocs = scalarModel["docs"];
	scalarDocs.clear();
	scalarDocs.add(42);
	std::vector<uint8_t> scalarBytes = serializeLegacy(scalarArchive);

	JsonDocument countArchive = makeLegacyBase();
	countArchive["models"][0]["recordCount"] = 2;
	std::vector<uint8_t> countBytes = serializeLegacy(countArchive);

	return expectResult(inspected, "inspect legacy backup") &&
	       expect(metadata.legacyFormat, "legacy backup not identified") &&
	       expect(metadata.containerVersion == 2, "legacy version mismatch") &&
	       expect(metadata.generatedAt == 1234, "legacy timestamp mismatch") &&
	       expect(metadata.modelCount == 1 && metadata.recordCount == 1,
	              "legacy counts mismatch") &&
	       expectInspectionFailure(
	           inspector,
	           duplicateBytes,
	           FreshStatus::CorruptData,
	           "duplicate legacy id accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           scalarBytes,
	           FreshStatus::CorruptData,
	           "scalar legacy record accepted"
	       ) &&
	       expectInspectionFailure(
	           inspector,
	           countBytes,
	           FreshStatus::CorruptData,
	           "legacy count mismatch accepted"
	       );
}

bool testSizeLimit(const std::vector<uint8_t> &archive) {
	FreshConfig config;
	config.maxDocumentBytes = 32;
	config.maxJournalRecordBytes = 128;
	config.maxSnapshotBytes = 128;
	Fresh inspector;
	if (!expectResult(inspector.init(SmallInspectorPath, config), "small inspector init")) return false;
	FreshBackupMetadata metadata;
	FreshResult result = inspector.inspectBackup(archive.data(), archive.size(), metadata);
	const bool ok = expect(!result && result.status == FreshStatus::SizeLimitExceeded,
	                       "oversized record was accepted");
	inspector.deinit(FreshDeinitOptions{.sync = false});
	return ok;
}

bool testEmptyArchive(Fresh &inspector) {
	Fresh empty;
	if (!expectResult(empty.init(EmptyPath), "empty source init")) return false;
	std::vector<uint8_t> archive;
	const bool collected = collectBackup(empty, FreshBackupOptions(), archive);
	empty.deinit(FreshDeinitOptions{.sync = false});
	if (!collected) return false;
	FreshBackupMetadata metadata;
	FreshResult result = inspector.inspectBackup(archive.data(), archive.size(), metadata);
	return expectResult(result, "inspect empty backup") &&
	       expect(metadata.modelCount == 0 && metadata.recordCount == 0,
	              "empty backup counts mismatch");
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(1000);
	Serial.println();
	Serial.println("Fresh BackupInspectionRegressionTest starting");
	if (!LittleFS.begin(true)) {
		Serial.println("[FAIL] LittleFS mount");
		return;
	}
	removeTree(SourcePath);
	removeTree(InspectorPath);
	removeTree(SmallInspectorPath);
	removeTree(EmptyPath);

	Fresh source;
	std::vector<uint8_t> full;
	std::vector<uint8_t> selected;
	if (!prepareSource(source, full, selected)) {
		Serial.println("[FAIL] prepare source");
		return;
	}

	Fresh inspector;
	if (!expectResult(inspector.init(InspectorPath), "inspector init")) return;
	runTest("metadata, selective scope, repeat, and no mutation",
	        testMetadataAndNoMutation(inspector, full, selected));
	runTest("framed corruption and structural validation", testFramedFailures(inspector, full));
	runTest("legacy inspection and semantic validation", testLegacy(inspector));
	runTest("empty archive inspection", testEmptyArchive(inspector));
	runTest("configured document size limit", testSizeLimit(full));

	inspector.deinit(FreshDeinitOptions{.sync = false});
	source.deinit(FreshDeinitOptions{.sync = false});
	Serial.printf("Backup inspection regression complete: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
