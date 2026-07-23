#pragma once

#include "../Fresh.h"
#include "FreshBuffer.h"
#include "FreshMutex.h"

#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

constexpr uint32_t FreshJournalMagic = 0x4c4a5246; // FRJL
constexpr uint16_t FreshJournalVersion = 3;
constexpr size_t FreshJournalHeaderSize = 4 + 2 + 1 + 1 + 4 + 4;
constexpr uint32_t FreshSlotMagic = 0x544c5346; // FSLT
constexpr uint16_t FreshSlotVersion = 1;
constexpr uint32_t FreshManifestVersion = 4;
constexpr uint32_t FreshSnapshotVersion = 4;
constexpr uint32_t FreshBackupVersion = 2;
constexpr size_t FreshSlotHeaderSize = 4 + 2 + 8 + 4 + 4;
constexpr size_t FreshMaxPersistedPayloadBytes = 1024 * 1024;
constexpr size_t FreshMaxBackupBufferBytes = 1024 * 1024;
constexpr const char *FreshManifestFile = "manifest";
constexpr const char *FreshSnapshotFile = "snapshot";
constexpr const char *FreshJournalFile = "journal.log";

enum class FreshJournalOp : uint8_t {
	Create = 1,
	Update = 2,
	Delete = 3,
	Append = 4,
};

struct FreshPendingRecord {
	FreshJournalOp op = FreshJournalOp::Create;
	uint64_t sequence = 0;
	std::string id;
	JsonDocument doc;
	size_t maxEntries = 0;
};

struct FreshSlotReadResult {
	FreshResult result = FreshResult::success("slot missing");
	JsonDocument payload;
	uint64_t generation = 0;
	bool hadCorruptSlot = false;
	bool hadValidSlot = false;
	bool missing = true;
};

struct FreshModel::State {
	std::string name;
	std::string storageId;
	FreshModelType type = FreshModelType::General;
	std::map<std::string, JsonDocument> docs;
	std::deque<JsonDocument> streamEntries;
	std::deque<FreshPendingRecord> pending;
	FreshResultValidator validator;
	bool dirty = false;
	bool dropped = false;
	bool degraded = false;
	bool snapshotRequired = false;
	uint32_t recordsSinceSnapshot = 0;
	size_t bytesSinceSnapshot = 0;
	uint64_t checkpointSequence = 0;
	uint64_t lastSequence = 0;
	uint64_t revision = 1;
	uint32_t storageEpoch = 0;
};

struct FreshBackupModelSnapshot {
	std::shared_ptr<FreshModel::State> state;
	std::string name;
	FreshModelType type = FreshModelType::General;
	uint64_t revision = 0;
	uint64_t recordCount = 0;
};

struct FreshBackupPlan {
	std::vector<FreshBackupModelSnapshot> models;
	uint64_t databaseRevision = 0;
	uint64_t recordCount = 0;
	uint64_t totalBytes = 0;
};

struct FreshBackupRuntimeState {
	FreshBuffer buffer;
	FreshBackupOptions options;
	size_t head = 0;
	size_t tail = 0;
	size_t used = 0;
	size_t progress = 0;
	size_t total = 0;
	size_t lastProgressEvent = 0;
	bool requested = false;
	bool running = false;
	bool done = false;
	bool cancelled = false;
	FreshBackupState state = FreshBackupState::NotRunning;
	FreshResult result = FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");
	FreshMutex mutex;
};

inline FreshResult FreshJsonAllocationFailure(const char *label) {
	std::string message = "failed to construct ";
	message += label != nullptr ? label : "json";
	return FreshResult::failure(FreshStatus::OutOfMemory, message.c_str());
}

template <typename TTarget, typename TValue>
FreshResult FreshJsonSet(
    TTarget target,
    const TValue &value,
    JsonDocument &document,
    const char *label
) {
	using Value = std::decay_t<TValue>;
	if constexpr (std::is_integral_v<Value>) {
		if (label != nullptr && strcmp(label, "journal sequence") == 0) {
			const uint64_t sequence = static_cast<uint64_t>(value);
			if (sequence == 0 || sequence == std::numeric_limits<uint64_t>::max()) {
				return FreshResult::failure(
				    FreshStatus::InternalError,
				    "journal sequence is exhausted"
				);
			}
		}
	}
	if (!target.set(value) || document.overflowed()) {
		return FreshJsonAllocationFailure(label);
	}
	return FreshResult::success();
}

template <typename TTarget>
FreshResult FreshJsonCreateArray(
    TTarget target,
    JsonDocument &document,
    JsonArray &array,
    const char *label
) {
	array = target.template to<JsonArray>();
	if (array.isNull() || document.overflowed()) {
		return FreshJsonAllocationFailure(label);
	}
	return FreshResult::success();
}

inline FreshResult FreshJsonAdd(
    JsonArray array,
    JsonVariantConst value,
    JsonDocument &document,
    const char *label
) {
	if (!array.add(value) || document.overflowed()) {
		return FreshJsonAllocationFailure(label);
	}
	return FreshResult::success();
}

inline FreshResult FreshJsonAddObject(
    JsonArray array,
    JsonDocument &document,
    JsonObject &object,
    const char *label
) {
	object = array.add<JsonObject>();
	if (object.isNull() || document.overflowed()) {
		return FreshJsonAllocationFailure(label);
	}
	return FreshResult::success();
}

uint32_t FreshChecksum(const uint8_t *data, size_t length);
void FreshWriteU16(File &file, uint16_t value);
void FreshWriteU32(File &file, uint32_t value);
void FreshWriteU64(File &file, uint64_t value);
bool FreshReadU16(File &file, uint16_t &value);
bool FreshReadU32(File &file, uint32_t &value);
bool FreshReadU64(File &file, uint64_t &value);
std::string FreshJoinPath(const std::string &base, const std::string &name);
bool FreshIsValidName(const char *name);
const char *FreshModelTypeToString(FreshModelType type);
FreshModelType FreshModelTypeFromString(const char *type);
bool FreshParseJournalOp(uint8_t value, FreshJournalOp &op);
const char *FreshJournalOpToString(FreshJournalOp op);
std::string FreshMakeId();
FreshResult FreshCloneJson(JsonDocument &target, JsonVariantConst source, const char *label);
FreshResult FreshCopyJson(JsonDocument &target, const JsonDocument &source, const char *label = "json");
FreshResult FreshMergePatch(JsonDocument &target, const JsonDocument &patch);
FreshResult FreshValidateJsonDocument(const JsonDocument &document, const char *label);
FreshResult FreshNextRevision(uint64_t current, uint64_t &next, const char *label);
