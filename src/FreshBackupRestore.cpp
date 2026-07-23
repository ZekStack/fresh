#include "Fresh.h"
#include "FreshRestoreTesting.h"
#include "internal/FreshBackupArchive.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <LittleFS.h>

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

std::string FreshRestoreSlotPath(const std::string &modelPath, char slot) {
	std::string name = FreshSnapshotFile;
	name += ".";
	name.push_back(slot);
	name += ".msgpack";
	return FreshJoinPath(modelPath, name);
}

bool FreshRemoveRestoreStorage(const std::string &modelPath) {
	LittleFS.remove(FreshJoinPath(modelPath, FreshJournalFile).c_str());
	LittleFS.remove(FreshRestoreSlotPath(modelPath, 'a').c_str());
	LittleFS.remove(FreshRestoreSlotPath(modelPath, 'b').c_str());
	return !LittleFS.exists(modelPath.c_str()) || LittleFS.rmdir(modelPath.c_str());
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
	const size_t actualCount = payload[arrayName].as<JsonArrayConst>().size();
	if (declaredCount > SIZE_MAX || static_cast<size_t>(declaredCount) != actualCount ||
	    actualCount != expectedRecordCount) {
		return FreshResult::failure(FreshStatus::CorruptData, "restore snapshot record count mismatch");
	}
	return FreshResult::success("restore snapshot valid");
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

	const size_t payloadBytes = measureMsgPack(payload);
	if (payloadBytes == 0 || payloadBytes > maxSnapshotBytes ||
	    payloadBytes > FreshMaxPersistedPayloadBytes || payloadBytes > UINT32_MAX) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "restore snapshot size limit exceeded");
	}

	FreshBuffer encoded;
	if (!encoded.allocate(payloadBytes, FreshAllocationCategory::DurableSlotPayload)) {
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate restore snapshot payload");
	}
	if (serializeMsgPack(payload, encoded.data(), encoded.size()) != encoded.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to serialize restore snapshot");
	}

	if (LittleFS.exists(modelPath.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "restore storage path already exists");
	}
	if (!LittleFS.mkdir(modelPath.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create restore storage directory");
	}

	const std::string slotPath = FreshRestoreSlotPath(modelPath, 'b');
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

	File input = LittleFS.open(slotPath.c_str(), "r");
	if (!input) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to reopen restore snapshot");
	}
	uint32_t magic = 0;
	uint16_t version = 0;
	uint64_t generation = 0;
	uint32_t storedSize = 0;
	uint32_t storedChecksum = 0;
	const bool headerOk = FreshReadU32(input, magic) && FreshReadU16(input, version) &&
	                      FreshReadU64(input, generation) && FreshReadU32(input, storedSize) &&
	                      FreshReadU32(input, storedChecksum);
	if (!headerOk || magic != FreshSlotMagic || version != FreshSlotVersion || generation != 1 ||
	    storedSize != encoded.size() || input.available() != static_cast<int>(storedSize)) {
		input.close();
		return FreshResult::failure(FreshStatus::CorruptData, "invalid restore snapshot slot");
	}

	FreshBuffer verifiedBytes;
	if (!verifiedBytes.allocate(storedSize, FreshAllocationCategory::DurableSlotPayload)) {
		input.close();
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate restore verification payload");
	}
	const size_t read = input.read(verifiedBytes.data(), verifiedBytes.size());
	const bool trailing = input.available() != 0;
	input.close();
	if (read != verifiedBytes.size() || trailing) {
		return FreshResult::failure(FreshStatus::CorruptData, "truncated restore snapshot slot");
	}
	if (FreshChecksum(verifiedBytes.data(), verifiedBytes.size()) != storedChecksum) {
		return FreshResult::failure(FreshStatus::CorruptData, "restore snapshot checksum mismatch");
	}

	JsonDocument verified(&FreshJsonAllocator());
	DeserializationError decode = deserializeMsgPack(
	    verified,
	    verifiedBytes.data(),
	    verifiedBytes.size()
	);
	if (decode || verified.overflowed()) {
		return FreshResult::failure(
		    decode == DeserializationError::NoMemory || verified.overflowed()
		        ? FreshStatus::OutOfMemory
		        : FreshStatus::CorruptData,
		    "failed to decode restore snapshot"
		);
	}
	return FreshValidateRestoreSnapshot(verified, storageId, type, expectedRecordCount);
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

	// The current registry must be fully durable before it can contribute
	// preserved models to the transactional manifest.
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
		jsonResult = FreshJsonSet(
		    modelObject["storageId"],
		    entry.second->storageId,
		    manifest,
		    "manifest storage id"
		);
		if (!jsonResult) return jsonResult;
		jsonResult = FreshJsonSet(
		    modelObject["type"],
		    FreshModelTypeToString(entry.second->type),
		    manifest,
		    "manifest model type"
		);
		if (!jsonResult) return jsonResult;
	}
	jsonResult = FreshValidateJsonDocument(manifest, "restore manifest");
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

		FreshResult manifestResult = writeManifest(manifest);
		if (!manifestResult) {
			cleanupStaged();
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
