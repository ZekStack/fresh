#include "Fresh.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <cstring>
#include <limits>
#include <set>
#include <utility>

class FreshBackupPrint : public Print {
  public:
	explicit FreshBackupPrint(Fresh &db) : _db(db) {
	}

	size_t write(uint8_t byte) override {
		return _db.backupWriteByte(byte) ? 1 : 0;
	}

	size_t write(const uint8_t *buffer, size_t size) override {
		size_t written = 0;
		for (size_t i = 0; i < size; ++i) {
			if (write(buffer[i]) != 1) break;
			written++;
		}
		return written;
	}

  private:
	Fresh &_db;
};

bool Fresh::backupWriteByte(uint8_t byte) {
	while (true) {
		bool wrote = false;
		bool shouldEmitProgress = false;
		FreshBackupInfo info;
		{
			FreshLock lock(_backup->mutex);
			if (_backup->cancelled || _backup->buffer.empty()) return false;
			if (_backup->used < _backup->buffer.size()) {
				_backup->buffer[_backup->tail] = byte;
				_backup->tail = (_backup->tail + 1) % _backup->buffer.size();
				_backup->used++;
				_backup->progress++;
				shouldEmitProgress =
				    _backup->total > 0 && (_backup->progress == _backup->total ||
				                          _backup->progress - _backup->lastProgressEvent >= 512);
				if (shouldEmitProgress) {
					_backup->lastProgressEvent = _backup->progress;
					info.progress = _backup->progress;
					info.total = _backup->total;
					info.size = _backup->progress;
				}
				wrote = true;
			}
		}
		if (shouldEmitProgress) callBackupProgress(info);
		if (wrote) return true;
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

void Fresh::runBackupIfRequested() {
	{
		FreshLock lock(_backup->mutex);
		if (!_backup->requested || _backup->running) return;
		_backup->requested = false;
		_backup->running = true;
		_backup->done = false;
		_backup->cancelled = false;
		_backup->head = 0;
		_backup->tail = 0;
		_backup->used = 0;
		_backup->progress = 0;
		_backup->total = 0;
		_backup->lastProgressEvent = 0;
		_backup->state = FreshBackupState::Running;
		_backup->result = FreshResult::success("backup running");
	}

	FreshBackupInfo startInfo;
	startInfo.estimatedSize = estimateBackupSize();
	callBackupStart(startInfo);
	emitEvent({.type = FreshEventType::BackupStarted, .result = FreshResult::success("backup started")});

	FreshResult constructionResult = FreshResult::success();
	JsonDocument archive(&FreshJsonAllocator());
	const uint64_t generatedAt = now();
	{
		FreshLock lock(*_mutex);
		if (!lock || !_initialized || _stopping || _lifecycle != Lifecycle::Running) {
			constructionResult = FreshResult::failure(FreshStatus::Busy, "database is stopping");
		} else {
			size_t modelCount = 0;
			for (const auto &entry : _models) {
				if (!entry.second->dropped) modelCount++;
			}
			constructionResult = FreshJsonSet(
			    archive["version"],
			    FreshBackupVersion,
			    archive,
			    "backup version"
			);
			if (constructionResult) {
				constructionResult = FreshJsonSet(
				    archive["generatedAt"],
				    generatedAt,
				    archive,
				    "backup generation time"
				);
			}
			if (constructionResult) {
				constructionResult = FreshJsonSet(
				    archive["modelCount"],
				    modelCount,
				    archive,
				    "backup model count"
				);
			}
			JsonArray modelsArray;
			if (constructionResult) {
				constructionResult = FreshJsonCreateArray(
				    archive["models"],
				    archive,
				    modelsArray,
				    "backup models"
				);
			}
			for (const auto &entry : _models) {
				if (!constructionResult) break;
				const auto &state = entry.second;
				if (state->dropped) continue;
				const size_t recordCount = state->type == FreshModelType::Stream
				                               ? state->streamEntries.size()
				                               : state->docs.size();
				JsonObject modelObject;
				constructionResult = FreshJsonAddObject(
				    modelsArray,
				    archive,
				    modelObject,
				    "backup model"
				);
				if (!constructionResult) break;
				constructionResult = FreshJsonSet(
				    modelObject["name"],
				    state->name,
				    archive,
				    "backup model name"
				);
				if (!constructionResult) break;
				constructionResult = FreshJsonSet(
				    modelObject["type"],
				    FreshModelTypeToString(state->type),
				    archive,
				    "backup model type"
				);
				if (!constructionResult) break;
				constructionResult = FreshJsonSet(
				    modelObject["recordCount"],
				    recordCount,
				    archive,
				    "backup record count"
				);
				if (!constructionResult) break;

				const char *arrayName = state->type == FreshModelType::Stream ? "entries" : "docs";
				JsonArray records;
				constructionResult = FreshJsonCreateArray(
				    modelObject[arrayName],
				    archive,
				    records,
				    "backup records"
				);
				if (!constructionResult) break;
				if (state->type == FreshModelType::Stream) {
					for (const JsonDocument &doc : state->streamEntries) {
						constructionResult = FreshJsonAdd(
						    records,
						    doc.as<JsonVariantConst>(),
						    archive,
						    "backup stream entry"
						);
						if (!constructionResult) break;
					}
				} else {
					for (const auto &docEntry : state->docs) {
						constructionResult = FreshJsonAdd(
						    records,
						    docEntry.second.as<JsonVariantConst>(),
						    archive,
						    "backup document"
						);
						if (!constructionResult) break;
					}
				}
			}
		}
	}
	if (constructionResult) {
		constructionResult = FreshValidateJsonDocument(archive, "backup archive");
	}

	size_t total = 0;
	size_t written = 0;
	FreshResult result;
	if (!constructionResult) {
		result = constructionResult;
	} else {
		total = measureMsgPack(archive);
		if (total == 0 || total > FreshMaxPersistedPayloadBytes) {
			result = FreshResult::failure(
			    total == 0 ? FreshStatus::InternalError : FreshStatus::SizeLimitExceeded,
			    total == 0 ? "backup archive is empty" : "backup archive is too large"
			);
		} else {
			{
				FreshLock lock(_backup->mutex);
				_backup->total = total;
			}
			FreshBackupPrint print(*this);
			written = serializeMsgPack(archive, print);
			const bool cancelled = isBackupCancelled();
			result = written == total && !cancelled
			             ? FreshResult::success("backup finished", written)
			             : FreshResult::failure(
			                   cancelled ? FreshStatus::Cancelled : FreshStatus::InternalError,
			                   cancelled ? "backup cancelled" : "backup serialization failed",
			                   written
			               );
		}
	}

	{
		FreshLock lock(_backup->mutex);
		_backup->running = false;
		_backup->done = true;
		_backup->state = result ? FreshBackupState::Finished
		                        : (result.status == FreshStatus::Cancelled ? FreshBackupState::Cancelled
		                                                                  : FreshBackupState::Error);
		_backup->result = result;
	}

	FreshBackupInfo endInfo;
	endInfo.progress = written;
	endInfo.total = total;
	endInfo.size = written;
	endInfo.result = result;
	if (result) {
		callBackupEnd(endInfo);
		emitEvent({.type = FreshEventType::BackupFinished, .result = result});
	} else if (result.status == FreshStatus::Cancelled) {
		callBackupError({.error = FreshBackupError::Cancelled, .result = result});
		emitEvent({.type = FreshEventType::BackupCancelled, .result = result});
	} else {
		const FreshBackupError error = result.status == FreshStatus::OutOfMemory
		                                   ? FreshBackupError::OutOfMemory
		                                   : FreshBackupError::SerializationFailed;
		callBackupError({.error = error, .result = result});
		emitEvent({.type = FreshEventType::BackupError, .result = result});
	}
}

bool Fresh::isBackupCancelled() {
	FreshLock lock(_backup->mutex);
	return _backup->cancelled;
}

size_t Fresh::estimateBackupSize() {
	FreshLock lock(*_mutex);
	size_t total = 96;
	for (const auto &entry : _models) {
		const auto &state = entry.second;
		if (state->dropped) continue;
		total += state->name.size() + 48;
		if (state->type == FreshModelType::Stream) {
			for (const JsonDocument &doc : state->streamEntries) total += measureMsgPack(doc);
		} else {
			for (const auto &docEntry : state->docs) total += measureMsgPack(docEntry.second);
		}
	}
	return total;
}

void Fresh::callBackupStart(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupStart;
	}
	if (callback) callback(info);
}

void Fresh::callBackupProgress(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupProgress;
	}
	if (callback) callback(info);
}

void Fresh::callBackupEnd(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupEnd;
	}
	if (callback) callback(info);
}

void Fresh::callBackupError(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupError;
	}
	if (callback) callback(info);
}

FreshResult Fresh::startBackup() {
	FreshLock dbLock(*_mutex);
	if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	if (_stopping || _lifecycle != Lifecycle::Running) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	{
		FreshLock backupLock(_backup->mutex);
		if (_backup->running || _backup->requested) {
			return FreshResult::failure(FreshStatus::Busy, "backup already running");
		}
		_backup->requested = true;
		_backup->done = false;
		_backup->cancelled = false;
		_backup->state = FreshBackupState::Queued;
		_backup->result = FreshResult::success("backup queued");
	}
	if (_syncTaskHandle != nullptr) xTaskNotifyGive(_syncTaskHandle);
	return FreshResult::success("backup queued");
}

size_t Fresh::readBackup(uint8_t *buffer, size_t length, uint32_t timeoutMS) {
	if (buffer == nullptr || length == 0) return 0;

	const uint32_t start = millis();
	size_t read = 0;
	while (read < length) {
		{
			FreshLock lock(_backup->mutex);
			while (_backup->used > 0 && read < length) {
				buffer[read++] = _backup->buffer[_backup->head];
				_backup->head = (_backup->head + 1) % _backup->buffer.size();
				_backup->used--;
			}
			if (read > 0 || _backup->done || (!_backup->running && !_backup->requested)) return read;
		}
		if (timeoutMS == 0 || millis() - start >= timeoutMS) return read;
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	return read;
}

FreshBackupStatus Fresh::backupStatus() const {
	FreshLock lock(_backup->mutex);
	return FreshBackupStatus{.state = _backup->state, .result = _backup->result};
}

FreshResult Fresh::cancelBackup() {
	FreshLock lock(_backup->mutex);
	if (!_backup->running && !_backup->requested) {
		return FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");
	}
	_backup->cancelled = true;
	_backup->requested = false;
	_backup->state = FreshBackupState::Cancelled;
	_backup->result = FreshResult::failure(FreshStatus::Cancelled, "backup cancelled");
	return _backup->result;
}

FreshResult Fresh::backupImport(Stream &input) {
	{
		FreshLock backupLock(_backup->mutex);
		if (_backup->running || _backup->requested) {
			return FreshResult::failure(FreshStatus::Busy, "backup already running");
		}
	}
	JsonDocument archive(&FreshJsonAllocator());
	DeserializationError error = deserializeMsgPack(archive, input);
	if (error || archive.overflowed()) {
		return FreshResult::failure(
		    error == DeserializationError::NoMemory || archive.overflowed()
		        ? FreshStatus::OutOfMemory
		        : FreshStatus::CorruptData,
		    "failed to read backup"
		);
	}
	return importBackupArchive(archive);
}

FreshResult Fresh::backupImport(const uint8_t *data, size_t length) {
	if (data == nullptr || length == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "backup buffer is required");
	}
	{
		FreshLock backupLock(_backup->mutex);
		if (_backup->running || _backup->requested) {
			return FreshResult::failure(FreshStatus::Busy, "backup already running");
		}
	}
	JsonDocument archive(&FreshJsonAllocator());
	DeserializationError error = deserializeMsgPack(archive, data, length);
	if (error || archive.overflowed()) {
		return FreshResult::failure(
		    error == DeserializationError::NoMemory || archive.overflowed()
		        ? FreshStatus::OutOfMemory
		        : FreshStatus::CorruptData,
		    "failed to read backup"
		);
	}
	return importBackupArchive(archive);
}

FreshResult Fresh::importBackupArchive(const JsonDocument &archive) {
	if ((archive["version"] | 0U) != FreshBackupVersion ||
	    !archive["modelCount"].is<uint64_t>() ||
	    !archive["models"].is<JsonArrayConst>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "unsupported or corrupt backup");
	}
	const uint64_t declaredModelCount = archive["modelCount"].as<uint64_t>();
	const JsonArrayConst modelArray = archive["models"].as<JsonArrayConst>();
	if (declaredModelCount > SIZE_MAX ||
	    static_cast<size_t>(declaredModelCount) != modelArray.size()) {
		return FreshResult::failure(FreshStatus::CorruptData, "backup model count mismatch");
	}

	uint64_t capturedDatabaseRevision = 0;
	std::map<std::string, std::shared_ptr<FreshModel::State>> oldModels;
	{
		FreshLock lock(*_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		capturedDatabaseRevision = _databaseRevision;
		oldModels = _models;
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> importedModels;
	for (JsonObjectConst modelObject : modelArray) {
		const char *name = modelObject["name"] | "";
		const char *typeName = modelObject["type"] | "";
		if (!FreshIsValidName(name)) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup contains invalid model name");
		}
		if (strcmp(typeName, "general") != 0 && strcmp(typeName, "stream") != 0) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup contains invalid model type");
		}
		if (!modelObject["recordCount"].is<uint64_t>()) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup record count is missing");
		}

		const std::string modelName = name;
		if (importedModels.find(modelName) != importedModels.end()) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup contains duplicate model");
		}
		const FreshModelType type = FreshModelTypeFromString(typeName);
		const char *arrayName = type == FreshModelType::Stream ? "entries" : "docs";
		if (!modelObject[arrayName].is<JsonArrayConst>()) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup records are missing");
		}
		const uint64_t declaredRecordCount = modelObject["recordCount"].as<uint64_t>();
		const JsonArrayConst records = modelObject[arrayName].as<JsonArrayConst>();
		if (declaredRecordCount > SIZE_MAX ||
		    static_cast<size_t>(declaredRecordCount) != records.size()) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup record count mismatch");
		}

		auto state = std::make_shared<FreshModel::State>();
		if (!state) {
			return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate imported model");
		}
		state->name = modelName;
		state->type = type;
		state->dirty = true;
		state->snapshotRequired = true;
		auto old = oldModels.find(modelName);
		if (old != oldModels.end()) {
			state->storageId = old->second->storageId;
			if (old->second->storageEpoch == UINT32_MAX) {
				return FreshResult::failure(FreshStatus::InternalError, "storage epoch overflow");
			}
			state->storageEpoch = old->second->storageEpoch + 1;
			state->validator = old->second->validator;
		}

		if (type == FreshModelType::Stream) {
			for (JsonVariantConst entry : records) {
				JsonDocument copy;
				FreshResult cloneResult = FreshCloneJson(copy, entry, "backup stream entry");
				if (!cloneResult) return cloneResult;
				FreshResult sizeResult = checkPayloadSize(
				    measureMsgPack(copy),
				    _config.maxDocumentBytes,
				    "stream entry"
				);
				if (!sizeResult) return sizeResult;
				state->streamEntries.push_back(std::move(copy));
			}
		} else {
			for (JsonVariantConst entry : records) {
				JsonDocument copy;
				FreshResult cloneResult = FreshCloneJson(copy, entry, "backup document");
				if (!cloneResult) return cloneResult;
				FreshResult sizeResult = checkPayloadSize(
				    measureMsgPack(copy),
				    _config.maxDocumentBytes,
				    "document"
				);
				if (!sizeResult) return sizeResult;
				const char *id = copy["_id"] | "";
				if (*id == '\0' || state->docs.find(id) != state->docs.end()) {
					return FreshResult::failure(FreshStatus::CorruptData, "backup contains invalid document id");
				}
				state->docs[id] = std::move(copy);
			}
		}
		importedModels[modelName] = state;
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> finalModels = importedModels;
	for (const auto &entry : oldModels) {
		if (importedModels.find(entry.first) == importedModels.end()) {
			finalModels[entry.first] = entry.second;
		}
	}

	uint64_t nextDatabaseRevision = 0;
	FreshResult revisionResult = FreshNextRevision(
	    capturedDatabaseRevision,
	    nextDatabaseRevision,
	    "database revision"
	);
	if (!revisionResult) return revisionResult;

	TaskHandle_t syncTaskHandle = nullptr;
	size_t affectedCount = 0;
	{
		FreshLock syncLock(*_syncMutex);
		if (!syncLock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock sync");
		FreshLock lock(*_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		if (_databaseRevision != capturedDatabaseRevision) {
			return FreshResult::failure(FreshStatus::Busy, "database changed while backup import was prepared");
		}

		for (auto &entry : oldModels) {
			auto &state = entry.second;
			uint64_t nextRevision = 0;
			FreshResult stateRevision = FreshNextRevision(state->revision, nextRevision, "model revision");
			if (!stateRevision) return stateRevision;
			state->revision = nextRevision;
			state->dropped = true;
			state->dirty = true;
			state->snapshotRequired = false;
			state->pending.clear();
			if (importedModels.find(entry.first) == importedModels.end()) {
				state->storageEpoch++;
			}
		}
		for (auto &entry : importedModels) {
			entry.second->revision = 1;
			entry.second->dropped = false;
			entry.second->dirty = true;
			entry.second->snapshotRequired = true;
		}

		affectedCount = finalModels.size();
		_models.swap(finalModels);
		_manifestDirty = true;
		_manifestEpoch++;
		_databaseRevision = nextDatabaseRevision;
		syncTaskHandle = _syncTaskHandle;
	}

	if (syncTaskHandle != nullptr) xTaskNotifyGive(syncTaskHandle);
	return FreshResult::success("backup imported", affectedCount);
}
