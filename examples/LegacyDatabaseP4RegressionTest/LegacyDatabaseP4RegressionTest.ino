#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fresh.h>
#include <FreshFile.h>
#include <LittleFS.h>

#include <atomic>
#include <string>
#include <vector>

#if defined(CONFIG_IDF_TARGET_ESP32P4)

namespace {

constexpr const char *DatabasePath = "/fresh_legacy_p4";
constexpr const char *MarkerPath = "/fresh_legacy_p4.ready";
constexpr const char *ModelsPath = "/fresh_legacy_p4/models";
constexpr const char *StorageId = "0123456789abcdef";
constexpr const char *ModelPath = "/fresh_legacy_p4/models/0123456789abcdef";
constexpr const char *ManifestPath = "/fresh_legacy_p4/manifest.b.msgpack";
constexpr const char *JournalPath = "/fresh_legacy_p4/models/0123456789abcdef/journal.log";
constexpr const char *SizeCachePath = "/fresh_vfs_size_cache.bin";
constexpr const char *ModelName = "Items";
constexpr size_t RecordCount = 2048;

// Fresh 0.1.1 persisted-format constants. Keep these local so this fixture
// continues to represent the legacy format even if a future release changes it.
constexpr uint32_t LegacyJournalMagic = 0x4c4a5246; // FRJL
constexpr uint16_t LegacyJournalVersion = 3;
constexpr uint8_t LegacyCreateOperation = 1;
constexpr uint32_t LegacySlotMagic = 0x544c5346; // FSLT
constexpr uint16_t LegacySlotVersion = 1;
constexpr uint32_t LegacyManifestVersion = 4;
constexpr uint64_t LegacyManifestGeneration = 1;

Fresh database;
std::atomic<bool> replayFinished{false};
std::atomic<bool> replayPassed{false};
std::atomic<uint32_t> cpu0ProbeRuns{0};

uint32_t checksum(const uint8_t *data, size_t length) {
	uint32_t hash = 2166136261u;
	for (size_t index = 0; index < length; ++index) {
		hash ^= data[index];
		hash *= 16777619u;
	}
	return hash;
}

bool writeU16(File &file, uint16_t value) {
	return file.write(static_cast<uint8_t>(value & 0xff)) == 1 &&
	       file.write(static_cast<uint8_t>((value >> 8) & 0xff)) == 1;
}

bool writeU32(File &file, uint32_t value) {
	return file.write(static_cast<uint8_t>(value & 0xff)) == 1 &&
	       file.write(static_cast<uint8_t>((value >> 8) & 0xff)) == 1 &&
	       file.write(static_cast<uint8_t>((value >> 16) & 0xff)) == 1 &&
	       file.write(static_cast<uint8_t>((value >> 24) & 0xff)) == 1;
}

bool writeU64(File &file, uint64_t value) {
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		if (file.write(static_cast<uint8_t>((value >> shift) & 0xff)) != 1) return false;
	}
	return true;
}

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

bool createDirectory(const char *path) {
	return LittleFS.exists(path) || LittleFS.mkdir(path);
}

bool writeLegacyManifest() {
	JsonDocument manifest;
	manifest["version"] = LegacyManifestVersion;
	manifest["modelCount"] = 1ULL;
	JsonArray models = manifest["models"].to<JsonArray>();
	JsonObject model = models.add<JsonObject>();
	model["name"] = ModelName;
	model["storageId"] = StorageId;
	model["type"] = "general";

	const size_t payloadSize = measureMsgPack(manifest);
	if (payloadSize == 0 || payloadSize > UINT32_MAX) return false;
	std::vector<uint8_t> payload(payloadSize);
	if (serializeMsgPack(manifest, payload.data(), payload.size()) != payload.size()) return false;

	File file = LittleFS.open(ManifestPath, "w");
	if (!file) return false;
	const bool headerWritten = writeU32(file, LegacySlotMagic) &&
	                           writeU16(file, LegacySlotVersion) &&
	                           writeU64(file, LegacyManifestGeneration) &&
	                           writeU32(file, static_cast<uint32_t>(payload.size())) &&
	                           writeU32(file, checksum(payload.data(), payload.size()));
	const bool payloadWritten = headerWritten && file.write(payload.data(), payload.size()) == payload.size();
	file.flush();
	const bool writeSucceeded = payloadWritten && file.getWriteError() == 0;
	file.close();
	return writeSucceeded;
}

bool writeLegacyJournal() {
	File file = LittleFS.open(JournalPath, "w");
	if (!file) return false;

	std::vector<uint8_t> payload;
	for (size_t index = 0; index < RecordCount; ++index) {
		char id[24] = {};
		snprintf(id, sizeof(id), "legacy-%04u", static_cast<unsigned>(index));

		JsonDocument record;
		record["sequence"] = static_cast<uint64_t>(index + 1);
		record["op"] = "create";
		record["id"] = id;
		JsonObject document = record["doc"].to<JsonObject>();
		document["_id"] = id;
		document["value"] = static_cast<uint32_t>(index);

		const size_t payloadSize = measureMsgPack(record);
		if (payloadSize == 0 || payloadSize > UINT32_MAX) {
			file.close();
			return false;
		}
		payload.resize(payloadSize);
		if (serializeMsgPack(record, payload.data(), payload.size()) != payload.size()) {
			file.close();
			return false;
		}

		const bool headerWritten = writeU32(file, LegacyJournalMagic) &&
		                           writeU16(file, LegacyJournalVersion) &&
		                           file.write(LegacyCreateOperation) == 1 &&
		                           file.write(static_cast<uint8_t>(0)) == 1 &&
		                           writeU32(file, static_cast<uint32_t>(payload.size())) &&
		                           writeU32(file, checksum(payload.data(), payload.size()));
		if (!headerWritten || file.write(payload.data(), payload.size()) != payload.size() ||
		    file.getWriteError() != 0) {
			file.close();
			return false;
		}

		// Fixture creation is not the behavior under test. Keep it cooperative so
		// the first boot cannot starve the idle task while producing the journal.
		if ((index + 1) % 32 == 0) delay(1);
	}

	file.flush();
	const bool writeSucceeded = file.getWriteError() == 0;
	file.close();
	return writeSucceeded;
}

bool prepareLegacyFixture() {
	removeTree(DatabasePath);
	LittleFS.remove(MarkerPath);
	if (!createDirectory(DatabasePath) || !createDirectory(ModelsPath) || !createDirectory(ModelPath)) {
		return false;
	}
	if (!writeLegacyManifest() || !writeLegacyJournal()) return false;

	File marker = LittleFS.open(MarkerPath, "w");
	if (!marker) return false;
	const char contents[] = "fresh-0.1.1-compatible-fixture";
	const bool written = marker.write(
	    reinterpret_cast<const uint8_t *>(contents),
	    sizeof(contents) - 1
	) == sizeof(contents) - 1;
	marker.flush();
	const bool succeeded = written && marker.getWriteError() == 0;
	marker.close();
	return succeeded;
}

bool verifyReplayedRecords() {
	FreshModelListResult models = database.listModels();
	if (!models || models.models.size() != 1 || models.models[0].name != ModelName ||
	    models.models[0].recordCount != RecordCount) {
		Serial.println("legacy model metadata did not replay correctly");
		return false;
	}

	FreshModel model = database.model(ModelName);
	if (!model) {
		Serial.println("legacy model is unavailable after initialization");
		return false;
	}

	for (size_t index = 0; index < RecordCount; ++index) {
		char id[24] = {};
		snprintf(id, sizeof(id), "legacy-%04u", static_cast<unsigned>(index));
		FreshResult found = model.findById(id);
		if (!found || (found.doc["value"] | UINT32_MAX) != static_cast<uint32_t>(index)) {
			Serial.printf("record verification failed at index %u\n", static_cast<unsigned>(index));
			return false;
		}
		if ((index + 1) % 64 == 0) delay(1);
	}
	return true;
}

FreshResult openApplicationFile(const char *path, FreshOpenMode mode, FreshFile &file) {
	return database.withStorage([&](FreshStorage &storage) {
		return storage.open(path, mode, file);
	});
}

bool verifyVFSSizeCache() {
	const uint8_t first[] = {1, 2, 3, 4};
	const uint8_t second[] = {5, 6};

	FreshFile file;
	FreshResult opened = openApplicationFile(SizeCachePath, FreshOpenMode::Write, file);
	if (!opened || file.write(first, sizeof(first)) != sizeof(first) || file.size() != sizeof(first)) {
		Serial.println("VFS size cache did not track initial writes");
		file.close();
		return false;
	}
	if (!file.close()) {
		Serial.println("failed to close initial VFS size-cache file");
		return false;
	}

	opened = openApplicationFile(SizeCachePath, FreshOpenMode::Append, file);
	if (!opened || file.size() != sizeof(first) ||
	    file.write(second, sizeof(second)) != sizeof(second) ||
	    file.size() != sizeof(first) + sizeof(second)) {
		Serial.println("VFS size cache did not initialize or grow during append");
		file.close();
		return false;
	}
	if (!file.close()) {
		Serial.println("failed to close appended VFS size-cache file");
		return false;
	}

	opened = openApplicationFile(SizeCachePath, FreshOpenMode::Read, file);
	if (!opened || file.size() != sizeof(first) + sizeof(second) ||
	    file.available() != static_cast<int>(sizeof(first) + sizeof(second))) {
		Serial.println("VFS size cache did not restore the open-time file size");
		file.close();
		return false;
	}
	if (!file.close()) {
		Serial.println("failed to close read VFS size-cache file");
		return false;
	}

	FreshResult removed = database.withStorage([](FreshStorage &storage) {
		return storage.removeFile(SizeCachePath);
	});
	if (!removed) {
		Serial.println("failed to remove VFS size-cache test file");
		return false;
	}
	return true;
}

void cpu0ProbeTask(void *) {
	while (!replayFinished.load()) {
		cpu0ProbeRuns.fetch_add(1);
		vTaskDelay(1);
	}
	vTaskDelete(nullptr);
}

void replayTask(void *) {
	FreshConfig config;
	config.syncIntervalMS = 60000;
	config.littleFS.formatOnMountFailure = false;
	config.snapshotRecordThreshold = UINT32_MAX;
	config.snapshotBytesThreshold = SIZE_MAX;

	const uint32_t probeBefore = cpu0ProbeRuns.load();
	const uint32_t startedAt = millis();
	FreshResult initialized = database.init(DatabasePath, config);
	const uint32_t replayDurationMS = millis() - startedAt;
	const uint32_t probeRunsDuringReplay = cpu0ProbeRuns.load() - probeBefore;
	if (!initialized) {
		Serial.printf(
		    "Fresh initialization failed: %s (%s)\n",
		    database.statusToString(initialized.status),
		    initialized.message.c_str()
		);
		replayFinished.store(true);
		replayPassed.store(false);
		vTaskDelete(nullptr);
		return;
	}

	Serial.printf(
	    "Fresh 0.1.1 journal replay completed on CPU%d in %u ms; CPU0 probe ran %u times\n",
	    xPortGetCoreID(),
	    static_cast<unsigned>(replayDurationMS),
	    static_cast<unsigned>(probeRunsDuringReplay)
	);
	const bool cooperative = probeRunsDuringReplay > 0;
	if (!cooperative) {
		Serial.println("journal replay did not yield to the CPU0 cooperative probe");
	}
	const bool recordsVerified = verifyReplayedRecords();
	const bool sizeCacheVerified = verifyVFSSizeCache();
	FreshResult stopped = database.deinit(FreshDeinitOptions{.sync = false, .timeoutMS = 5000});
	if (!stopped) {
		Serial.printf("Fresh shutdown failed: %s\n", stopped.message.c_str());
	}

	replayPassed.store(
	    cooperative && recordsVerified && sizeCacheVerified && static_cast<bool>(stopped)
	);
	replayFinished.store(true);
	vTaskDelete(nullptr);
}

} // namespace

#endif

void setup() {
	Serial.begin(115200);
	delay(500);

#if !defined(CONFIG_IDF_TARGET_ESP32P4)
	Serial.println("LegacyDatabaseP4RegressionTest is only executed on ESP32-P4.");
#else
	if (!LittleFS.begin(true)) {
		Serial.println("failed to mount LittleFS for legacy fixture preparation");
		return;
	}

	const bool fixtureReady = LittleFS.exists(MarkerPath) &&
	                          LittleFS.exists(ManifestPath) &&
	                          LittleFS.exists(JournalPath);
	if (!fixtureReady) {
		Serial.printf("creating Fresh 0.1.1-compatible fixture with %u records\n", RecordCount);
		if (!prepareLegacyFixture()) {
			Serial.println("failed to create Fresh 0.1.1-compatible fixture");
			LittleFS.end();
			return;
		}
		LittleFS.end();
		Serial.println("fixture prepared; restarting into Fresh 0.2.0 replay boot");
		delay(100);
		ESP.restart();
		return;
	}
	LittleFS.end();
	delay(50);

	BaseType_t probeCreated = xTaskCreatePinnedToCore(
	    cpu0ProbeTask,
	    "fresh-cpu0-probe",
	    3 * 1024,
	    nullptr,
	    0,
	    nullptr,
	    0
	);
	BaseType_t replayCreated = xTaskCreatePinnedToCore(
	    replayTask,
	    "fresh-legacy-replay",
	    24 * 1024,
	    nullptr,
	    5,
	    nullptr,
	    0
	);
	if (probeCreated != pdPASS || replayCreated != pdPASS) {
		Serial.println("failed to create CPU0 regression tasks");
		replayFinished.store(true);
	}
#endif
}

void loop() {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
	static bool reported = false;
	if (!reported && replayFinished.load()) {
		reported = true;
		Serial.println(replayPassed.load()
		                   ? "PASS: legacy replay recovered every record and yielded on CPU0"
		                   : "FAIL: legacy database replay regression");
	}
#endif
	delay(1000);
}
