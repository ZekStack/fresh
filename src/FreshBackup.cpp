#include "Fresh.h"
#include "internal/FreshInternal.h"

#include <cstring>
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
			if (write(buffer[i]) != 1) {
				break;
			}
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
			if (_backup->cancelled) {
				return false;
			}
			if (_backup->buffer.empty()) {
				return false;
			}
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
		if (shouldEmitProgress) {
			callBackupProgress(info);
		}
		if (wrote) {
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

void Fresh::runBackupIfRequested() {
	{
		FreshLock lock(_backup->mutex);
		if (!_backup->requested || _backup->running) {
			return;
		}
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
	emitEvent(
	    {.type = FreshEventType::BackupStarted, .result = FreshResult::success("backup started")}
	);

	const uint64_t generatedAt = now();
	JsonDocument archive;
	{
		FreshLock lock(*_mutex);
		archive["version"] = 1;
		archive["generatedAt"] = generatedAt;
		JsonArray modelsArray = archive["models"].to<JsonArray>();
		for (const auto &entry : _models) {
			const auto &state = entry.second;
			if (state->dropped) {
				continue;
			}
			JsonObject modelObject = modelsArray.add<JsonObject>();
			modelObject["name"] = state->name;
			modelObject["type"] = FreshModelTypeToString(state->type);
			if (state->type == FreshModelType::Stream) {
				JsonArray entries = modelObject["entries"].to<JsonArray>();
				for (const JsonDocument &doc : state->streamEntries) {
					entries.add(doc.as<JsonVariantConst>());
				}
			} else {
				JsonArray docs = modelObject["docs"].to<JsonArray>();
				for (const auto &docEntry : state->docs) {
					docs.add(docEntry.second.as<JsonVariantConst>());
				}
			}
		}
	}

	FreshBackupPrint print(*this);
	size_t total = measureMsgPack(archive);
	{
		FreshLock lock(_backup->mutex);
		_backup->total = total;
	}
	size_t written = serializeMsgPack(archive, print);
	FreshResult result =
	    written == total && !isBackupCancelled()
	        ? FreshResult::success("backup finished", written)
	        : FreshResult::failure(
	              isBackupCancelled() ? FreshStatus::Cancelled : FreshStatus::InternalError,
	              isBackupCancelled() ? "backup cancelled" : "backup serialization failed",
	              written
	          );

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
		callBackupError({.error = FreshBackupError::SerializationFailed, .result = result});
		emitEvent({.type = FreshEventType::BackupError, .result = result});
	}
}

bool Fresh::isBackupCancelled() {
	FreshLock lock(_backup->mutex);
	return _backup->cancelled;
}

size_t Fresh::estimateBackupSize() {
	FreshLock lock(*_mutex);
	size_t total = 64;
	for (const auto &entry : _models) {
		const auto &state = entry.second;
		total += state->name.size() + 32;
		if (state->type == FreshModelType::Stream) {
			for (const JsonDocument &doc : state->streamEntries) {
				total += measureMsgPack(doc);
			}
		} else {
			for (const auto &docEntry : state->docs) {
				total += measureMsgPack(docEntry.second);
			}
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
	if (callback) {
		callback(info);
	}
}

void Fresh::callBackupProgress(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupProgress;
	}
	if (callback) {
		callback(info);
	}
}

void Fresh::callBackupEnd(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupEnd;
	}
	if (callback) {
		callback(info);
	}
}

void Fresh::callBackupError(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupError;
	}
	if (callback) {
		callback(info);
	}
}

FreshResult Fresh::startBackup() {
	FreshLock dbLock(*_mutex);
	if (!_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_stopping) {
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
	if (_syncTaskHandle != nullptr) {
		xTaskNotifyGive(_syncTaskHandle);
	}
	return FreshResult::success("backup queued");
}

size_t Fresh::readBackup(uint8_t *buffer, size_t length, uint32_t timeoutMS) {
	if (buffer == nullptr || length == 0) {
		return 0;
	}

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
			if (read > 0 || _backup->done || (!_backup->running && !_backup->requested)) {
				return read;
			}
		}
		if (timeoutMS == 0 || millis() - start >= timeoutMS) {
			return read;
		}
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
	bool backupActive = false;
	{
		FreshLock backupLock(_backup->mutex);
		backupActive = _backup->running || _backup->requested;
	}
	if (backupActive) {
		return FreshResult::failure(FreshStatus::Busy, "backup already running");
	}
	{
		FreshLock dbLock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}

	JsonDocument archive;
	DeserializationError error = deserializeMsgPack(archive, input);
	if (error) {
		return FreshResult::failure(FreshStatus::CorruptData, "failed to read backup");
	}
	return importBackupArchive(archive);
}

FreshResult Fresh::backupImport(const uint8_t *data, size_t length) {
	if (data == nullptr || length == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "backup buffer is required");
	}

	bool backupActive = false;
	{
		FreshLock backupLock(_backup->mutex);
		backupActive = _backup->running || _backup->requested;
	}
	if (backupActive) {
		return FreshResult::failure(FreshStatus::Busy, "backup already running");
	}
	{
		FreshLock dbLock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}

	JsonDocument archive;
	DeserializationError error = deserializeMsgPack(archive, data, length);
	if (error) {
		return FreshResult::failure(FreshStatus::CorruptData, "failed to read backup");
	}
	return importBackupArchive(archive);
}

FreshResult Fresh::importBackupArchive(const JsonDocument &archive) {
	{
		FreshLock dbLock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}
	if ((archive["version"] | 0) != 1) {
		return FreshResult::failure(FreshStatus::CorruptData, "unsupported backup version");
	}
	if (!archive["models"].is<JsonArrayConst>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "backup models are missing");
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> importedModels;
	for (JsonObjectConst modelObject : archive["models"].as<JsonArrayConst>()) {
		const char *name = modelObject["name"] | "";
		const char *typeName = modelObject["type"] | "";
		if (!FreshIsValidName(name)) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup contains invalid model name");
		}
		if (strcmp(typeName, "general") != 0 && strcmp(typeName, "stream") != 0) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup contains invalid model type");
		}

		const std::string modelName = name;
		if (importedModels.find(modelName) != importedModels.end()) {
			return FreshResult::failure(FreshStatus::CorruptData, "backup contains duplicate model");
		}

		auto state = std::make_shared<FreshModel::State>();
		state->name = modelName;
		state->type = FreshModelTypeFromString(typeName);
		state->dirty = true;
		state->snapshotRequired = true;

		if (state->type == FreshModelType::Stream) {
			if (!modelObject["entries"].is<JsonArrayConst>()) {
				return FreshResult::failure(FreshStatus::CorruptData, "stream backup entries are missing");
			}
			for (JsonVariantConst entry : modelObject["entries"].as<JsonArrayConst>()) {
				JsonDocument copy;
				FreshResult cloneResult = FreshCloneJson(copy, entry, "backup stream entry");
				if (!cloneResult) {
					return cloneResult;
				}
				FreshResult sizeResult =
				    checkPayloadSize(measureMsgPack(copy), _config.maxDocumentBytes, "stream entry");
				if (!sizeResult) {
					return sizeResult;
				}
				state->streamEntries.push_back(std::move(copy));
			}
		} else {
			if (!modelObject["docs"].is<JsonArrayConst>()) {
				return FreshResult::failure(FreshStatus::CorruptData, "document backup docs are missing");
			}
			for (JsonVariantConst entry : modelObject["docs"].as<JsonArrayConst>()) {
				JsonDocument copy;
				FreshResult cloneResult = FreshCloneJson(copy, entry, "backup document");
				if (!cloneResult) {
					return cloneResult;
				}
				FreshResult sizeResult =
				    checkPayloadSize(measureMsgPack(copy), _config.maxDocumentBytes, "document");
				if (!sizeResult) {
					return sizeResult;
				}
				const char *id = copy["_id"] | "";
				if (*id == '\0') {
					return FreshResult::failure(FreshStatus::CorruptData, "backup document is missing id");
				}
				if (state->docs.find(id) != state->docs.end()) {
					return FreshResult::failure(FreshStatus::CorruptData, "backup contains duplicate document id");
				}
				state->docs[id] = std::move(copy);
			}
		}

		importedModels[modelName] = state;
	}

	size_t affectedCount = 0;
	{
		FreshLock lock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		for (const auto &entry : importedModels) {
			const std::string &name = entry.first;
			const auto &incoming = entry.second;
			auto found = _models.find(name);
			std::shared_ptr<FreshModel::State> target;
			if (found == _models.end()) {
				target = incoming;
				_models[name] = target;
			} else {
				target = found->second;
				std::map<std::string, JsonDocument> clonedDocs;
				std::deque<JsonDocument> clonedStreamEntries;
				for (const auto &docEntry : incoming->docs) {
					JsonDocument clone;
					FreshResult cloneResult =
					    FreshCloneJson(clone, docEntry.second.as<JsonVariantConst>(), "backup document");
					if (!cloneResult) {
						return cloneResult;
					}
					clonedDocs[docEntry.first] = std::move(clone);
				}
				for (const JsonDocument &streamEntry : incoming->streamEntries) {
					JsonDocument clone;
					FreshResult cloneResult =
					    FreshCloneJson(clone, streamEntry.as<JsonVariantConst>(), "backup stream entry");
					if (!cloneResult) {
						return cloneResult;
					}
					clonedStreamEntries.push_back(std::move(clone));
				}
				target->name = incoming->name;
				target->type = incoming->type;
				target->docs = std::move(clonedDocs);
				target->streamEntries = std::move(clonedStreamEntries);
				target->pending.clear();
				target->dropped = false;
				target->degraded = false;
				target->recordsSinceSnapshot = 0;
				target->bytesSinceSnapshot = 0;
				target->checkpointSequence = 0;
				target->lastSequence = 0;
				target->dirty = true;
				target->snapshotRequired = true;
				target->storageEpoch++;
			}
			affectedCount++;
		}

		for (auto &entry : _models) {
			if (importedModels.find(entry.first) != importedModels.end()) {
				continue;
			}
			const auto &state = entry.second;
			if (!state->dropped) {
				state->dropped = true;
				state->dirty = true;
				state->snapshotRequired = false;
				state->pending.clear();
				state->storageEpoch++;
				affectedCount++;
			}
		}
		_manifestDirty = true;
		_manifestEpoch++;
	}

	if (_syncTaskHandle != nullptr) {
		xTaskNotifyGive(_syncTaskHandle);
	}
	return FreshResult::success("backup imported", affectedCount);
}
