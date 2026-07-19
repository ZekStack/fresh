#include "Fresh.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <LittleFS.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace {

struct FreshJournalSyncRecord {
	FreshJournalSyncRecord() = default;
	FreshJournalSyncRecord(const FreshJournalSyncRecord &) = delete;
	FreshJournalSyncRecord &operator=(const FreshJournalSyncRecord &) = delete;
	FreshJournalSyncRecord(FreshJournalSyncRecord &&) noexcept = default;
	FreshJournalSyncRecord &operator=(FreshJournalSyncRecord &&) noexcept = default;

	uint64_t sequence = 0;
	FreshJournalOp op = FreshJournalOp::Create;
	FreshBuffer payload;
};

struct FreshModelSyncBatch {
	FreshModelSyncBatch() : snapshot(&FreshJsonAllocator()) {
	}
	FreshModelSyncBatch(const FreshModelSyncBatch &) = delete;
	FreshModelSyncBatch &operator=(const FreshModelSyncBatch &) = delete;
	FreshModelSyncBatch(FreshModelSyncBatch &&) noexcept = default;
	FreshModelSyncBatch &operator=(FreshModelSyncBatch &&) noexcept = default;

	std::shared_ptr<void> state;
	std::string name;
	std::string storageId;
	std::string path;
	std::string journalPath;
	FreshModelType type = FreshModelType::General;
	bool dropped = false;
	bool writeSnapshot = false;
	uint32_t storageEpoch = 0;
	uint64_t checkpointSequence = 0;
	size_t maxSnapshotBytes = FreshMaxPersistedPayloadBytes;
	size_t snapshotRecordCount = 0;
	std::vector<FreshJournalSyncRecord> records;
	JsonDocument snapshot;
	size_t snapshotPayloadBytes = 0;
};

struct FreshModelSyncResult {
	FreshResult result = FreshResult::success("model synced");
	size_t recordsWritten = 0;
	size_t bytesWritten = 0;
	bool snapshotWritten = false;
	bool dropped = false;
};

std::string FreshSlotPath(const std::string &basePath, const char *fileBaseName, char slot) {
	std::string fileName = fileBaseName;
	fileName += ".";
	fileName.push_back(slot);
	fileName += ".msgpack";
	return FreshJoinPath(basePath, fileName);
}

bool FreshAddRequiredBytes(size_t &total, size_t value) {
	if (value > std::numeric_limits<size_t>::max() - total) {
		return false;
	}
	total += value;
	return true;
}

FreshResult FreshAllocateBuffer(
    FreshBuffer &buffer,
    size_t size,
    const char *label,
    FreshAllocationCategory category = FreshAllocationCategory::General
) {
	if (!buffer.allocate(size, category)) {
		std::string message = "failed to allocate ";
		message += label != nullptr ? label : "buffer";
		return FreshResult::failure(FreshStatus::OutOfMemory, message.c_str());
	}
	return FreshResult::success();
}

FreshResult FreshSerializePayload(const JsonDocument &payload, FreshBuffer &bytes, const char *label) {
	FreshResult valid = FreshValidateJsonDocument(payload, "persisted payload");
	if (!valid) {
		return valid;
	}
	const size_t payloadBytes = measureMsgPack(payload);
	if (payloadBytes == 0) {
		return FreshResult::failure(FreshStatus::FileSystemError, label);
	}
	if (payloadBytes > FreshMaxPersistedPayloadBytes || payloadBytes > UINT32_MAX) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "persisted payload is too large");
	}
	FreshResult allocation = FreshAllocateBuffer(
	    bytes,
	    payloadBytes,
	    "persisted payload",
	    FreshAllocationCategory::DurableSlotPayload
	);
	if (!allocation) {
		return allocation;
	}
	const size_t written = serializeMsgPack(payload, bytes.data(), bytes.size());
	if (written != payloadBytes) {
		bytes.reset();
		return FreshResult::failure(FreshStatus::FileSystemError, label);
	}
	return FreshResult::success();
}

FreshResult FreshReadSlotFile(
    const std::string &path,
    JsonDocument &payload,
    uint64_t &generation,
    size_t maxPayloadBytes
) {
	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open durable slot");
	}

	uint32_t magic = 0;
	uint16_t version = 0;
	uint64_t slotGeneration = 0;
	uint32_t payloadSize = 0;
	uint32_t expectedChecksum = 0;
	const bool headerOk = FreshReadU32(file, magic) && FreshReadU16(file, version) &&
	                      FreshReadU64(file, slotGeneration) && FreshReadU32(file, payloadSize) &&
	                      FreshReadU32(file, expectedChecksum);
	if (!headerOk || magic != FreshSlotMagic || version != FreshSlotVersion || payloadSize == 0 ||
	    payloadSize > FreshMaxPersistedPayloadBytes || payloadSize > maxPayloadBytes ||
	    file.available() < static_cast<int>(payloadSize)) {
		file.close();
		return FreshResult::failure(FreshStatus::CorruptData, "invalid durable slot header");
	}

	FreshBuffer bytes;
	FreshResult allocation = FreshAllocateBuffer(
	    bytes,
	    payloadSize,
	    "durable slot payload",
	    FreshAllocationCategory::DurableSlotPayload
	);
	if (!allocation) {
		file.close();
		return allocation;
	}
	const int read = file.read(bytes.data(), bytes.size());
	file.close();
	if (read != static_cast<int>(bytes.size())) {
		return FreshResult::failure(FreshStatus::CorruptData, "truncated durable slot");
	}
	if (FreshChecksum(bytes.data(), bytes.size()) != expectedChecksum) {
		return FreshResult::failure(FreshStatus::CorruptData, "durable slot checksum mismatch");
	}

	JsonDocument decoded(&FreshJsonAllocator());
	DeserializationError error = deserializeMsgPack(decoded, bytes.data(), bytes.size());
	if (error || decoded.overflowed()) {
		return FreshResult::failure(
		    error == DeserializationError::NoMemory || decoded.overflowed()
		        ? FreshStatus::OutOfMemory
		        : FreshStatus::CorruptData,
		    "failed to decode durable slot"
		);
	}
	payload = std::move(decoded);
	generation = slotGeneration;
	return FreshResult::success("durable slot loaded");
}

FreshSlotReadResult readDurableSlot(
    const std::string &basePath,
    const char *fileBaseName,
    size_t maxPayloadBytes = FreshMaxPersistedPayloadBytes
) {
	FreshSlotReadResult result;
	const std::string slotAPath = FreshSlotPath(basePath, fileBaseName, 'a');
	const std::string slotBPath = FreshSlotPath(basePath, fileBaseName, 'b');
	const bool existsA = LittleFS.exists(slotAPath.c_str());
	const bool existsB = LittleFS.exists(slotBPath.c_str());
	if (!existsA && !existsB) {
		result.missing = true;
		result.result = FreshResult::success("durable slot missing");
		return result;
	}

	FreshResult lastFailure = FreshResult::failure(FreshStatus::CorruptData, "no valid durable slot");
	for (const std::string &path : {slotAPath, slotBPath}) {
		if (!LittleFS.exists(path.c_str())) {
			continue;
		}
		JsonDocument candidate(&FreshJsonAllocator());
		uint64_t candidateGeneration = 0;
		FreshResult readResult = FreshReadSlotFile(path, candidate, candidateGeneration, maxPayloadBytes);
		if (!readResult) {
			if (readResult.status == FreshStatus::OutOfMemory) {
				result.missing = false;
				result.result = readResult;
				return result;
			}
			lastFailure = readResult;
			result.hadCorruptSlot = true;
			continue;
		}
		if (!result.hadValidSlot || candidateGeneration > result.generation) {
			result.payload = std::move(candidate);
			result.generation = candidateGeneration;
		}
		result.hadValidSlot = true;
	}

	result.missing = false;
	if (!result.hadValidSlot) {
		result.result = lastFailure.status == FreshStatus::FileSystemError
		                    ? lastFailure
		                    : FreshResult::failure(FreshStatus::CorruptData, "no valid durable slot");
		return result;
	}
	result.result = FreshResult::success(
	    result.hadCorruptSlot ? "durable slot loaded with recovery" : "durable slot loaded"
	);
	return result;
}

FreshResult writeDurableSlot(
    const std::string &basePath,
    const char *fileBaseName,
    const JsonDocument &payload,
    uint64_t currentGeneration,
    size_t maxPayloadBytes,
    JsonDocument *verifiedPayload = nullptr,
    uint64_t *committedGeneration = nullptr
) {
	FreshBuffer bytes;
	FreshResult serializeResult = FreshSerializePayload(payload, bytes, "failed to serialize durable slot");
	if (!serializeResult) {
		return serializeResult;
	}
	if (bytes.size() > maxPayloadBytes) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "durable slot payload is too large");
	}

	const uint64_t nextGeneration = currentGeneration + 1;
	if (nextGeneration == 0) {
		return FreshResult::failure(FreshStatus::InternalError, "durable slot generation overflow");
	}
	const char targetSlot = nextGeneration % 2 == 0 ? 'a' : 'b';
	const std::string targetPath = FreshSlotPath(basePath, fileBaseName, targetSlot);
	File file = LittleFS.open(targetPath.c_str(), "w");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open durable slot");
	}

	FreshWriteU32(file, FreshSlotMagic);
	FreshWriteU16(file, FreshSlotVersion);
	FreshWriteU64(file, nextGeneration);
	FreshWriteU32(file, static_cast<uint32_t>(bytes.size()));
	FreshWriteU32(file, FreshChecksum(bytes.data(), bytes.size()));
	const size_t written = file.write(bytes.data(), bytes.size());
	file.flush();
	file.close();
	if (written != bytes.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write durable slot");
	}

	JsonDocument verified(&FreshJsonAllocator());
	uint64_t verifiedGeneration = 0;
	FreshResult verifyResult = FreshReadSlotFile(targetPath, verified, verifiedGeneration, maxPayloadBytes);
	if (!verifyResult) {
		return verifyResult;
	}
	if (verifiedGeneration != nextGeneration) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to verify durable slot");
	}
	if (verifiedPayload != nullptr) {
		*verifiedPayload = std::move(verified);
	}
	if (committedGeneration != nullptr) {
		*committedGeneration = nextGeneration;
	}
	return FreshResult::success("durable slot written");
}

FreshResult FreshValidateManifestPayload(const JsonDocument &payload) {
	if ((payload["version"] | 0U) != FreshManifestVersion ||
	    !payload["modelCount"].is<uint64_t>() ||
	    !payload["models"].is<JsonArrayConst>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "unsupported or corrupt manifest payload");
	}
	const uint64_t declaredCount = payload["modelCount"].as<uint64_t>();
	const size_t actualCount = payload["models"].as<JsonArrayConst>().size();
	if (declaredCount > SIZE_MAX || static_cast<size_t>(declaredCount) != actualCount) {
		return FreshResult::failure(FreshStatus::CorruptData, "manifest model count mismatch");
	}
	return FreshResult::success();
}

FreshResult FreshValidateSnapshotPayload(
    const JsonDocument &payload,
    const std::string &storageId,
    FreshModelType type,
    uint64_t checkpointSequence,
    size_t expectedRecordCount
) {
	const char *snapshotStorageId = payload["storageId"] | "";
	const char *snapshotType = payload["type"] | "";
	if ((payload["version"] | 0U) != FreshSnapshotVersion ||
	    storageId != snapshotStorageId ||
	    strcmp(snapshotType, FreshModelTypeToString(type)) != 0 ||
	    !payload["appliedThroughSequence"].is<uint64_t>() ||
	    payload["appliedThroughSequence"].as<uint64_t>() != checkpointSequence ||
	    !payload["recordCount"].is<uint64_t>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "invalid snapshot payload");
	}
	const char *arrayName = type == FreshModelType::Stream ? "entries" : "docs";
	if (!payload[arrayName].is<JsonArrayConst>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "snapshot records are missing");
	}
	const uint64_t declaredCount = payload["recordCount"].as<uint64_t>();
	const size_t actualCount = payload[arrayName].as<JsonArrayConst>().size();
	if (declaredCount > SIZE_MAX || static_cast<size_t>(declaredCount) != actualCount ||
	    actualCount != expectedRecordCount) {
		return FreshResult::failure(FreshStatus::CorruptData, "snapshot record count mismatch");
	}
	return FreshResult::success();
}

FreshResult FreshWriteJournalRecord(const FreshModelSyncBatch &batch, const FreshJournalSyncRecord &record) {
	if (record.payload.empty() || record.payload.size() > UINT32_MAX ||
	    record.payload.size() > FreshMaxPersistedPayloadBytes) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "journal record payload is too large");
	}
	if (!LittleFS.exists(batch.path.c_str()) && !LittleFS.mkdir(batch.path.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create model directory");
	}

	File file = LittleFS.open(batch.journalPath.c_str(), "a");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open journal");
	}

	FreshWriteU32(file, FreshJournalMagic);
	FreshWriteU16(file, FreshJournalVersion);
	file.write(static_cast<uint8_t>(record.op));
	file.write(static_cast<uint8_t>(0));
	FreshWriteU32(file, static_cast<uint32_t>(record.payload.size()));
	FreshWriteU32(file, FreshChecksum(record.payload.data(), record.payload.size()));
	const size_t written = file.write(record.payload.data(), record.payload.size());
	file.flush();
	file.close();

	if (written != record.payload.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write journal");
	}
	return FreshResult::success("journal record written");
}

FreshResult FreshWriteSnapshotBatch(const FreshModelSyncBatch &batch, bool &snapshotWritten) {
	snapshotWritten = false;
	if (!LittleFS.exists(batch.path.c_str()) && !LittleFS.mkdir(batch.path.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create model directory");
	}

	FreshSlotReadResult slot = readDurableSlot(batch.path, FreshSnapshotFile, batch.maxSnapshotBytes);
	if (!slot.result) {
		return slot.result;
	}
	JsonDocument verified(&FreshJsonAllocator());
	FreshResult writeResult = writeDurableSlot(
	    batch.path,
	    FreshSnapshotFile,
	    batch.snapshot,
	    slot.generation,
	    batch.maxSnapshotBytes,
	    &verified
	);
	if (!writeResult) {
		return writeResult;
	}
	FreshResult semanticResult = FreshValidateSnapshotPayload(
	    verified,
	    batch.storageId,
	    batch.type,
	    batch.checkpointSequence,
	    batch.snapshotRecordCount
	);
	if (!semanticResult) {
		return semanticResult;
	}
	snapshotWritten = true;

	if (LittleFS.exists(batch.journalPath.c_str()) && !LittleFS.remove(batch.journalPath.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "snapshot written but journal cleanup failed");
	}
	return FreshResult::success("snapshot written");
}

FreshModelSyncResult FreshWriteActiveModelBatch(const FreshModelSyncBatch &batch) {
	FreshModelSyncResult result;
	result.result = FreshResult::success("model synced");

	for (const FreshJournalSyncRecord &record : batch.records) {
		FreshResult writeResult = FreshWriteJournalRecord(batch, record);
		if (!writeResult) {
			result.result = writeResult;
			return result;
		}
		result.recordsWritten++;
		result.bytesWritten += record.payload.size();
	}

	if (batch.writeSnapshot) {
		bool snapshotWritten = false;
		FreshResult snapshotResult = FreshWriteSnapshotBatch(batch, snapshotWritten);
		result.snapshotWritten = snapshotWritten;
		if (!snapshotResult) {
			result.result = snapshotResult;
			return result;
		}
		result.result = snapshotResult;
	}

	return result;
}

FreshModelSyncResult FreshDeleteModelBatch(const FreshModelSyncBatch &batch) {
	FreshModelSyncResult result;
	LittleFS.remove(batch.journalPath.c_str());
	LittleFS.remove(FreshSlotPath(batch.path, FreshSnapshotFile, 'a').c_str());
	LittleFS.remove(FreshSlotPath(batch.path, FreshSnapshotFile, 'b').c_str());
	if (LittleFS.exists(batch.path.c_str()) && !LittleFS.rmdir(batch.path.c_str())) {
		result.result = FreshResult::failure(FreshStatus::FileSystemError, "failed to remove model directory");
		return result;
	}
	result.dropped = true;
	result.result = FreshResult::success("model dropped");
	return result;
}

FreshResult FreshLoadResult(FreshLoadStatus status, const char *message) {
	FreshResult result = FreshResult::success(message);
	result.affectedCount = static_cast<size_t>(status);
	return result;
}

FreshLoadStatus FreshLoadStatusFromResult(const FreshResult &result) {
	return static_cast<FreshLoadStatus>(result.affectedCount);
}

bool FreshShouldCheckpointRecords(uint32_t previous, size_t added, uint32_t threshold) {
	if (threshold == 0) {
		return true;
	}
	return previous >= threshold || added >= static_cast<size_t>(threshold - previous);
}

bool FreshShouldCheckpointBytes(size_t previous, size_t added, size_t threshold) {
	if (threshold == 0) {
		return true;
	}
	return previous >= threshold || added >= threshold - previous;
}

} // namespace

std::string Fresh::modelPath(const std::string &storageId) const {
	return FreshJoinPath(FreshJoinPath(_rootPath, "models"), storageId);
}

std::string Fresh::modelFile(const std::string &storageId, const char *fileName) const {
	return FreshJoinPath(modelPath(storageId), fileName);
}

FreshResult Fresh::ensureDir(const std::string &path) {
	if (LittleFS.exists(path.c_str())) {
		return FreshResult::success();
	}
	if (!LittleFS.mkdir(path.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create directory");
	}
	return FreshResult::success();
}

FreshResult Fresh::checkFreeSpace(size_t requiredBytes) const {
	const size_t total = LittleFS.totalBytes();
	const size_t used = LittleFS.usedBytes();
	const size_t freeBytes = total > used ? total - used : 0;
	if (freeBytes < _config.minFreeBytes || freeBytes - _config.minFreeBytes < requiredBytes) {
		return FreshResult::failure(FreshStatus::StorageFull, "not enough LittleFS space");
	}
	return FreshResult::success();
}

FreshResult Fresh::checkPayloadSize(size_t payloadBytes, size_t limit, const char *label) const {
	if (payloadBytes == 0 || payloadBytes > FreshMaxPersistedPayloadBytes || payloadBytes > UINT32_MAX ||
	    (limit > 0 && payloadBytes > limit)) {
		std::string message = label != nullptr ? label : "payload";
		message += payloadBytes == 0 ? " is empty" : " size limit exceeded";
		return FreshResult::failure(
		    payloadBytes == 0 ? FreshStatus::InternalError : FreshStatus::SizeLimitExceeded,
		    message.c_str(),
		    payloadBytes
		);
	}
	return FreshResult::success();
}

FreshResult Fresh::readManifest() {
	FreshSlotReadResult manifestSlot = readDurableSlot(
	    _rootPath,
	    FreshManifestFile,
	    FreshMaxPersistedPayloadBytes
	);
	if (!manifestSlot.result) {
		return manifestSlot.result;
	}
	if (manifestSlot.missing) {
		return FreshResult::success("manifest not found");
	}
	FreshResult manifestValid = FreshValidateManifestPayload(manifestSlot.payload);
	if (!manifestValid) {
		return manifestValid;
	}

	JsonArrayConst modelsArray = manifestSlot.payload["models"].as<JsonArrayConst>();
	std::set<std::string> storageIds;
	bool failed = false;
	for (JsonObjectConst modelObject : modelsArray) {
		const char *name = modelObject["name"] | "";
		const char *storageId = modelObject["storageId"] | "";
		const char *type = modelObject["type"] | "general";
		if (!FreshIsValidName(name) || !FreshIsValidName(storageId) ||
		    (strcmp(type, "general") != 0 && strcmp(type, "stream") != 0)) {
			return FreshResult::failure(FreshStatus::CorruptData, "manifest contains invalid model metadata");
		}
		if (_models.find(name) != _models.end() || !storageIds.insert(storageId).second) {
			return FreshResult::failure(FreshStatus::CorruptData, "manifest contains duplicate model metadata");
		}
		auto state = std::make_shared<FreshModel::State>();
		state->name = name;
		state->storageId = storageId;
		state->type = FreshModelTypeFromString(type);
		_models[state->name] = state;
		FreshResult loadResult = loadModel(state);
		FreshLoadStatus loadStatus = loadResult ? FreshLoadStatusFromResult(loadResult)
		                                       : FreshLoadStatus::FailedToLoad;
		FreshModelLoadInfo info;
		info.modelName = state->name;
		info.modelType = state->type;
		info.status = loadStatus;
		info.degraded = loadStatus != FreshLoadStatus::LoadedOk;
		info.message = loadResult.message;
		_diagnostics.modelLoads.push_back(info);
		if (info.degraded) {
			_diagnostics.degradedModelCount++;
		}
		if (!loadResult || loadStatus == FreshLoadStatus::FailedToLoad) {
			failed = true;
		}
	}

	if (failed) {
		return FreshResult::failure(FreshStatus::CorruptData, "failed to load one or more models");
	}
	if (_diagnostics.degradedModelCount > 0) {
		return FreshResult::success("manifest loaded with recovered models", _diagnostics.degradedModelCount);
	}
	if (manifestSlot.hadCorruptSlot) {
		return FreshResult::success("manifest loaded with recovered slot");
	}
	return FreshResult::success();
}

FreshResult Fresh::writeManifest(const JsonDocument &manifest) {
	FreshResult valid = FreshValidateManifestPayload(manifest);
	if (!valid) {
		return valid;
	}
	FreshResult dirResult = ensureDir(_rootPath);
	if (!dirResult) {
		return dirResult;
	}
	const size_t payloadBytes = measureMsgPack(manifest);
	FreshResult sizeResult = checkPayloadSize(payloadBytes, FreshMaxPersistedPayloadBytes, "manifest");
	if (!sizeResult) {
		return sizeResult;
	}

	FreshSlotReadResult slot = readDurableSlot(
	    _rootPath,
	    FreshManifestFile,
	    FreshMaxPersistedPayloadBytes
	);
	if (!slot.result) {
		return slot.result;
	}
	JsonDocument verified(&FreshJsonAllocator());
	FreshResult writeResult = writeDurableSlot(
	    _rootPath,
	    FreshManifestFile,
	    manifest,
	    slot.generation,
	    FreshMaxPersistedPayloadBytes,
	    &verified
	);
	if (!writeResult) {
		return writeResult;
	}
	valid = FreshValidateManifestPayload(verified);
	if (!valid) {
		return valid;
	}
	return FreshResult::success("manifest written");
}

FreshResult Fresh::applyRecord(
    const std::shared_ptr<FreshModel::State> &state,
    const FreshPendingRecord &record
) {
	if (state->type == FreshModelType::Stream) {
		if (record.op == FreshJournalOp::Append) {
			JsonDocument copy;
			FreshResult cloneResult = FreshCloneJson(copy, record.doc.as<JsonVariantConst>(), "stream record");
			if (!cloneResult) {
				return cloneResult;
			}
			state->streamEntries.push_back(std::move(copy));
			while (record.maxEntries > 0 && state->streamEntries.size() > record.maxEntries) {
				state->streamEntries.pop_front();
			}
			state->lastSequence = record.sequence;
			return FreshResult::success();
		}
		return FreshResult::failure(FreshStatus::CorruptData, "invalid stream journal operation");
	}
	if (record.op != FreshJournalOp::Create && record.op != FreshJournalOp::Update &&
	    record.op != FreshJournalOp::Delete) {
		return FreshResult::failure(FreshStatus::CorruptData, "invalid document journal operation");
	}
	if (record.id.empty()) {
		return FreshResult::failure(FreshStatus::CorruptData, "journal document id is missing");
	}

	if (record.op == FreshJournalOp::Delete) {
		state->docs.erase(record.id);
		state->lastSequence = record.sequence;
		return FreshResult::success();
	}

	JsonDocument copy;
	FreshResult cloneResult = FreshCloneJson(copy, record.doc.as<JsonVariantConst>(), "journal document");
	if (!cloneResult) {
		return cloneResult;
	}
	state->docs[record.id] = std::move(copy);
	state->lastSequence = record.sequence;
	return FreshResult::success();
}

FreshResult Fresh::loadSnapshot(const std::shared_ptr<FreshModel::State> &state) {
	FreshSlotReadResult snapshotSlot = readDurableSlot(
	    modelPath(state->storageId),
	    FreshSnapshotFile,
	    _config.maxSnapshotBytes
	);
	if (!snapshotSlot.result) {
		state->degraded = true;
		return snapshotSlot.result;
	}
	if (snapshotSlot.missing) {
		return FreshLoadResult(FreshLoadStatus::LoadedOk, "snapshot not found");
	}
	if (!snapshotSlot.payload["appliedThroughSequence"].is<uint64_t>() ||
	    !snapshotSlot.payload["recordCount"].is<uint64_t>()) {
		state->degraded = true;
		return FreshResult::failure(FreshStatus::CorruptData, "invalid snapshot payload");
	}
	const uint64_t checkpointSequence = snapshotSlot.payload["appliedThroughSequence"].as<uint64_t>();
	const uint64_t declaredRecordCount = snapshotSlot.payload["recordCount"].as<uint64_t>();
	if (checkpointSequence == UINT64_MAX || declaredRecordCount > SIZE_MAX) {
		state->degraded = true;
		return FreshResult::failure(FreshStatus::CorruptData, "invalid snapshot counters");
	}
	FreshResult semanticResult = FreshValidateSnapshotPayload(
	    snapshotSlot.payload,
	    state->storageId,
	    state->type,
	    checkpointSequence,
	    static_cast<size_t>(declaredRecordCount)
	);
	if (!semanticResult) {
		state->degraded = true;
		return semanticResult;
	}

	std::map<std::string, JsonDocument> loadedDocs;
	std::deque<JsonDocument> loadedStreamEntries;
	if (state->type == FreshModelType::Stream) {
		for (JsonVariantConst entry : snapshotSlot.payload["entries"].as<JsonArrayConst>()) {
			JsonDocument copy;
			FreshResult cloneResult = FreshCloneJson(copy, entry, "snapshot stream entry");
			if (!cloneResult) {
				state->degraded = true;
				return cloneResult;
			}
			loadedStreamEntries.push_back(std::move(copy));
		}
	} else {
		for (JsonVariantConst entry : snapshotSlot.payload["docs"].as<JsonArrayConst>()) {
			JsonDocument copy;
			FreshResult cloneResult = FreshCloneJson(copy, entry, "snapshot document");
			if (!cloneResult) {
				state->degraded = true;
				return cloneResult;
			}
			const char *id = copy["_id"] | "";
			if (*id == '\0' || loadedDocs.find(id) != loadedDocs.end()) {
				state->degraded = true;
				return FreshResult::failure(FreshStatus::CorruptData, "snapshot contains invalid document id");
			}
			loadedDocs[id] = std::move(copy);
		}
	}

	state->docs = std::move(loadedDocs);
	state->streamEntries = std::move(loadedStreamEntries);
	state->checkpointSequence = checkpointSequence;
	state->lastSequence = checkpointSequence;
	_nextPendingSequence = std::max(_nextPendingSequence, checkpointSequence + 1);
	if (snapshotSlot.hadCorruptSlot) {
		state->degraded = true;
		return FreshLoadResult(FreshLoadStatus::LoadedWithCorruptSnapshot, "snapshot loaded with recovery");
	}
	return FreshLoadResult(FreshLoadStatus::LoadedOk, "snapshot loaded");
}

FreshResult Fresh::loadJournal(const std::shared_ptr<FreshModel::State> &state) {
	const std::string path = modelFile(state->storageId, FreshJournalFile);
	if (!LittleFS.exists(path.c_str())) {
		return FreshLoadResult(FreshLoadStatus::LoadedOk, "journal not found");
	}

	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		state->degraded = true;
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open journal");
	}

	bool journalRecovered = false;
	bool journalCleanupNeeded = false;
	while (file.available() > 0) {
		uint32_t magic = 0;
		uint16_t version = 0;
		uint8_t opByte = 0;
		uint8_t reserved = 0;
		uint32_t payloadSize = 0;
		uint32_t expectedChecksum = 0;
		if (!FreshReadU32(file, magic) || !FreshReadU16(file, version)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (file.available() < 10) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		opByte = file.read();
		reserved = file.read();
		(void)reserved;
		if (!FreshReadU32(file, payloadSize) || !FreshReadU32(file, expectedChecksum)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (version != FreshJournalVersion) {
			state->degraded = true;
			file.close();
			return FreshResult::failure(FreshStatus::CorruptData, "unsupported journal version");
		}
		if (magic != FreshJournalMagic || payloadSize == 0 ||
		    payloadSize > FreshMaxPersistedPayloadBytes ||
		    payloadSize > _config.maxJournalRecordBytes) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (file.available() < static_cast<int>(payloadSize)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}

		FreshBuffer payload;
		FreshResult allocation = FreshAllocateBuffer(
		    payload,
		    payloadSize,
		    "journal payload",
		    FreshAllocationCategory::JournalPayload
		);
		if (!allocation) {
			file.close();
			return allocation;
		}
		if (file.read(payload.data(), payloadSize) != static_cast<int>(payloadSize)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (FreshChecksum(payload.data(), payload.size()) != expectedChecksum) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}

		JsonDocument recordDoc(&FreshJsonAllocator());
		DeserializationError error = deserializeMsgPack(recordDoc, payload.data(), payload.size());
		if (error || recordDoc.overflowed()) {
			if (error == DeserializationError::NoMemory || recordDoc.overflowed()) {
				file.close();
				return FreshResult::failure(FreshStatus::OutOfMemory, "failed to decode journal record");
			}
			state->degraded = true;
			journalRecovered = true;
			break;
		}

		FreshPendingRecord record;
		if (!FreshParseJournalOp(opByte, record.op)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		const char *payloadOp = recordDoc["op"] | "";
		record.sequence = recordDoc["sequence"] | 0ULL;
		const uint64_t maxEntries = recordDoc["maxEntries"] | 0ULL;
		if (strcmp(payloadOp, FreshJournalOpToString(record.op)) != 0 || record.sequence == 0 ||
		    record.sequence == UINT64_MAX || maxEntries > SIZE_MAX ||
		    (record.op != FreshJournalOp::Append && maxEntries != 0)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		record.maxEntries = static_cast<size_t>(maxEntries);
		_nextPendingSequence = std::max(_nextPendingSequence, record.sequence + 1);
		if (record.sequence <= state->checkpointSequence) {
			journalCleanupNeeded = true;
			continue;
		}
		if (record.sequence <= state->lastSequence) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		record.id = recordDoc["id"] | "";
		if (record.op != FreshJournalOp::Delete) {
			FreshResult cloneResult = FreshCloneJson(
			    record.doc,
			    recordDoc["doc"].as<JsonVariantConst>(),
			    "journal document"
			);
			if (!cloneResult) {
				state->degraded = true;
				file.close();
				return cloneResult;
			}
		}
		FreshResult applyResult = applyRecord(state, record);
		if (!applyResult) {
			state->degraded = true;
			file.close();
			return applyResult;
		}
		state->recordsSinceSnapshot++;
		if (payloadSize > std::numeric_limits<size_t>::max() - state->bytesSinceSnapshot) {
			file.close();
			return FreshResult::failure(FreshStatus::CorruptData, "journal byte counter overflow");
		}
		state->bytesSinceSnapshot += payloadSize;
	}
	file.close();
	if (journalRecovered || journalCleanupNeeded) {
		state->snapshotRequired = true;
	}
	return FreshLoadResult(
	    journalRecovered ? FreshLoadStatus::LoadedWithRecoveredJournal : FreshLoadStatus::LoadedOk,
	    journalRecovered ? "journal recovered with corruption" : "journal loaded"
	);
}

FreshResult Fresh::loadModel(const std::shared_ptr<FreshModel::State> &state) {
	FreshResult snapshotResult = loadSnapshot(state);
	const bool snapshotFailed = !snapshotResult;
	FreshResult journalResult = loadJournal(state);
	const bool journalFailed = !journalResult;
	const std::string journalMessage = journalResult.message;

	FreshLoadStatus status = FreshLoadStatus::LoadedOk;
	const char *message = "model loaded";
	if (snapshotFailed && journalFailed) {
		status = FreshLoadStatus::FailedToLoad;
		message = "failed to load snapshot and journal";
	} else if (snapshotFailed && journalMessage == "journal not found") {
		status = FreshLoadStatus::FailedToLoad;
		message = "failed to load snapshot and journal";
	} else if (snapshotFailed) {
		status = FreshLoadStatus::LoadedWithCorruptSnapshot;
		message = "model loaded with corrupt snapshot";
	} else if (journalFailed) {
		status = FreshLoadStatus::FailedToLoad;
		message = "failed to load journal";
	} else {
		FreshLoadStatus snapshotStatus = FreshLoadStatusFromResult(snapshotResult);
		FreshLoadStatus journalStatus = FreshLoadStatusFromResult(journalResult);
		if (snapshotStatus == FreshLoadStatus::LoadedWithCorruptSnapshot) {
			status = snapshotStatus;
			message = "model loaded with recovered snapshot slot";
		} else if (journalStatus == FreshLoadStatus::LoadedWithRecoveredJournal) {
			status = journalStatus;
			message = "model loaded with recovered journal";
		}
	}

	state->dirty = state->snapshotRequired;
	state->pending.clear();
	state->degraded = status != FreshLoadStatus::LoadedOk;
	if (status == FreshLoadStatus::FailedToLoad) {
		return FreshResult::failure(FreshStatus::CorruptData, message, static_cast<size_t>(status));
	}
	return FreshLoadResult(status, message);
}

FreshResult Fresh::recordToJson(const FreshPendingRecord &record, JsonDocument &out) {
	JsonDocument doc(&FreshJsonAllocator());
	FreshResult result = FreshJsonSet(doc["sequence"], record.sequence, doc, "journal sequence");
	if (!result) return result;
	result = FreshJsonSet(doc["op"], FreshJournalOpToString(record.op), doc, "journal operation");
	if (!result) return result;
	result = FreshJsonSet(doc["id"], record.id, doc, "journal id");
	if (!result) return result;
	if (record.op == FreshJournalOp::Delete) {
		result = FreshJsonSet(doc["doc"], nullptr, doc, "journal document");
	} else {
		result = FreshJsonSet(
		    doc["doc"],
		    record.doc.as<JsonVariantConst>(),
		    doc,
		    "journal document"
		);
	}
	if (!result) return result;
	if (record.op == FreshJournalOp::Append && record.maxEntries > 0) {
		result = FreshJsonSet(doc["maxEntries"], record.maxEntries, doc, "journal retention");
		if (!result) return result;
	}
	result = FreshValidateJsonDocument(doc, "journal record");
	if (!result) return result;
	out = std::move(doc);
	return FreshResult::success("journal record constructed");
}

FreshResult Fresh::syncDirty(bool force) {
	FreshLock syncLock(*_syncMutex);
	if (!syncLock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock sync");
	}

	JsonDocument manifest(&FreshJsonAllocator());
	bool shouldWriteManifest = false;
	uint32_t manifestEpoch = 0;
	size_t requiredBytes = 0;
	std::vector<FreshModelSyncBatch> activeBatches;
	std::vector<FreshModelSyncBatch> droppedBatches;
	{
		FreshLock lock(*_mutex);
		if (!lock || !_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}

		std::set<std::string> usedStorageIds;
		for (const auto &entry : _models) {
			if (!entry.second->storageId.empty()) {
				usedStorageIds.insert(entry.second->storageId);
			}
		}
		for (const auto &entry : _models) {
			const auto &state = entry.second;
			if (!state->dropped && state->storageId.empty()) {
				do {
					state->storageId = FreshMakeId();
				} while (usedStorageIds.find(state->storageId) != usedStorageIds.end() ||
				         LittleFS.exists(modelPath(state->storageId).c_str()));
				usedStorageIds.insert(state->storageId);
				state->storageEpoch++;
				state->dirty = true;
				state->snapshotRequired = true;
				_manifestDirty = true;
				_manifestEpoch++;
			}
		}

		shouldWriteManifest = _manifestDirty;
		manifestEpoch = _manifestEpoch;
		if (shouldWriteManifest) {
			size_t modelCount = 0;
			for (const auto &entry : _models) {
				if (!entry.second->dropped) modelCount++;
			}
			FreshResult jsonResult = FreshJsonSet(
			    manifest["version"],
			    FreshManifestVersion,
			    manifest,
			    "manifest version"
			);
			if (!jsonResult) return jsonResult;
			jsonResult = FreshJsonSet(manifest["modelCount"], modelCount, manifest, "manifest count");
			if (!jsonResult) return jsonResult;
			JsonArray modelsArray;
			jsonResult = FreshJsonCreateArray(manifest["models"], manifest, modelsArray, "manifest models");
			if (!jsonResult) return jsonResult;
			for (const auto &entry : _models) {
				const auto &state = entry.second;
				if (state->dropped) {
					continue;
				}
				JsonObject modelObject;
				jsonResult = FreshJsonAddObject(modelsArray, manifest, modelObject, "manifest model");
				if (!jsonResult) return jsonResult;
				jsonResult = FreshJsonSet(modelObject["name"], state->name, manifest, "manifest model name");
				if (!jsonResult) return jsonResult;
				jsonResult = FreshJsonSet(
				    modelObject["storageId"],
				    state->storageId,
				    manifest,
				    "manifest storage id"
				);
				if (!jsonResult) return jsonResult;
				jsonResult = FreshJsonSet(
				    modelObject["type"],
				    FreshModelTypeToString(state->type),
				    manifest,
				    "manifest model type"
				);
				if (!jsonResult) return jsonResult;
			}
			jsonResult = FreshValidateManifestPayload(manifest);
			if (!jsonResult) return jsonResult;
			const size_t manifestPayloadBytes = measureMsgPack(manifest);
			FreshResult manifestSize = checkPayloadSize(
			    manifestPayloadBytes,
			    FreshMaxPersistedPayloadBytes,
			    "manifest"
			);
			if (!manifestSize) {
				return manifestSize;
			}
			if (!FreshAddRequiredBytes(requiredBytes, FreshSlotHeaderSize) ||
			    !FreshAddRequiredBytes(requiredBytes, manifestPayloadBytes)) {
				return FreshResult::failure(FreshStatus::SizeLimitExceeded, "sync size calculation overflow");
			}
		}

		for (const auto &entry : _models) {
			const auto &state = entry.second;
			if (!state->dirty && state->pending.empty() && !state->snapshotRequired) {
				continue;
			}

			FreshModelSyncBatch batch;
			batch.state = std::static_pointer_cast<void>(state);
			batch.name = state->name;
			batch.storageId = state->storageId;
			batch.path = modelPath(batch.storageId);
			batch.journalPath = modelFile(batch.storageId, FreshJournalFile);
			batch.type = state->type;
			batch.dropped = state->dropped;
			batch.storageEpoch = state->storageEpoch;
			batch.checkpointSequence = state->lastSequence;
			batch.maxSnapshotBytes = _config.maxSnapshotBytes;

			if (batch.dropped) {
				droppedBatches.push_back(std::move(batch));
				continue;
			}

			size_t pendingBytes = 0;
			for (const FreshPendingRecord &pending : state->pending) {
				JsonDocument recordDoc(&FreshJsonAllocator());
				FreshResult recordResult = recordToJson(pending, recordDoc);
				if (!recordResult) {
					return recordResult;
				}
				const size_t recordBytes = measureMsgPack(recordDoc);
				FreshResult sizeResult = checkPayloadSize(
				    recordBytes,
				    _config.maxJournalRecordBytes,
				    "journal record"
				);
				if (!sizeResult) {
					return sizeResult;
				}
				FreshJournalSyncRecord record;
				record.sequence = pending.sequence;
				record.op = pending.op;
				FreshResult allocation = FreshAllocateBuffer(
				    record.payload,
				    recordBytes,
				    "journal record",
				    FreshAllocationCategory::JournalPayload
				);
				if (!allocation) {
					return allocation;
				}
				const size_t written = serializeMsgPack(
				    recordDoc,
				    record.payload.data(),
				    record.payload.size()
				);
				if (written != record.payload.size()) {
					return FreshResult::failure(FreshStatus::InternalError, "failed to serialize journal record");
				}
				if (!FreshAddRequiredBytes(pendingBytes, record.payload.size()) ||
				    !FreshAddRequiredBytes(requiredBytes, FreshJournalHeaderSize) ||
				    !FreshAddRequiredBytes(requiredBytes, record.payload.size())) {
					return FreshResult::failure(FreshStatus::SizeLimitExceeded, "sync size calculation overflow");
				}
				batch.records.push_back(std::move(record));
			}

			batch.writeSnapshot =
			    force || state->snapshotRequired ||
			    FreshShouldCheckpointRecords(
			        state->recordsSinceSnapshot,
			        batch.records.size(),
			        _config.snapshotRecordThreshold
			    ) ||
			    FreshShouldCheckpointBytes(
			        state->bytesSinceSnapshot,
			        pendingBytes,
			        _config.snapshotBytesThreshold
			    );
			if (batch.writeSnapshot) {
				batch.snapshotRecordCount = state->type == FreshModelType::Stream
				                                ? state->streamEntries.size()
				                                : state->docs.size();
				FreshResult jsonResult = FreshJsonSet(
				    batch.snapshot["version"],
				    FreshSnapshotVersion,
				    batch.snapshot,
				    "snapshot version"
				);
				if (!jsonResult) return jsonResult;
				jsonResult = FreshJsonSet(
				    batch.snapshot["storageId"],
				    batch.storageId,
				    batch.snapshot,
				    "snapshot storage id"
				);
				if (!jsonResult) return jsonResult;
				jsonResult = FreshJsonSet(
				    batch.snapshot["type"],
				    FreshModelTypeToString(batch.type),
				    batch.snapshot,
				    "snapshot type"
				);
				if (!jsonResult) return jsonResult;
				jsonResult = FreshJsonSet(
				    batch.snapshot["appliedThroughSequence"],
				    batch.checkpointSequence,
				    batch.snapshot,
				    "snapshot sequence"
				);
				if (!jsonResult) return jsonResult;
				jsonResult = FreshJsonSet(
				    batch.snapshot["recordCount"],
				    batch.snapshotRecordCount,
				    batch.snapshot,
				    "snapshot count"
				);
				if (!jsonResult) return jsonResult;

				JsonArray records;
				const char *arrayName = batch.type == FreshModelType::Stream ? "entries" : "docs";
				jsonResult = FreshJsonCreateArray(
				    batch.snapshot[arrayName],
				    batch.snapshot,
				    records,
				    "snapshot records"
				);
				if (!jsonResult) return jsonResult;
				if (batch.type == FreshModelType::Stream) {
					for (const JsonDocument &streamEntry : state->streamEntries) {
						jsonResult = FreshJsonAdd(
						    records,
						    streamEntry.as<JsonVariantConst>(),
						    batch.snapshot,
						    "snapshot stream entry"
						);
						if (!jsonResult) return jsonResult;
					}
				} else {
					for (const auto &docEntry : state->docs) {
						jsonResult = FreshJsonAdd(
						    records,
						    docEntry.second.as<JsonVariantConst>(),
						    batch.snapshot,
						    "snapshot document"
						);
						if (!jsonResult) return jsonResult;
					}
				}
				jsonResult = FreshValidateSnapshotPayload(
				    batch.snapshot,
				    batch.storageId,
				    batch.type,
				    batch.checkpointSequence,
				    batch.snapshotRecordCount
				);
				if (!jsonResult) return jsonResult;
				batch.snapshotPayloadBytes = measureMsgPack(batch.snapshot);
				FreshResult snapshotSizeResult = checkPayloadSize(
				    batch.snapshotPayloadBytes,
				    _config.maxSnapshotBytes,
				    "snapshot"
				);
				if (!snapshotSizeResult) {
					return snapshotSizeResult;
				}
				if (!FreshAddRequiredBytes(requiredBytes, FreshSlotHeaderSize) ||
				    !FreshAddRequiredBytes(requiredBytes, batch.snapshotPayloadBytes)) {
					return FreshResult::failure(FreshStatus::SizeLimitExceeded, "sync size calculation overflow");
				}
			}

			activeBatches.push_back(std::move(batch));
		}
		if (!shouldWriteManifest && activeBatches.empty() && droppedBatches.empty()) {
			return FreshResult::success("nothing dirty");
		}
	}

	emitEvent({.type = FreshEventType::SyncStarted, .result = FreshResult::success("sync started")});

	FreshResult last = FreshResult::success("nothing dirty");
	FreshResult spaceResult = checkFreeSpace(requiredBytes);
	if (!spaceResult) {
		emitSync(spaceResult);
		return spaceResult;
	}

	// Active storage is made durable before a manifest can point at it.
	for (const FreshModelSyncBatch &batch : activeBatches) {
		FreshModelSyncResult syncResult = FreshWriteActiveModelBatch(batch);
		{
			FreshLock lock(*_mutex);
			auto state = std::static_pointer_cast<FreshModel::State>(batch.state);
			size_t recordsPopped = 0;
			while (recordsPopped < syncResult.recordsWritten && !state->pending.empty() &&
			       state->pending.front().sequence == batch.records[recordsPopped].sequence) {
				state->pending.pop_front();
				recordsPopped++;
			}

			if (state->storageEpoch == batch.storageEpoch) {
				if (syncResult.snapshotWritten) {
					state->recordsSinceSnapshot = 0;
					state->bytesSinceSnapshot = 0;
					state->checkpointSequence = batch.checkpointSequence;
				} else {
					if (syncResult.recordsWritten > UINT32_MAX - state->recordsSinceSnapshot) {
						state->snapshotRequired = true;
					} else {
						state->recordsSinceSnapshot += static_cast<uint32_t>(syncResult.recordsWritten);
					}
					if (syncResult.bytesWritten > std::numeric_limits<size_t>::max() - state->bytesSinceSnapshot) {
						state->snapshotRequired = true;
					} else {
						state->bytesSinceSnapshot += syncResult.bytesWritten;
					}
				}
			}

			if (syncResult.result && state->storageEpoch == batch.storageEpoch) {
				if (syncResult.snapshotWritten) {
					state->snapshotRequired = false;
				}
				state->dirty = state->dropped || !state->pending.empty() || state->snapshotRequired;
			} else {
				if (syncResult.snapshotWritten && state->storageEpoch == batch.storageEpoch) {
					state->snapshotRequired = true;
				}
				state->dirty = true;
			}
		}

		last = syncResult.result;
		if (!last) {
			emitSync(last);
			return last;
		}
	}

	// The manifest is the commit point for create, rename, drop, and import.
	if (shouldWriteManifest) {
		last = writeManifest(manifest);
		{
			FreshLock lock(*_mutex);
			if (last && _manifestEpoch == manifestEpoch) {
				_manifestDirty = false;
			}
		}
		if (!last) {
			emitSync(last);
			return last;
		}
	}

	// Once the committed manifest no longer references a dropped model, its
	// immutable storage directory can be removed without risking data loss.
	for (const FreshModelSyncBatch &batch : droppedBatches) {
		FreshModelSyncResult syncResult = FreshDeleteModelBatch(batch);
		{
			FreshLock lock(*_mutex);
			auto state = std::static_pointer_cast<FreshModel::State>(batch.state);
			if (syncResult.result && state->storageEpoch == batch.storageEpoch && state->dropped) {
				state->dirty = false;
				state->pending.clear();
				state->snapshotRequired = false;
				auto found = _models.find(batch.name);
				if (found != _models.end() && found->second == state) {
					_models.erase(found);
				}
			} else {
				state->dirty = true;
			}
		}
		last = syncResult.result;
		if (!last) {
			emitSync(last);
			return last;
		}
	}

	emitEvent({.type = FreshEventType::SyncFinished, .result = last});
	emitSync(last);
	return last;
}

FreshResult Fresh::flush() {
	{
		FreshLock lock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}
	return syncDirty(false);
}

FreshResult Fresh::forceSyncAsync() {
	TaskHandle_t handle = nullptr;
	{
		FreshLock lock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		_forceSyncRequested = true;
		handle = _syncTaskHandle;
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}
	return FreshResult::success("sync requested");
}

FreshResult Fresh::forceSync() {
	{
		FreshLock lock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}
	return syncDirty(true);
}
