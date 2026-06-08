#pragma once

#include "../Fresh.h"
#include "FreshMutex.h"
#include "FreshPsramAllocator.h"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

constexpr uint32_t FreshJournalMagic = 0x4c4a5246; // FRJL
constexpr uint16_t FreshJournalVersion = 1;
constexpr const char *FreshManifestFile = "manifest.msgpack";
constexpr const char *FreshSnapshotFile = "snapshot.msgpack";
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
};

struct FreshModel::State {
	std::string name;
	std::string previousName;
	FreshModelType type = FreshModelType::General;
	std::map<std::string, JsonDocument> docs;
	std::vector<JsonDocument> streamEntries;
	std::deque<FreshPendingRecord> pending;
	FreshResultValidator validator;
	bool dirty = false;
	bool dropped = false;
	bool degraded = false;
	bool snapshotRequired = false;
	uint32_t recordsSinceSnapshot = 0;
	size_t bytesSinceSnapshot = 0;
	uint32_t storageEpoch = 0;
};

struct FreshBackupState {
	FreshByteVector buffer;
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
	FreshResult result = FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");
	FreshMutex mutex;
};

uint32_t FreshChecksum(const uint8_t *data, size_t length);
void FreshWriteU16(File &file, uint16_t value);
void FreshWriteU32(File &file, uint32_t value);
bool FreshReadU16(File &file, uint16_t &value);
bool FreshReadU32(File &file, uint32_t &value);
std::string FreshJoinPath(const std::string &base, const std::string &name);
bool FreshIsValidName(const char *name);
const char *FreshModelTypeToString(FreshModelType type);
FreshModelType FreshModelTypeFromString(const char *type);
const char *FreshJournalOpToString(FreshJournalOp op);
std::string FreshMakeId();
void FreshCopyJson(JsonDocument &target, const JsonDocument &source);
void FreshMergePatch(JsonDocument &target, const JsonDocument &patch);
