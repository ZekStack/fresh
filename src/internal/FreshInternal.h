#pragma once

#include "../Fresh.h"
#include "FreshMemory.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

constexpr uint16_t FreshManifestVersion = 1;
constexpr uint32_t FreshSlotMagic = 0x46525348;
constexpr uint16_t FreshSlotVersion = 1;
constexpr uint32_t FreshJournalMagic = 0x4A524E4C;
constexpr uint8_t FreshJournalVersion = 1;
constexpr size_t FreshSlotHeaderSize = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint64_t) +
                                       sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t FreshJournalHeaderSize = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) +
                                          sizeof(uint32_t);
constexpr size_t FreshMaxPersistedPayloadBytes = 1024 * 1024;
constexpr size_t FreshMaxBackupBufferBytes = 256 * 1024;
constexpr size_t FreshMaxBackupFrameBytes = FreshMaxPersistedPayloadBytes;
constexpr size_t FreshMaxModelCount = 1024;
constexpr size_t FreshMaxNameBytes = 128;
constexpr const char *FreshManifestFile = "manifest";
constexpr const char *FreshSnapshotFile = "snapshot";
constexpr const char *FreshJournalFile = "journal.log";

struct FreshMutex {
	FreshMutex();
	~FreshMutex();

	FreshMutex(const FreshMutex &) = delete;
	FreshMutex &operator=(const FreshMutex &) = delete;

	SemaphoreHandle_t handle = nullptr;
};

class FreshLock {
  public:
	explicit FreshLock(FreshMutex &mutex, TickType_t timeout = portMAX_DELAY);
	~FreshLock();

	FreshLock(const FreshLock &) = delete;
	FreshLock &operator=(const FreshLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	FreshMutex *_mutex = nullptr;
	bool _locked = false;
};

enum class FreshJournalOp : uint8_t {
	Create = 1,
	Replace = 2,
	Update = 3,
	Delete = 4,
	Append = 5,
};

struct FreshPendingRecord {
	FreshJournalOp op = FreshJournalOp::Create;
	uint64_t sequence = 0;
	std::string id;
	JsonDocument doc;
	size_t maxEntries = 0;
};

struct FreshModel::State {
	std::string name;
	std::string storageId;
	FreshModelType type = FreshModelType::General;
	std::map<std::string, JsonDocument> docs;
	std::deque<JsonDocument> streamEntries;
	std::vector<FreshPendingRecord> pending;
	FreshResultValidator validator;
	bool dirty = false;
	bool snapshotRequired = false;
	bool degraded = false;
	bool dropped = false;
	uint64_t generation = 0;
	uint64_t lastSequence = 0;
	uint32_t journalRecordCount = 0;
	size_t journalBytes = 0;
	uint32_t storageEpoch = 0;
	uint64_t revision = 1;
};

struct FreshBackupRuntimeState {
	FreshMutex mutex;
	FreshBuffer buffer;
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
	FreshBackupOptions options;
};

inline FreshResult FreshJsonAllocationFailure(const char *label) {
	std::string message = label != nullptr ? label : "json";
	message += " allocation failed";
	return FreshResult::failure(FreshStatus::OutOfMemory, message.c_str());
}

template <typename TDestination, typename TValue>
FreshResult FreshJsonSet(
    TDestination destination,
    const TValue &value,
    JsonDocument &document,
    const char *label
) {
	destination.set(value);
	if (document.overflowed()) return FreshJsonAllocationFailure(label);
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
void FreshWriteU16(Print &output, uint16_t value);
void FreshWriteU32(Print &output, uint32_t value);
void FreshWriteU64(Print &output, uint64_t value);
bool FreshReadU16(Stream &input, uint16_t &value);
bool FreshReadU32(Stream &input, uint32_t &value);
bool FreshReadU64(Stream &input, uint64_t &value);
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
