#include "Fresh.h"
#include "internal/FreshBackupArchive.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshInternal.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace {

class CallbackBackupVisitor : public FreshBackupArchiveVisitor {
  public:
	using ModelCallback = std::function<FreshResult(const FreshBackupModelMetadata &)>;
	using RecordCallback = std::function<FreshResult(FreshModelType, JsonDocument &&)>;

	CallbackBackupVisitor(
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

} // namespace

FreshResult Fresh::backupImport(Stream &input) {
	{
		FreshLock backupLock(_backup->mutex);
		if (_backup->running || _backup->requested) {
			return FreshResult::failure(FreshStatus::Busy, "backup already running");
		}
	}

	uint64_t capturedDatabaseRevision = 0;
	size_t maxDocumentBytes = 0;
	std::map<std::string, std::shared_ptr<FreshModel::State>> oldModels;
	std::map<std::string, uint64_t> oldRevisions;
	{
		FreshLock lock(*_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		capturedDatabaseRevision = _databaseRevision;
		maxDocumentBytes = _config.maxDocumentBytes;
		oldModels = _models;
		for (const auto &entry : oldModels) oldRevisions[entry.first] = entry.second->revision;
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> importedModels;
	std::shared_ptr<FreshModel::State> currentModel;

	CallbackBackupVisitor visitor(
	    [&](const FreshBackupModelMetadata &model) -> FreshResult {
		    currentModel = std::make_shared<FreshModel::State>();
		    if (!currentModel) {
			    return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate imported model");
		    }
		    currentModel->name = model.name;
		    currentModel->type = model.type;
		    currentModel->dirty = true;
		    currentModel->snapshotRequired = true;
		    auto old = oldModels.find(model.name);
		    if (old != oldModels.end()) {
			    if (old->second->storageEpoch == UINT32_MAX) {
				    return FreshResult::failure(FreshStatus::InternalError, "storage epoch overflow");
			    }
			    currentModel->storageId = old->second->storageId;
			    currentModel->storageEpoch = old->second->storageEpoch + 1;
			    currentModel->validator = old->second->validator;
		    }
		    return FreshResult::success();
	    },
	    [&](FreshModelType type, JsonDocument &&document) -> FreshResult {
		    if (!currentModel || currentModel->type != type) {
			    return FreshResult::failure(FreshStatus::InternalError, "backup import visitor state mismatch");
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
			    return FreshResult::failure(FreshStatus::InternalError, "backup import model state mismatch");
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
		return FreshResult::failure(FreshStatus::InternalError, "backup import parser state mismatch");
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> finalModels = importedModels;
	for (const auto &entry : oldModels) {
		if (importedModels.find(entry.first) == importedModels.end()) finalModels[entry.first] = entry.second;
	}

	FreshLock importSyncLock(*_syncMutex);
	if (!importSyncLock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock sync");
	FreshLock importDbLock(*_mutex);
	if (!importDbLock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	if (_stopping || _lifecycle != Lifecycle::Running) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	if (_databaseRevision != capturedDatabaseRevision || _models.size() != oldModels.size()) {
		return FreshResult::failure(FreshStatus::Busy, "database changed during backup import");
	}
	for (const auto &entry : oldModels) {
		auto current = _models.find(entry.first);
		auto revision = oldRevisions.find(entry.first);
		if (current == _models.end() || current->second != entry.second || revision == oldRevisions.end() ||
		    entry.second->revision != revision->second) {
			return FreshResult::failure(FreshStatus::Busy, "database changed during backup import");
		}
	}

	uint64_t nextDatabaseRevision = 0;
	FreshResult revisionResult = FreshNextRevision(
	    capturedDatabaseRevision,
	    nextDatabaseRevision,
	    "database revision"
	);
	if (!revisionResult) return revisionResult;
	if (_manifestEpoch == UINT32_MAX) {
		return FreshResult::failure(FreshStatus::InternalError, "manifest epoch overflow");
	}
	for (auto &entry : oldModels) {
		if (importedModels.find(entry.first) == importedModels.end()) continue;
		uint64_t nextRevision = 0;
		FreshResult oldRevision = FreshNextRevision(entry.second->revision, nextRevision, "model revision");
		if (!oldRevision) return oldRevision;
		entry.second->revision = nextRevision;
		entry.second->dropped = true;
		entry.second->dirty = true;
		entry.second->snapshotRequired = false;
		entry.second->pending.clear();
	}
	for (auto &entry : importedModels) {
		entry.second->revision = 1;
		entry.second->dropped = false;
		entry.second->dirty = true;
		entry.second->snapshotRequired = true;
	}

	const size_t affectedCount = importedModels.size();
	_models.swap(finalModels);
	_manifestDirty = true;
	_manifestEpoch++;
	_databaseRevision = nextDatabaseRevision;
	TaskHandle_t syncTaskHandle = _syncTaskHandle;
	if (syncTaskHandle != nullptr) xTaskNotifyGive(syncTaskHandle);
	return FreshResult::success("backup imported", affectedCount);
}

FreshResult Fresh::backupImport(const uint8_t *data, size_t length) {
	if (data == nullptr || length == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "backup buffer is required");
	}
	fresh_backup_v2::MemoryStream input(data, length);
	return backupImport(input);
}
