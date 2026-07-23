#include "Fresh.h"
#include "FreshRestoreTesting.h"
#include "internal/FreshBackupArchive.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <LittleFS.h>

#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

class DurableRestoreVisitor : public FreshBackupArchiveVisitor {
  public:
	using ModelCallback = std::function<FreshResult(const FreshBackupModelMetadata &)>;
	using RecordCallback = std::function<FreshResult(FreshModelType, JsonDocument &&)>;

	DurableRestoreVisitor(
	    ModelCallback onModelBegin,
	    RecordCallback onRecord,
	    ModelCallback onModelEnd
	)
	    : _onModelBegin(std::move(onModelBegin)),
	      _onRecord(std::move(onRecord)),
	      _onModelEnd(std::move(onModelEnd)) {
	}

	FreshResult onModelBegin(const FreshBackupModelMetadata &model) override {
		return _onModelBegin ? _onModelBegin(model) : FreshResult::success();
	}

	FreshResult onRecord(FreshModelType type, JsonDocument &&document) override {
		return _onRecord ? _onRecord(type, std::move(document)) : FreshResult::success();
	}

	FreshResult onModelEnd(const FreshBackupModelMetadata &model) override {
		return _onModelEnd ? _onModelEnd(model) : FreshResult::success();
	}

  private:
	ModelCallback _onModelBegin;
	RecordCallback _onRecord;
	ModelCallback _onModelEnd;
};

enum class FreshRestoreManifestCommitState : uint8_t {
	NotCommitted,
	Committed,
	Unknown,
};

enum class FreshRestoreSlotVerification : uint8_t {
	Exact,
	Invalid,
	Unavailable,
};

struct FreshRestoreSlotProbe {
	bool exists = false;
	bool valid = false;
	uint64_t generation = 0;
};

bool FreshRestoreModeKnown(FreshRestoreMode mode) {
	switch (mode) {
	case FreshRestoreMode::ReplaceSelected:
	case FreshRestoreMode::ReplaceAll:
		return true;
	}
	return false;
}

bool FreshRestoreAddBytes(size_t &total, size_t value) {
	if (value > std::numeric_limits<size_t>::max() - total) return false;
	total += value;
	return true;
}

std::string FreshRestoreDurableSlotPath(
    const std::string &basePath,
    const char *fileBaseName,
    char slot
) {
	std::string name = fileBaseName;
	name += ".";
	name.push_back(slot);
	name += ".msgpack";
	return FreshJoinPath(basePath, name);
}

std::string FreshRestoreSnapshotSlotPath(const std::string &modelPath, char slot) {
	return FreshRestoreDurableSlotPath(modelPath, FreshSnapshotFile, slot);
}

bool FreshRemoveRestoreStorage(const std::string &modelPath) {
	LittleFS.remove(FreshJoinPath(modelPath, FreshJournalFile).c_str());
	LittleFS.remove(FreshRestoreSnapshotSlotPath(modelPath, 'a').c_str());
	LittleFS.remove(FreshRestoreSnapshotSlotPath(modelPath, 'b').c_str());
	return !LittleFS.exists(modelPath.c_str()) || LittleFS.rmdir(modelPath.c_str());
}

FreshResult FreshValidateRestoreManifest(const JsonDocument &manifest) {
	if ((manifest["version"] | 0U) != FreshManifestVersion ||
	    !manifest["modelCount"].is<uint64_t>() ||
	    !manifest["models"].is<JsonArrayConst>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "invalid restore manifest");
	}

	const uint64_t declaredCount = manifest["modelCount"].as<uint64_t>();
	JsonArrayConst models = manifest["models"].as<JsonArrayConst>();
	if (declaredCount > SIZE_MAX || static_cast<size_t>(declaredCount) != models.size()) {
		return FreshResult::failure(FreshStatus::CorruptData, "restore manifest count mismatch");
	}

	std::set<std::string> names;
	std::set<std::string> storageIds;
	for (JsonObjectConst model : models) {
		const char *name = model["name"] | "";
		const char *storageId = model["storageId"] | "";
		const char *type = model["type"] | "";
		if (!FreshIsValidName(name) || !FreshIsValidName(storageId) ||
		    (strcmp(type, "general") != 0 && strcmp(type, "stream") != 0)) {
			return FreshResult::failure(FreshStatus::CorruptData, "invalid restore manifest model");
		}
		if (!names.insert(name).second || !storageIds.insert(storageId).second) {
			return FreshResult::failure(FreshStatus::CorruptData, "duplicate restore manifest model");
		}
	}
	return FreshResult::success("restore manifest valid");
}

FreshResult FreshValidateRestoreSnapshot(
    const JsonDocument &payload,
    const std::string &storageId,
    FreshModelType type,
    size_t expectedRecordCount
) {
	FreshResult jsonResult = FreshValidateJsonDocument(payload, "restore snapshot");
	if (!jsonResult) return jsonResult;

	const char *payloadStorageId = payload["storageId"] | "";
	const char *payloadType = payload["type"] | "";
	if ((payload["version"] | 0U) != FreshSnapshotVersion ||
	    storageId != payloadStorageId ||
	    strcmp(payloadType, FreshModelTypeToString(type)) != 0 ||
	    !payload["appliedThroughSequence"].is<uint64_t>() ||
	    payload["appliedThroughSequence"].as<uint64_t>() != 0 ||
	    !payload["recordCount"].is<uint64_t>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "invalid restore snapshot metadata");
	}

	const char *arrayName = type == FreshModelType::Stream ? "entries" : "docs";
	if (!payload[arrayName].is<JsonArrayConst>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "restore snapshot records are missing");
	}
	const uint64_t declaredCount = payload["recordCount"].as<uint64_t>();
	JsonArrayConst records = payload[arrayName].as<JsonArrayConst>();
	if (declaredCount > SIZE_MAX || static_cast<size_t>(declaredCount) != records.size() ||
	    records.size() != expectedRecordCount) {
		return FreshResult::failure(FreshStatus::CorruptData, "restore snapshot record count mismatch");
	}

	std::set<std::string> documentIds;
	for (JsonVariantConst record : records) {
		if (!record.is<JsonObjectConst>()) {
			return FreshResult::failure(FreshStatus::CorruptData, "restore snapshot record is not an object");
		}
		if (type == FreshModelType::General) {
			const char *id = record["_id"] | "";
			if (*id == '\0' || !documentIds.insert(id).second) {
				return FreshResult::failure(FreshStatus::CorruptData, "invalid restore snapshot document id");
			}
		}
	}
	return FreshResult::success("restore snapshot valid");
}

FreshResult FreshEncodeRestorePayload(
    const JsonDocument &payload,
    size_t maxBytes,
    const char *label,
    FreshBuffer &encoded
) {
	FreshResult valid = FreshValidateJsonDocument(payload, label);
	if (!valid) return valid;
	const size_t payloadBytes = measureMsgPack(payload);
	if (payloadBytes == 0 || payloadBytes > maxBytes ||
	    payloadBytes > FreshMaxPersistedPayloadBytes || payloadBytes > UINT32_MAX) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "restore payload size limit exceeded");
	}
	if (!encoded.allocate(payloadBytes, FreshAllocationCategory::DurableSlotPayload)) {
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate restore payload");
	}
	if (serializeMsgPack(payload, encoded.data(), encoded.size()) != encoded.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to serialize restore payload");
	}
	return FreshResult::success("restore payload encoded");
}

FreshRestoreSlotVerification FreshVerifyExactRestoreSlot(
    const std::string &path,
    uint64_t expectedGeneration,
    const FreshBuffer &expected
) {
	File input = LittleFS.open(path.c_str(), "r");
	if (!input) return FreshRestoreSlotVerification::Unavailable;

	uint32_t magic = 0;
	uint16_t version = 0;
	uint64_t generation = 0;
	uint32_t payloadSize = 0;
	uint32_t expectedChecksum = 0;
	const bool headerOk = FreshReadU32(input, magic) && FreshReadU16(input, version) &&
	                      FreshReadU64(input, generation) && FreshReadU32(input, payloadSize) &&
	                      FreshReadU32(input, expectedChecksum);
	if (!headerOk || magic != FreshSlotMagic || version != FreshSlotVersion ||
	    generation != expectedGeneration || payloadSize != expected.size() ||
	    input.available() != static_cast<int>(payloadSize)) {
		input.close();
		return FreshRestoreSlotVerification::Invalid;
	}

	uint32_t checksum = 2166136261u;
	uint8_t buffer[256];
	size_t offset = 0;
	while (offset < expected.size()) {
		const size_t remaining = expected.size() - offset;
		const size_t chunkSize = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		const int read = input.read(buffer, chunkSize);
		if (read != static_cast<int>(chunkSize)) {
			input.close();
			return FreshRestoreSlotVerification::Invalid;
		}
		for (size_t i = 0; i < chunkSize; ++i) {
			if (buffer[i] != expected.data()[offset + i]) {
				input.close();
				return FreshRestoreSlotVerification::Invalid;
			}
			checksum ^= buffer[i];
			checksum *= 16777619u;
		}
		offset += chunkSize;
	}
	const bool trailing = input.available() != 0;
	input.close();
	if (trailing || checksum != expectedChecksum || checksum != FreshChecksum(expected.data(), expected.size())) {
		return FreshRestoreSlotVerification::Invalid;
	}
	return FreshRestoreSlotVerification::Exact;
}

FreshResult FreshProbeRestoreManifestSlot(
    const std::string &path,
    FreshRestoreSlotProbe &probe
) {
	probe = FreshRestoreSlotProbe();
	probe.exists = LittleFS.exists(path.c_str());
	if (!probe.exists) return FreshResult::success("manifest slot missing");

	File input = LittleFS.open(path.c_str(), "r");
	if (!input) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open manifest slot");
	}
	uint32_t magic = 0;
	uint16_t version = 0;
	uint64_t generation = 0;
	uint32_t payloadSize = 0;
	uint32_t checksum = 0;
	const bool headerOk = FreshReadU32(input, magic) && FreshReadU16(input, version) &&
	                      FreshReadU64(input, generation) && FreshReadU32(input, payloadSize) &&
	                      FreshReadU32(input, checksum);
	if (!headerOk || magic != FreshSlotMagic || version != FreshSlotVersion || payloadSize == 0 ||
	    payloadSize > FreshMaxPersistedPayloadBytes || input.available() != static_cast<int>(payloadSize)) {
		input.close();
		return FreshResult::success("manifest slot invalid");
	}

	FreshBuffer bytes;
	if (!bytes.allocate(payloadSize, FreshAllocationCategory::DurableSlotPayload)) {
		input.close();
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate manifest probe payload");
	}
	const int read = input.read(bytes.data(), bytes.size());
	const bool trailing = input.available() != 0;
	input.close();
	if (read != static_cast<int>(bytes.size()) || trailing ||
	    FreshChecksum(bytes.data(), bytes.size()) != checksum) {
		return FreshResult::success("manifest slot invalid");
	}

	JsonDocument manifest(&FreshJsonAllocator());
	DeserializationError decode = deserializeMsgPack(manifest, bytes.data(), bytes.size());
	if (decode || manifest.overflowed()) {
		if (decode == DeserializationError::NoMemory || manifest.overflowed()) {
			return FreshResult::failure(FreshStatus::OutOfMemory, "failed to decode manifest probe");
		}
		return FreshResult::success("manifest slot invalid");
	}
	FreshResult valid = FreshValidateRestoreManifest(manifest);
	if (!valid) return FreshResult::success("manifest slot invalid");
	probe.valid = true;
	probe.generation = generation;
	return FreshResult::success("manifest slot valid");
}

FreshResult FreshCommitRestoreManifest(
    const std::string &rootPath,
    const JsonDocument &manifest,
    FreshRestoreManifestCommitState &commitState
) {
	commitState = FreshRestoreManifestCommitState::NotCommitted;
	FreshResult valid = FreshValidateRestoreManifest(manifest);
	if (!valid) return valid;

	FreshBuffer encoded;
	FreshResult encodedResult = FreshEncodeRestorePayload(
	    manifest,
	    FreshMaxPersistedPayloadBytes,
	    "restore manifest",
	    encoded
	);
	if (!encodedResult) return encodedResult;

	const std::string slotA = FreshRestoreDurableSlotPath(rootPath, FreshManifestFile, 'a');
	const std::string slotB = FreshRestoreDurableSlotPath(rootPath, FreshManifestFile, 'b');
	FreshRestoreSlotProbe probeA;
	FreshRestoreSlotProbe probeB;
	FreshResult probeResult = FreshProbeRestoreManifestSlot(slotA, probeA);
	if (!probeResult) return probeResult;
	probeResult = FreshProbeRestoreManifestSlot(slotB, probeB);
	if (!probeResult) return probeResult;

	const bool anyExisting = probeA.exists || probeB.exists;
	if (anyExisting && !probeA.valid && !probeB.valid) {
		return FreshResult::failure(FreshStatus::CorruptData, "no valid manifest slot before restore");
	}
	uint64_t currentGeneration = 0;
	if (probeA.valid && probeA.generation > currentGeneration) currentGeneration = probeA.generation;
	if (probeB.valid && probeB.generation > currentGeneration) currentGeneration = probeB.generation;
	if (currentGeneration == UINT64_MAX) {
		return FreshResult::failure(FreshStatus::InternalError, "manifest generation overflow");
	}
	const uint64_t nextGeneration = currentGeneration + 1;
	const char targetSlot = nextGeneration % 2 == 0 ? 'a' : 'b';
	const std::string targetPath = targetSlot == 'a' ? slotA : slotB;

	File output = LittleFS.open(targetPath.c_str(), "w");
	if (!output) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open restore manifest slot");
	}
	FreshWriteU32(output, FreshSlotMagic);
	FreshWriteU16(output, FreshSlotVersion);
	FreshWriteU64(output, nextGeneration);
	FreshWriteU32(output, static_cast<uint32_t>(encoded.size()));
	FreshWriteU32(output, FreshChecksum(encoded.data(), encoded.size()));
	const size_t written = output.write(encoded.data(), encoded.size());
	output.flush();
	output.close();

	const FreshRestoreSlotVerification verification = FreshVerifyExactRestoreSlot(
	    targetPath,
	    nextGeneration,
	    encoded
	);
	if (verification == FreshRestoreSlotVerification::Exact) {
		commitState = FreshRestoreManifestCommitState::Committed;
		return FreshResult::success("restore manifest committed");
	}
	if (verification == FreshRestoreSlotVerification::Invalid) {
		commitState = FreshRestoreManifestCommitState::NotCommitted;
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    written == encoded.size()
		        ? "restore manifest verification failed"
		        : "restore manifest write was incomplete"
		);
	}

	// The slot could be complete even though verification could not reopen it.
	// Retaining staged snapshots is mandatory because reboot will resolve which
	// manifest generation is valid.
	commitState = FreshRestoreManifestCommitState::Unknown;
	return FreshResult::failure(
	    FreshStatus::FileSystemError,
	    "restore manifest commit could not be verified; reboot required"
	);
}

FreshResult FreshWriteRestoreSnapshot(
    const std::string &modelPath,
    const std::string &storageId,
    FreshModelType type,
    size_t expectedRecordCount,
    const JsonDocument &payload,
    size_t maxSnapshotBytes
) {
	FreshResult semanticResult = FreshValidateRestoreSnapshot(
	    payload,
	    storageId,
	    type,
	    expectedRecordCount
	);
	if (!semanticResult) return semanticResult;

	FreshBuffer encoded;
	FreshResult encodedResult = FreshEncodeRestorePayload(
	    payload,
	    maxSnapshotBytes,
	    "restore snapshot",
	    encoded
	);
	if (!encodedResult) return encodedResult;

	if (LittleFS.exists(modelPath.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "restore storage path already exists");
	}
	if (!LittleFS.mkdir(modelPath.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create restore storage directory");
	}

	const std::string slotPath = FreshRestoreSnapshotSlotPath(modelPath, 'b');
	File output = LittleFS.open(slotPath.c_str(), "w");
	if (!output) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open restore snapshot slot");
	}
	FreshWriteU32(output, FreshSlotMagic);
	FreshWriteU16(output, FreshSlotVersion);
	FreshWriteU64(output, 1);
	FreshWriteU32(output, static_cast<uint32_t>(encoded.size()));
	FreshWriteU32(output, FreshChecksum(encoded.data(), encoded.size()));
	const size_t written = output.write(encoded.data(), encoded.size());
	output.flush();
	output.close();
	if (written != encoded.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write restore snapshot");
	}
	if (FreshVerifyExactRestoreSlot(slotPath, 1, encoded) != FreshRestoreSlotVerification::Exact) {
		return FreshResult::failure(FreshStatus::CorruptData, "restore snapshot verification failed");
	}
	return FreshResult::success("restore snapshot written");
}

#if defined(FRESH_TESTING)
FreshRestoreTestFailurePoint FreshRestoreFailurePoint = FreshRestoreTestFailurePoint::None;

bool FreshRestoreShouldFail(FreshRestoreTestFailurePoint point) {
	if (FreshRestoreFailurePoint != point) return false;
	FreshRestoreFailurePoint = FreshRestoreTestFailurePoint::None;
	return true;
}
#else
bool FreshRestoreShouldFail(FreshRestoreTestFailurePoint) {
	return false;
}
#endif

} // namespace

#if defined(FRESH_TESTING)
void FreshTestConfigureRestoreFailure(FreshRestoreTestFailurePoint point) {
	FreshRestoreFailurePoint = point;
}

void FreshTestResetRestoreFailure() {
	FreshRestoreFailurePoint = FreshRestoreTestFailurePoint::None;
}
#endif

FreshResult Fresh::restoreBackup(Stream &input, const FreshRestoreOptions &options) {
	FreshLock backupOperationLock(_backup->mutex);
	if (!backupOperationLock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock backup state");
	}
	if (_backup->running || _backup->requested) {
		return FreshResult::failure(FreshStatus::Busy, "backup already running");
	}
	if (!FreshRestoreModeKnown(options.mode)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "invalid restore mode");
	}
	{
		FreshLock lock(*_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}

	// Preserved and protected states keep their existing storage IDs, so the
	// current registry must be fully durable before restore planning begins.
	FreshResult syncResult = forceSync();
	if (!syncResult) return syncResult;

	FreshLock restoreSyncLock(*_syncMutex);
	if (!restoreSyncLock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock restore storage");
	}

	uint64_t capturedDatabaseRevision = 0;
	size_t maxDocumentBytes = 0;
	size_t maxSnapshotBytes = 0;
	std::map<std::string, std::shared_ptr<FreshModel::State>> oldModels;
	std::map<std::string, uint64_t> oldRevisions;
	std::set<std::string> protectedNames;
	{
		FreshLock lock(*_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		if (_manifestDirty) {
			return FreshResult::failure(FreshStatus::Busy, "database changed during restore preflight");
		}
		for (const auto &entry : _models) {
			const auto &state = entry.second;
			if (!state || state->dropped || state->storageId.empty() || state->dirty ||
			    !state->pending.empty() || state->snapshotRequired) {
				return FreshResult::failure(FreshStatus::Busy, "database changed during restore preflight");
			}
		}
		for (const std::string &name : options.protectedModels) {
			if (!FreshIsValidName(name.c_str())) {
				return FreshResult::failure(FreshStatus::InvalidArgument, "invalid protected model name");
			}
			if (!protectedNames.insert(name).second) {
				return FreshResult::failure(FreshStatus::InvalidArgument, "duplicate protected model name");
			}
			auto found = _models.find(name);
			if (found == _models.end() || !found->second || found->second->dropped) {
				return FreshResult::failure(FreshStatus::ModelNotFound, "protected model not found");
			}
		}
		capturedDatabaseRevision = _databaseRevision;
		maxDocumentBytes = _config.maxDocumentBytes;
		maxSnapshotBytes = _config.maxSnapshotBytes;
		oldModels = _models;
		for (const auto &entry : oldModels) oldRevisions[entry.first] = entry.second->revision;
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> importedModels;
	std::shared_ptr<FreshModel::State> currentModel;
	DurableRestoreVisitor visitor(
	    [&](const FreshBackupModelMetadata &model) -> FreshResult {
		    if (protectedNames.find(model.name) != protectedNames.end()) {
			    return FreshResult::failure(FreshStatus::InvalidArgument, "backup contains a protected model");
		    }
		    currentModel = std::make_shared<FreshModel::State>();
		    if (!currentModel) {
			    return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate restored model");
		    }
		    currentModel->name = model.name;
		    currentModel->type = model.type;
		    auto old = oldModels.find(model.name);
		    if (old != oldModels.end()) {
			    if (old->second->storageEpoch == UINT32_MAX) {
				    return FreshResult::failure(FreshStatus::InternalError, "storage epoch overflow");
			    }
			    currentModel->storageEpoch = old->second->storageEpoch + 1;
			    currentModel->validator = old->second->validator;
		    } else {
			    currentModel->storageEpoch = 1;
		    }
		    return FreshResult::success();
	    },
	    [&](FreshModelType type, JsonDocument &&document) -> FreshResult {
		    if (!currentModel || currentModel->type != type) {
			    return FreshResult::failure(FreshStatus::InternalError, "durable restore visitor state mismatch");
		    }
		    if (currentModel->validator) {
			    FreshValidationResult validation = currentModel->validator(document);
			    if (!validation) {
				    return FreshResult::failure(FreshStatus::ValidationFailed, validation.message.c_str());
			    }
		    }
		    if (type == FreshModelType::Stream) {
			    currentModel->streamEntries.push_back(std::move(document));
		    } else {
			    const char *id = document["_id"] | "";
			    currentModel->docs[id] = std::move(document);
		    }
		    return FreshResult::success();
	    },
	    [&](const FreshBackupModelMetadata &model) -> FreshResult {
		    if (!currentModel || currentModel->name != model.name || currentModel->type != model.type) {
			    return FreshResult::failure(FreshStatus::InternalError, "durable restore model state mismatch");
		    }
		    importedModels[model.name] = currentModel;
		    currentModel.reset();
		    return FreshResult::success();
	    }
	);

	FreshBackupMetadata metadata;
	FreshResult parseResult = FreshReadBackupArchive(input, maxDocumentBytes, visitor, metadata);
	if (!parseResult) return parseResult;
	if (currentModel || importedModels.size() != metadata.modelCount) {
		return FreshResult::failure(FreshStatus::InternalError, "durable restore parser state mismatch");
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> finalModels;
	std::vector<std::shared_ptr<FreshModel::State>> retiredModels;
	size_t createdModels = 0;
	size_t replacedModels = 0;
	size_t removedModels = 0;
	if (options.mode == FreshRestoreMode::ReplaceSelected) {
		finalModels = oldModels;
		for (const auto &entry : importedModels) {
			auto old = oldModels.find(entry.first);
			if (old == oldModels.end()) createdModels++;
			else {
				replacedModels++;
				retiredModels.push_back(old->second);
			}
			finalModels[entry.first] = entry.second;
		}
	} else {
		finalModels = importedModels;
		for (const auto &entry : importedModels) {
			auto old = oldModels.find(entry.first);
			if (old == oldModels.end()) createdModels++;
			else {
				replacedModels++;
				retiredModels.push_back(old->second);
			}
		}
		for (const auto &entry : oldModels) {
			if (importedModels.find(entry.first) != importedModels.end()) continue;
			if (protectedNames.find(entry.first) != protectedNames.end()) {
				finalModels[entry.first] = entry.second;
			} else {
				removedModels++;
				retiredModels.push_back(entry.second);
			}
		}
	}

	const size_t affectedCount = createdModels + replacedModels + removedModels;
	if (affectedCount == 0) return FreshResult::success("restore completed with no changes");

	std::set<std::string> usedStorageIds;
	for (const auto &entry : oldModels) usedStorageIds.insert(entry.second->storageId);
	for (auto &entry : importedModels) {
		std::string storageId;
		do {
			storageId = FreshMakeId();
		} while (storageId.empty() || usedStorageIds.find(storageId) != usedStorageIds.end() ||
		         LittleFS.exists(modelPath(storageId).c_str()));
		entry.second->storageId = storageId;
		usedStorageIds.insert(storageId);
	}

	auto buildSnapshot = [&](const std::shared_ptr<FreshModel::State> &state,
	                         JsonDocument &snapshot,
	                         size_t &recordCount) -> FreshResult {
		recordCount = state->type == FreshModelType::Stream ? state->streamEntries.size() : state->docs.size();
		FreshResult result = FreshJsonSet(snapshot["version"], FreshSnapshotVersion, snapshot, "snapshot version");
		if (!result) return result;
		result = FreshJsonSet(snapshot["storageId"], state->storageId, snapshot, "snapshot storage id");
		if (!result) return result;
		result = FreshJsonSet(
		    snapshot["type"],
		    FreshModelTypeToString(state->type),
		    snapshot,
		    "snapshot type"
		);
		if (!result) return result;
		result = FreshJsonSet(snapshot["appliedThroughSequence"], 0ULL, snapshot, "snapshot sequence");
		if (!result) return result;
		result = FreshJsonSet(snapshot["recordCount"], recordCount, snapshot, "snapshot count");
		if (!result) return result;
		JsonArray records;
		const char *arrayName = state->type == FreshModelType::Stream ? "entries" : "docs";
		result = FreshJsonCreateArray(snapshot[arrayName], snapshot, records, "snapshot records");
		if (!result) return result;
		if (state->type == FreshModelType::Stream) {
			for (const JsonDocument &document : state->streamEntries) {
				result = FreshJsonAdd(records, document.as<JsonVariantConst>(), snapshot, "snapshot stream entry");
				if (!result) return result;
			}
		} else {
			for (const auto &document : state->docs) {
				result = FreshJsonAdd(records, document.second.as<JsonVariantConst>(), snapshot, "snapshot document");
				if (!result) return result;
			}
		}
		return FreshValidateRestoreSnapshot(snapshot, state->storageId, state->type, recordCount);
	};

	JsonDocument manifest(&FreshJsonAllocator());
	FreshResult jsonResult = FreshJsonSet(manifest["version"], FreshManifestVersion, manifest, "manifest version");
	if (!jsonResult) return jsonResult;
	jsonResult = FreshJsonSet(manifest["modelCount"], finalModels.size(), manifest, "manifest count");
	if (!jsonResult) return jsonResult;
	JsonArray manifestModels;
	jsonResult = FreshJsonCreateArray(manifest["models"], manifest, manifestModels, "manifest models");
	if (!jsonResult) return jsonResult;
	for (const auto &entry : finalModels) {
		JsonObject modelObject;
		jsonResult = FreshJsonAddObject(manifestModels, manifest, modelObject, "manifest model");
		if (!jsonResult) return jsonResult;
		jsonResult = FreshJsonSet(modelObject["name"], entry.second->name, manifest, "manifest model name");
		if (!jsonResult) return jsonResult;
		jsonResult = FreshJsonSet(modelObject["storageId"], entry.second->storageId, manifest, "manifest storage id");
		if (!jsonResult) return jsonResult;
		jsonResult = FreshJsonSet(
		    modelObject["type"],
		    FreshModelTypeToString(entry.second->type),
		    manifest,
		    "manifest model type"
		);
		if (!jsonResult) return jsonResult;
	}
	jsonResult = FreshValidateRestoreManifest(manifest);
	if (!jsonResult) return jsonResult;

	size_t requiredBytes = 0;
	const size_t manifestBytes = measureMsgPack(manifest);
	FreshResult manifestSize = checkPayloadSize(manifestBytes, FreshMaxPersistedPayloadBytes, "manifest");
	if (!manifestSize) return manifestSize;
	if (!FreshRestoreAddBytes(requiredBytes, FreshSlotHeaderSize) ||
	    !FreshRestoreAddBytes(requiredBytes, manifestBytes)) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "restore size calculation overflow");
	}
	for (const auto &entry : importedModels) {
		JsonDocument snapshot(&FreshJsonAllocator());
		size_t recordCount = 0;
		FreshResult snapshotResult = buildSnapshot(entry.second, snapshot, recordCount);
		if (!snapshotResult) return snapshotResult;
		const size_t snapshotBytes = measureMsgPack(snapshot);
		FreshResult snapshotSize = checkPayloadSize(snapshotBytes, maxSnapshotBytes, "snapshot");
		if (!snapshotSize) return snapshotSize;
		if (!FreshRestoreAddBytes(requiredBytes, FreshSlotHeaderSize) ||
		    !FreshRestoreAddBytes(requiredBytes, snapshotBytes)) {
			return FreshResult::failure(FreshStatus::SizeLimitExceeded, "restore size calculation overflow");
		}
	}
	FreshResult spaceResult = checkFreeSpace(requiredBytes);
	if (!spaceResult) return spaceResult;

	std::vector<std::string> stagedPaths;
	auto cleanupStaged = [&]() {
		for (const std::string &path : stagedPaths) FreshRemoveRestoreStorage(path);
	};
	for (const auto &entry : importedModels) {
		if (FreshRestoreShouldFail(FreshRestoreTestFailurePoint::BeforeSnapshotWrite)) {
			cleanupStaged();
			return FreshResult::failure(FreshStatus::FileSystemError, "injected failure before restore snapshot");
		}
		JsonDocument snapshot(&FreshJsonAllocator());
		size_t recordCount = 0;
		FreshResult snapshotResult = buildSnapshot(entry.second, snapshot, recordCount);
		if (!snapshotResult) {
			cleanupStaged();
			return snapshotResult;
		}
		const std::string path = modelPath(entry.second->storageId);
		stagedPaths.push_back(path);
		snapshotResult = FreshWriteRestoreSnapshot(
		    path,
		    entry.second->storageId,
		    entry.second->type,
		    recordCount,
		    snapshot,
		    maxSnapshotBytes
		);
		if (!snapshotResult) {
			cleanupStaged();
			return snapshotResult;
		}
		if (FreshRestoreShouldFail(FreshRestoreTestFailurePoint::AfterSnapshotWrite)) {
			cleanupStaged();
			return FreshResult::failure(FreshStatus::FileSystemError, "injected failure after restore snapshot");
		}
	}
	if (FreshRestoreShouldFail(FreshRestoreTestFailurePoint::BeforeManifestCommit)) {
		cleanupStaged();
		return FreshResult::failure(FreshStatus::FileSystemError, "injected failure before restore manifest");
	}

	auto capturedStateMatches = [&]() {
		if (_databaseRevision != capturedDatabaseRevision || _models.size() != oldModels.size()) return false;
		for (const auto &entry : oldModels) {
			auto current = _models.find(entry.first);
			auto revision = oldRevisions.find(entry.first);
			if (current == _models.end() || current->second != entry.second || revision == oldRevisions.end() ||
			    entry.second->revision != revision->second) {
				return false;
			}
		}
		return true;
	};

	std::vector<uint64_t> retiredRevisions;
	uint64_t nextDatabaseRevision = 0;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			cleanupStaged();
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized) {
			cleanupStaged();
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping || _lifecycle != Lifecycle::Running || !capturedStateMatches()) {
			cleanupStaged();
			return FreshResult::failure(FreshStatus::Busy, "database changed during durable restore");
		}
		FreshResult revisionResult = FreshNextRevision(
		    capturedDatabaseRevision,
		    nextDatabaseRevision,
		    "database revision"
		);
		if (!revisionResult) {
			cleanupStaged();
			return revisionResult;
		}
		if (_manifestEpoch == UINT32_MAX) {
			cleanupStaged();
			return FreshResult::failure(FreshStatus::InternalError, "manifest epoch overflow");
		}
		retiredRevisions.reserve(retiredModels.size());
		for (const auto &state : retiredModels) {
			uint64_t nextRevision = 0;
			FreshResult retiredRevision = FreshNextRevision(state->revision, nextRevision, "model revision");
			if (!retiredRevision) {
				cleanupStaged();
				return retiredRevision;
			}
			retiredRevisions.push_back(nextRevision);
		}

		FreshRestoreManifestCommitState commitState = FreshRestoreManifestCommitState::NotCommitted;
		FreshResult manifestResult = FreshCommitRestoreManifest(_rootPath, manifest, commitState);
		if (!manifestResult) {
			if (commitState == FreshRestoreManifestCommitState::NotCommitted) cleanupStaged();
			return manifestResult;
		}
		if (FreshRestoreShouldFail(FreshRestoreTestFailurePoint::AfterManifestCommit)) {
			return FreshResult::failure(FreshStatus::FileSystemError, "injected reset after restore manifest commit");
		}

		for (size_t i = 0; i < retiredModels.size(); ++i) {
			auto &state = retiredModels[i];
			state->revision = retiredRevisions[i];
			state->dropped = true;
			state->dirty = false;
			state->snapshotRequired = false;
			state->pending.clear();
		}
		for (auto &entry : importedModels) {
			auto &state = entry.second;
			state->revision = 1;
			state->dropped = false;
			state->degraded = false;
			state->dirty = false;
			state->snapshotRequired = false;
			state->recordsSinceSnapshot = 0;
			state->bytesSinceSnapshot = 0;
			state->checkpointSequence = 0;
			state->lastSequence = 0;
			state->pending.clear();
		}
		_models.swap(finalModels);
		_manifestDirty = false;
		_manifestEpoch++;
		_databaseRevision = nextDatabaseRevision;
	}

	bool cleanupDeferred = FreshRestoreShouldFail(FreshRestoreTestFailurePoint::BeforeCleanup);
	if (!cleanupDeferred) {
		for (const auto &state : retiredModels) {
			if (!state->storageId.empty() && !FreshRemoveRestoreStorage(modelPath(state->storageId))) {
				cleanupDeferred = true;
			}
		}
	}
	return FreshResult::success(
	    cleanupDeferred ? "backup restored; obsolete storage cleanup deferred" : "backup restored durably",
	    affectedCount
	);
}

FreshResult Fresh::restoreBackup(
    const uint8_t *data,
    size_t length,
    const FreshRestoreOptions &options
) {
	if (data == nullptr || length == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "backup buffer is required");
	}
	fresh_backup_v2::MemoryStream input(data, length);
	return restoreBackup(input, options);
}
