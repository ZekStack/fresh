#include "Fresh.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <cstring>
#include <map>
#include <utility>

using namespace fresh_backup_v2;

namespace {

FreshResult backupCorrupt(const char *message) {
	return FreshResult::failure(FreshStatus::CorruptData, message);
}

FreshResult backupOutOfMemory(const char *message) {
	return FreshResult::failure(FreshStatus::OutOfMemory, message);
}

bool readPrefix(Stream &input, uint8_t prefix[4]) {
	return input.readBytes(reinterpret_cast<char *>(prefix), 4) == 4;
}

} // namespace

FreshResult Fresh::backupImport(Stream &input) {
	{
		FreshLock backupLock(_backup->mutex);
		if (_backup->running || _backup->requested) {
			return FreshResult::failure(FreshStatus::Busy, "backup already running");
		}
	}

	uint8_t prefix[4]{};
	if (!readPrefix(input, prefix)) return backupCorrupt("truncated backup");
	PrefixStream prefixed(prefix, sizeof(prefix), input);
	if (!hasMagic(prefix)) {
		JsonDocument archive(&FreshJsonAllocator());
		DeserializationError error = deserializeMsgPack(archive, prefixed);
		if (error || archive.overflowed()) {
			return FreshResult::failure(
			    error == DeserializationError::NoMemory || archive.overflowed()
			        ? FreshStatus::OutOfMemory
			        : FreshStatus::CorruptData,
			    "failed to read legacy backup"
			);
		}
		return importBackupArchive(archive);
	}

	ContainerHeader header;
	Reader reader(prefixed);
	const char *parseError = nullptr;
	if (!reader.readContainerHeader(header, parseError)) return backupCorrupt(parseError);
	if (header.modelCount > SIZE_MAX || header.recordCount > SIZE_MAX || header.totalBytes > SIZE_MAX) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup counters exceed platform limits");
	}

	uint64_t capturedDatabaseRevision = 0;
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
		oldModels = _models;
		for (const auto &entry : oldModels) oldRevisions[entry.first] = entry.second->revision;
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> importedModels;
	std::shared_ptr<FreshModel::State> currentModel;
	uint64_t currentDeclaredRecords = 0;
	uint64_t currentReadRecords = 0;
	uint64_t parsedRecords = 0;
	uint32_t parsedModels = 0;
	bool finished = false;

	while (!finished) {
		if (reader.bytesRead() >= header.totalBytes) return backupCorrupt("backup is missing archive trailer");
		FrameHeader frame;
		if (!reader.readFrameHeader(frame, parseError)) return backupCorrupt(parseError);
		switch (frame.type) {
		case FrameType::ModelBegin:
			if (currentModel || frame.payloadSize < ModelBeginFixedPayloadSize ||
			    frame.payloadSize > MaxMetadataPayloadSize) {
				return backupCorrupt("invalid model-begin frame");
			}
			break;
		case FrameType::Record:
			if (!currentModel || frame.payloadSize == 0 || frame.payloadSize > _config.maxDocumentBytes) {
				return frame.payloadSize > _config.maxDocumentBytes
				           ? FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup record is too large")
				           : backupCorrupt("invalid record frame");
			}
			break;
		case FrameType::ModelEnd:
			if (!currentModel || frame.payloadSize != ModelEndPayloadSize) {
				return backupCorrupt("invalid model-end frame");
			}
			break;
		case FrameType::ArchiveEnd:
			if (currentModel || frame.payloadSize != ArchiveEndPayloadSize) {
				return backupCorrupt("invalid archive-end frame");
			}
			break;
		}

		FreshBuffer payload;
		if (!reader.readFramePayload(frame, payload, parseError)) {
			if (std::strcmp(parseError, "failed to allocate backup frame") == 0) {
				return backupOutOfMemory(parseError);
			}
			return backupCorrupt(parseError);
		}
		if (reader.bytesRead() > header.totalBytes) return backupCorrupt("backup exceeds declared byte count");

		switch (frame.type) {
		case FrameType::ModelBegin: {
			const uint8_t typeByte = payload[0];
			if (typeByte > 1 || payload[1] != 0) return backupCorrupt("invalid backup model type");
			const uint16_t nameLength = getU16(payload.data() + 2);
			currentDeclaredRecords = getU64(payload.data() + 4);
			if (nameLength == 0 || payload.size() != ModelBeginFixedPayloadSize + nameLength ||
			    currentDeclaredRecords > SIZE_MAX) {
				return backupCorrupt("invalid backup model metadata");
			}
			std::string name(reinterpret_cast<const char *>(payload.data() + ModelBeginFixedPayloadSize), nameLength);
			if (!FreshIsValidName(name.c_str()) || importedModels.find(name) != importedModels.end() ||
			    parsedModels >= header.modelCount) {
				return backupCorrupt("backup contains invalid or duplicate model");
			}
			currentModel = std::make_shared<FreshModel::State>();
			if (!currentModel) return backupOutOfMemory("failed to allocate imported model");
			currentModel->name = name;
			currentModel->type = typeByte == 1 ? FreshModelType::Stream : FreshModelType::General;
			currentModel->dirty = true;
			currentModel->snapshotRequired = true;
			auto old = oldModels.find(name);
			if (old != oldModels.end()) {
				if (old->second->storageEpoch == UINT32_MAX) {
					return FreshResult::failure(FreshStatus::InternalError, "storage epoch overflow");
				}
				currentModel->storageId = old->second->storageId;
				currentModel->storageEpoch = old->second->storageEpoch + 1;
				currentModel->validator = old->second->validator;
			}
			currentReadRecords = 0;
			break;
		}
		case FrameType::Record: {
			if (currentReadRecords >= currentDeclaredRecords || parsedRecords >= header.recordCount) {
				return backupCorrupt("backup contains too many records");
			}
			JsonDocument document(&FreshJsonAllocator());
			DeserializationError error = deserializeMsgPack(document, payload.data(), payload.size());
			if (error || document.overflowed()) {
				return FreshResult::failure(
				    error == DeserializationError::NoMemory || document.overflowed()
				        ? FreshStatus::OutOfMemory
				        : FreshStatus::CorruptData,
				    "failed to decode backup record"
				);
			}
			FreshResult sizeResult = checkPayloadSize(
			    measureMsgPack(document),
			    _config.maxDocumentBytes,
			    currentModel->type == FreshModelType::Stream ? "stream entry" : "document"
			);
			if (!sizeResult) return sizeResult;
			if (currentModel->validator) {
				FreshValidationResult validation = currentModel->validator(document);
				if (!validation) {
					return FreshResult::failure(FreshStatus::ValidationFailed, validation.message.c_str());
				}
			}
			if (currentModel->type == FreshModelType::Stream) {
				currentModel->streamEntries.push_back(std::move(document));
			} else {
				if (!document.is<JsonObjectConst>()) return backupCorrupt("backup document must be an object");
				const char *id = document["_id"] | "";
				if (*id == '\0' || currentModel->docs.find(id) != currentModel->docs.end()) {
					return backupCorrupt("backup contains invalid document id");
				}
				currentModel->docs[id] = std::move(document);
			}
			currentReadRecords++;
			parsedRecords++;
			break;
		}
		case FrameType::ModelEnd: {
			const uint64_t endCount = getU64(payload.data());
			if (endCount != currentDeclaredRecords || currentReadRecords != currentDeclaredRecords) {
				return backupCorrupt("backup model record count mismatch");
			}
			importedModels[currentModel->name] = currentModel;
			currentModel.reset();
			currentDeclaredRecords = 0;
			currentReadRecords = 0;
			parsedModels++;
			break;
		}
		case FrameType::ArchiveEnd: {
			const uint32_t endModels = getU32(payload.data());
			const uint64_t endRecords = getU64(payload.data() + 4);
			if (endModels != header.modelCount || endRecords != header.recordCount ||
			    parsedModels != header.modelCount || parsedRecords != header.recordCount) {
				return backupCorrupt("backup archive count mismatch");
			}
			finished = true;
			break;
		}
		}
	}

	if (reader.bytesRead() != header.totalBytes) return backupCorrupt("backup byte count mismatch");
	if (prefixed.available() > 0) return backupCorrupt("backup contains trailing bytes");

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
	(void)syncTaskHandle;
	if (syncTaskHandle != nullptr) xTaskNotifyGive(syncTaskHandle);
	return FreshResult::success("backup imported", affectedCount);
}

FreshResult Fresh::backupImport(const uint8_t *data, size_t length) {
	if (data == nullptr || length == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "backup buffer is required");
	}
	MemoryStream input(data, length);
	return backupImport(input);
}

FreshResult Fresh::importBackupArchive(const JsonDocument &archive) {
	if ((archive["version"] | 0U) != FreshBackupVersion ||
	    !archive["modelCount"].is<uint64_t>() ||
	    !archive["models"].is<JsonArrayConst>()) {
		return backupCorrupt("unsupported or corrupt legacy backup");
	}
	const uint64_t declaredModelCount = archive["modelCount"].as<uint64_t>();
	const JsonArrayConst modelArray = archive["models"].as<JsonArrayConst>();
	if (declaredModelCount > SIZE_MAX || static_cast<size_t>(declaredModelCount) != modelArray.size()) {
		return backupCorrupt("legacy backup model count mismatch");
	}

	uint64_t capturedDatabaseRevision = 0;
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
		oldModels = _models;
		for (const auto &entry : oldModels) oldRevisions[entry.first] = entry.second->revision;
	}

	std::map<std::string, std::shared_ptr<FreshModel::State>> importedModels;
	for (JsonObjectConst modelObject : modelArray) {
		const char *name = modelObject["name"] | "";
		const char *typeName = modelObject["type"] | "";
		if (!FreshIsValidName(name) ||
		    (std::strcmp(typeName, "general") != 0 && std::strcmp(typeName, "stream") != 0) ||
		    !modelObject["recordCount"].is<uint64_t>()) {
			return backupCorrupt("legacy backup contains invalid model metadata");
		}
		const std::string modelName = name;
		if (importedModels.find(modelName) != importedModels.end()) {
			return backupCorrupt("legacy backup contains duplicate model");
		}
		const FreshModelType type = FreshModelTypeFromString(typeName);
		const char *arrayName = type == FreshModelType::Stream ? "entries" : "docs";
		if (!modelObject[arrayName].is<JsonArrayConst>()) return backupCorrupt("legacy backup records are missing");
		const uint64_t declaredRecordCount = modelObject["recordCount"].as<uint64_t>();
		const JsonArrayConst records = modelObject[arrayName].as<JsonArrayConst>();
		if (declaredRecordCount > SIZE_MAX || static_cast<size_t>(declaredRecordCount) != records.size()) {
			return backupCorrupt("legacy backup record count mismatch");
		}

		auto state = std::make_shared<FreshModel::State>();
		if (!state) return backupOutOfMemory("failed to allocate imported model");
		state->name = modelName;
		state->type = type;
		state->dirty = true;
		state->snapshotRequired = true;
		auto old = oldModels.find(modelName);
		if (old != oldModels.end()) {
			if (old->second->storageEpoch == UINT32_MAX) {
				return FreshResult::failure(FreshStatus::InternalError, "storage epoch overflow");
			}
			state->storageId = old->second->storageId;
			state->storageEpoch = old->second->storageEpoch + 1;
			state->validator = old->second->validator;
		}

		for (JsonVariantConst entry : records) {
			JsonDocument copy(&FreshJsonAllocator());
			FreshResult cloneResult = FreshCloneJson(
			    copy,
			    entry,
			    type == FreshModelType::Stream ? "legacy backup stream entry" : "legacy backup document"
			);
			if (!cloneResult) return cloneResult;
			FreshResult sizeResult = checkPayloadSize(
			    measureMsgPack(copy),
			    _config.maxDocumentBytes,
			    type == FreshModelType::Stream ? "stream entry" : "document"
			);
			if (!sizeResult) return sizeResult;
			if (state->validator) {
				FreshValidationResult validation = state->validator(copy);
				if (!validation) {
					return FreshResult::failure(FreshStatus::ValidationFailed, validation.message.c_str());
				}
			}
			if (type == FreshModelType::Stream) {
				state->streamEntries.push_back(std::move(copy));
			} else {
				if (!copy.is<JsonObjectConst>()) return backupCorrupt("legacy backup document must be an object");
				const char *id = copy["_id"] | "";
				if (*id == '\0' || state->docs.find(id) != state->docs.end()) {
					return backupCorrupt("legacy backup contains invalid document id");
				}
				state->docs[id] = std::move(copy);
			}
		}
		importedModels[modelName] = state;
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
	(void)syncTaskHandle;
	if (syncTaskHandle != nullptr) xTaskNotifyGive(syncTaskHandle);
	return FreshResult::success("backup imported", affectedCount);
}
