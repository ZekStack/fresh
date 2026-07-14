#pragma once

#include "../Fresh.h"
#include "FreshBuffer.h"
#include "FreshMutex.h"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

constexpr uint32_t FreshJournalMagic = 0x4c4a5246; // FRJL
constexpr uint16_t FreshJournalVersion = 3;
constexpr size_t FreshJournalHeaderSize = 4 + 2 + 1 + 1 + 4 + 4;
constexpr uint32_t FreshSlotMagic = 0x544c5346; // FSLT
constexpr uint16_t FreshSlotVersion = 1;
constexpr uint32_t FreshManifestVersion = 3;
constexpr uint32_t FreshSnapshotVersion = 3;
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
	uint32_t storageEpoch = 0;
};

struct FreshBackupRuntimeState {
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
	FreshMutex mutex;
};

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
void FreshCopyJson(JsonDocument &target, const JsonDocument &source);
void FreshMergePatch(JsonDocument &target, const JsonDocument &patch);
