#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fresh.h>
#include <LittleFS.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *TestPath = "/fresh_release_hardening";
constexpr size_t SlotHeaderSize = 4 + 2 + 8 + 4 + 4;
constexpr uint32_t SlotMagic = 0x544c5346;
constexpr uint16_t SlotVersion = 1;
constexpr uint32_t ManifestVersion = 4;

int passed = 0;
int failed = 0;

uint16_t readU16(const uint8_t *data) {
	return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readU32(const uint8_t *data) {
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
	       (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readU64(const uint8_t *data) {
	uint64_t value = 0;
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		value |= static_cast<uint64_t>(data[shift / 8]) << shift;
	}
	return value;
}

std::string joinPath(const std::string &base, const std::string &name) {
	if (base.empty() || base == "/") {
		return "/" + name;
	}
	return base.back() == '/' ? base + name : base + "/" + name;
}

void removeTree(const std::string &path) {
	File root = LittleFS.open(path.c_str(), "r");
	if (!root) {
		return;
	}
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
		if (directory) {
			removeTree(childPath);
		} else {
			LittleFS.remove(childPath.c_str());
		}
		child = root.openNextFile();
	}
	root.close();
	LittleFS.rmdir(path.c_str());
}

bool assertTrue(bool condition, const char *message) {
	if (!condition) {
		Serial.print("    ");
		Serial.println(message);
	}
	return condition;
}

bool assertResult(const FreshResult &result, const char *message) {
	if (result) {
		return true;
	}
	Serial.print("    ");
	Serial.print(message);
	Serial.print(": ");
	Serial.println(result.message.c_str());
	return false;
}

bool assertModelResult(const FreshModelResult &result, const char *message) {
	if (result) {
		return true;
	}
	Serial.print("    ");
	Serial.print(message);
	Serial.print(": ");
	Serial.println(result.message.c_str());
	return false;
}

bool readFile(const std::string &path, std::vector<uint8_t> &bytes) {
	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		return false;
	}
	bytes.resize(file.size());
	const int read = file.read(bytes.data(), bytes.size());
	file.close();
	return read == static_cast<int>(bytes.size());
}

bool readManifestSlot(const char slot, JsonDocument &manifest, uint64_t &generation) {
	std::vector<uint8_t> bytes;
	std::string path = joinPath(TestPath, std::string("manifest.") + slot + ".msgpack");
	if (!readFile(path, bytes) || bytes.size() < SlotHeaderSize) {
		return false;
	}
	const uint32_t magic = readU32(bytes.data());
	const uint16_t version = readU16(bytes.data() + 4);
	generation = readU64(bytes.data() + 6);
	const uint32_t payloadSize = readU32(bytes.data() + 14);
	if (magic != SlotMagic || version != SlotVersion || payloadSize == 0 ||
	    SlotHeaderSize + payloadSize != bytes.size()) {
		return false;
	}
	return !deserializeMsgPack(manifest, bytes.data() + SlotHeaderSize, payloadSize);
}

bool currentStorageId(const char *logicalName, std::string &storageId) {
	JsonDocument selected;
	uint64_t selectedGeneration = 0;
	bool foundSlot = false;
	for (char slot : {'a', 'b'}) {
		JsonDocument candidate;
		uint64_t generation = 0;
		if (!readManifestSlot(slot, candidate, generation)) {
			continue;
		}
		if (!foundSlot || generation > selectedGeneration) {
			selected = std::move(candidate);
			selectedGeneration = generation;
			foundSlot = true;
		}
	}
	if (!foundSlot || (selected["version"] | 0U) != ManifestVersion ||
	    !selected["modelCount"].is<uint64_t>() ||
	    !selected["models"].is<JsonArrayConst>()) {
		return false;
	}
	const JsonArrayConst models = selected["models"].as<JsonArrayConst>();
	if (selected["modelCount"].as<uint64_t>() != models.size()) {
		return false;
	}
	for (JsonObjectConst model : models) {
		if (strcmp(model["name"] | "", logicalName) == 0) {
			storageId = model["storageId"] | "";
			return !storageId.empty();
		}
	}
	return false;
}

bool testRenamePersistence() {
	removeTree(TestPath);

	FreshConfig config;
	config.snapshotRecordThreshold = 1000;
	Fresh db;
	if (!assertResult(db.init(TestPath, config), "init")) {
		return false;
	}
	FreshModelResult users = db.createModel("User");
	if (!assertModelResult(users, "create model")) {
		return false;
	}
	JsonDocument user;
	user["_id"] = "rename-user";
	user["name"] = "Panna";
	user["revision"] = 1;
	if (!assertResult(users.model.create(user), "create document") ||
	    !assertResult(db.forceSync(), "initial checkpoint")) {
		return false;
	}

	std::string originalStorageId;
	if (!assertTrue(currentStorageId("User", originalStorageId), "missing original storage id")) {
		return false;
	}

	if (!assertResult(db.renameModel("User", "Account"), "first rename") ||
	    !assertResult(db.renameModel("Account", "People"), "second rename")) {
		return false;
	}
	JsonDocument patch;
	patch["revision"] = 2;
	if (!assertResult(users.model.updateById("rename-user", patch), "write through renamed handle") ||
	    !assertResult(db.flush(), "flush rename and update") ||
	    !assertResult(db.deinit(FreshDeinitOptions{.sync = false}), "deinit without checkpoint")) {
		return false;
	}

	Fresh loaded;
	if (!assertResult(loaded.init(TestPath, config), "reload")) {
		return false;
	}
	FreshResult found = loaded.model("People").findById("rename-user");
	std::string reloadedStorageId;
	const std::string storagePath = joinPath(joinPath(TestPath, "models"), originalStorageId);
	const bool ok = assertResult(found, "find renamed document") &&
	                assertTrue((found.doc["revision"] | 0) == 2, "renamed update was not durable") &&
	                assertTrue(!loaded.model("User"), "old logical model still exists") &&
	                assertTrue(currentStorageId("People", reloadedStorageId), "missing renamed storage id") &&
	                assertTrue(reloadedStorageId == originalStorageId, "storage id changed during rename") &&
	                assertTrue(LittleFS.exists(storagePath.c_str()), "immutable storage directory is missing") &&
	                assertTrue(!LittleFS.exists(joinPath(TestPath, "User").c_str()), "logical-name directory was created") &&
	                assertResult(loaded.deinit(), "final deinit") &&
	                assertResult(loaded.deinit(), "repeated deinit");
	return ok;
}

bool testConfigLimits() {
	FreshConfig snapshotTooLarge;
	snapshotTooLarge.maxSnapshotBytes = 1024 * 1024 + 1;
	Fresh oversizedSnapshot;
	FreshResult snapshotResult = oversizedSnapshot.init(TestPath, snapshotTooLarge);

	FreshConfig journalTooSmall;
	journalTooSmall.maxJournalRecordBytes = journalTooSmall.maxDocumentBytes;
	Fresh invalidJournal;
	FreshResult journalResult = invalidJournal.init(TestPath, journalTooSmall);

	FreshConfig zeroBuffer;
	zeroBuffer.backupBufferSize = 0;
	Fresh invalidBuffer;
	FreshResult bufferResult = invalidBuffer.init(TestPath, zeroBuffer);

	return assertTrue(
	           !snapshotResult && snapshotResult.status == FreshStatus::InvalidArgument,
	           "oversized snapshot limit was accepted"
	       ) &&
	       assertTrue(
	           !journalResult && journalResult.status == FreshStatus::InvalidArgument,
	           "invalid journal/document limits were accepted"
	       ) &&
	       assertTrue(
	           !bufferResult && bufferResult.status == FreshStatus::InvalidArgument,
	           "zero backup buffer was accepted"
	       );
}

struct ReaderContext {
	FreshModel model;
	std::atomic<bool> stop{false};
	std::atomic<bool> failed{false};
	std::atomic<bool> finished{false};
};

void readerTask(void *arg) {
	auto *context = static_cast<ReaderContext *>(arg);
	while (!context->stop.load()) {
		const std::string name = context->model.name();
		if (!name.empty() && name != "User" && name != "Account") {
			context->failed.store(true);
			break;
		}
		(void)context->model.type();
		(void)static_cast<bool>(context->model);
		taskYIELD();
	}
	context->finished.store(true);
	vTaskDelete(nullptr);
}

bool testConcurrentMetadataAccess() {
	removeTree(TestPath);
	Fresh db;
	if (!assertResult(db.init(TestPath), "init concurrency test")) {
		return false;
	}
	FreshModelResult created = db.createModel("User");
	if (!assertModelResult(created, "create concurrency model")) {
		return false;
	}

	ReaderContext context;
	context.model = created.model;
	TaskHandle_t reader = nullptr;
	if (xTaskCreate(readerTask, "fresh-reader", 4096, &context, 1, &reader) != pdPASS) {
		db.deinit();
		return assertTrue(false, "failed to create reader task");
	}
	for (size_t i = 0; i < 100; ++i) {
		const bool even = i % 2 == 0;
		FreshResult renamed = db.renameModel(even ? "User" : "Account", even ? "Account" : "User");
		if (!renamed) {
			context.failed.store(true);
			break;
		}
		delay(1);
	}
	context.stop.store(true);
	while (!context.finished.load()) {
		delay(1);
	}
	const bool ok = assertTrue(!context.failed.load(), "metadata access observed invalid state") &&
	                assertResult(db.deinit(), "deinit concurrency test");
	return ok;
}

void runTest(const char *name, bool (*test)()) {
	Serial.print("[RUN ] ");
	Serial.println(name);
	if (test()) {
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
	Serial.println("Fresh release hardening test starting");
	if (!LittleFS.begin(false)) {
		Serial.println("LittleFS mount failed");
		return;
	}

	runTest("immutable storage id survives rename and flush", testRenamePersistence);
	runTest("invalid persisted-size configuration is rejected", testConfigLimits);
	runTest("concurrent model metadata access remains synchronized", testConcurrentMetadataAccess);

	Serial.printf("Release hardening test complete: %d passed, %d failed\n", passed, failed);
}

void loop() {
	delay(1000);
}
