#include "Fresh.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <LittleFS.h>

#include <utility>

FreshModel::FreshModel(Fresh *owner, std::shared_ptr<State> state) : _owner(owner), _state(state) {
}

FreshResult FreshModel::validateLocked(
    bool requireType,
    FreshModelType requiredType,
    const char *unsupportedMessage
) const {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (!_owner->_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	if (_state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "model was dropped");
	}
	if (requireType && _state->type != requiredType) {
		return FreshResult::failure(FreshStatus::UnsupportedOperation, unsupportedMessage);
	}
	return FreshResult::success();
}

FreshModel::operator bool() const {
	if (_owner == nullptr || _state == nullptr) {
		return false;
	}
	FreshLock lock(*_owner->_mutex);
	return lock && static_cast<bool>(validateLocked());
}

std::string FreshModel::name() const {
	if (_owner == nullptr || _state == nullptr) {
		return {};
	}
	FreshLock lock(*_owner->_mutex);
	if (!lock || !validateLocked()) {
		return {};
	}
	return _state->name;
}

FreshModelType FreshModel::type() const {
	if (_owner == nullptr || _state == nullptr) {
		return FreshModelType::General;
	}
	FreshLock lock(*_owner->_mutex);
	if (!lock || !validateLocked()) {
		return FreshModelType::General;
	}
	return _state->type;
}

FreshResult FreshModel::setValidator(FreshBoolValidator validator) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	FreshResult valid = validateLocked();
	if (!valid) {
		return valid;
	}
	_state->validator = [validator](const JsonDocument &doc) {
		if (!validator || validator(doc)) {
			return FreshValidationResult{.result = true, .message = "ok"};
		}
		return FreshValidationResult{.result = false, .message = "validation failed"};
	};
	return FreshResult::success("validator set");
}

FreshResult FreshModel::setValidator(FreshResultValidator validator) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	FreshResult valid = validateLocked();
	if (!valid) {
		return valid;
	}
	_state->validator = validator;
	return FreshResult::success("validator set");
}

FreshResult FreshModel::create(JsonDocument &doc) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}

	const uint64_t time = _owner->now();
	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "create is only valid for general models"
		);
		if (!valid) {
			return valid;
		}

		std::string id = doc["_id"] | "";
		if (id.empty() || _state->docs.find(id) != _state->docs.end()) {
			do {
				id = FreshMakeId();
			} while (_state->docs.find(id) != _state->docs.end());
			doc["_id"] = id;
		}

		doc["createdAt"] = time;
		doc["updatedAt"] = time;

		JsonDocument stored;
		FreshResult cloneResult = FreshCloneJson(stored, doc.as<JsonVariantConst>(), "document");
		if (!cloneResult) {
			return cloneResult;
		}
		FreshResult sizeResult =
		    _owner->checkPayloadSize(measureMsgPack(stored), _owner->_config.maxDocumentBytes, "document");
		if (!sizeResult) {
			return sizeResult;
		}
		if (_state->validator) {
			FreshValidationResult validation = _state->validator(stored);
			if (!validation) {
				return FreshResult::failure(FreshStatus::ValidationFailed, validation.message.c_str());
			}
		}

		FreshPendingRecord record;
		record.op = FreshJournalOp::Create;
		record.sequence = _owner->_nextPendingSequence;
		record.id = id;
		cloneResult = FreshCloneJson(record.doc, stored.as<JsonVariantConst>(), "journal document");
		if (!cloneResult) {
			return cloneResult;
		}
		JsonDocument recordDoc = _owner->recordToJson(record);
		sizeResult = _owner->checkPayloadSize(
		    measureMsgPack(recordDoc),
		    _owner->_config.maxJournalRecordBytes,
		    "journal record"
		);
		if (!sizeResult) {
			return sizeResult;
		}

		JsonDocument resultDoc;
		cloneResult = FreshCloneJson(resultDoc, stored.as<JsonVariantConst>(), "result document");
		if (!cloneResult) {
			return cloneResult;
		}

		_owner->_nextPendingSequence++;
		_state->lastSequence = record.sequence;
		_state->docs[id] = std::move(stored);
		_state->pending.push_back(std::move(record));
		_state->dirty = true;

		result = FreshResult::success("document created", 1);
		result.doc = std::move(resultDoc);
		event = {
		    .type = FreshEventType::DocumentCreated,
		    .modelName = _state->name,
		    .documentId = id,
		    .affectedCount = 1,
		    .result = result
		};
	}
	_owner->emitEvent(event);
	return result;
}

FreshResult FreshModel::append(JsonDocument &doc) {
	return append(doc, FreshStreamAppendOptions());
}

FreshResult FreshModel::append(JsonDocument &doc, const FreshStreamAppendOptions &options) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::Stream,
		    "append is only valid for stream models"
		);
		if (!valid) {
			return valid;
		}

		JsonDocument stored;
		FreshResult cloneResult = FreshCloneJson(stored, doc.as<JsonVariantConst>(), "stream entry");
		if (!cloneResult) {
			return cloneResult;
		}
		FreshResult sizeResult = _owner->checkPayloadSize(
		    measureMsgPack(stored),
		    _owner->_config.maxDocumentBytes,
		    "stream entry"
		);
		if (!sizeResult) {
			return sizeResult;
		}

		FreshPendingRecord record;
		record.op = FreshJournalOp::Append;
		record.sequence = _owner->_nextPendingSequence;
		record.maxEntries = options.maxEntries;
		cloneResult = FreshCloneJson(record.doc, stored.as<JsonVariantConst>(), "journal stream entry");
		if (!cloneResult) {
			return cloneResult;
		}
		JsonDocument recordDoc = _owner->recordToJson(record);
		sizeResult = _owner->checkPayloadSize(
		    measureMsgPack(recordDoc),
		    _owner->_config.maxJournalRecordBytes,
		    "journal record"
		);
		if (!sizeResult) {
			return sizeResult;
		}

		_owner->_nextPendingSequence++;
		_state->streamEntries.push_back(std::move(stored));
		while (options.maxEntries > 0 && _state->streamEntries.size() > options.maxEntries) {
			_state->streamEntries.pop_front();
		}
		_state->lastSequence = record.sequence;
		_state->pending.push_back(std::move(record));
		_state->dirty = true;

		result = FreshResult::success("stream entry appended", 1);
		event = {
		    .type = FreshEventType::StreamAppended,
		    .modelName = _state->name,
		    .affectedCount = 1,
		    .result = result
		};
	}
	_owner->emitEvent(event);
	return result;
}

FreshResult FreshModel::findById(const char *id) const {
	if (id == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "id is required");
	}
	return findById(std::string(id));
}

FreshResult FreshModel::findById(const std::string &id) const {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	FreshResult valid = validateLocked(
	    true,
	    FreshModelType::General,
	    "findById is only valid for general models"
	);
	if (!valid) {
		return valid;
	}
	auto found = _state->docs.find(id);
	if (found == _state->docs.end()) {
		return FreshResult::failure(FreshStatus::DocumentNotFound, "document not found");
	}
	FreshResult result = FreshResult::success("document found", 1);
	FreshResult cloneResult = FreshCloneJson(result.doc, found->second.as<JsonVariantConst>(), "result document");
	return cloneResult ? result : cloneResult;
}

FreshResult FreshModel::find(FreshPredicate predicate, bool stopAtFirst) const {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}

	FreshLock lock(*_owner->_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	FreshResult valid = validateLocked(
	    true,
	    FreshModelType::General,
	    "find is only valid for general models"
	);
	if (!valid) {
		return valid;
	}
	JsonDocument resultDoc(&FreshJsonAllocator());
	JsonArray array = resultDoc.to<JsonArray>();
	size_t affectedCount = 0;
	for (const auto &entry : _state->docs) {
		if (!predicate(entry.second)) {
			continue;
		}
		if (!array.add(entry.second.as<JsonVariantConst>()) || resultDoc.overflowed()) {
			return FreshResult::failure(
			    FreshStatus::OutOfMemory,
			    "failed to construct find result"
			);
		}
		affectedCount++;
		if (stopAtFirst) {
			break;
		}
	}
	resultDoc.shrinkToFit();
	FreshResult result = FreshResult::success(
	    affectedCount == 0 ? "no documents found" : "documents found",
	    affectedCount
	);
	result.doc = std::move(resultDoc);
	return result;
}

FreshResult FreshModel::updateById(const char *id, const JsonDocument &patch, FreshReturn returnMode) {
	if (id == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "id is required");
	}
	return updateById(std::string(id), patch, returnMode);
}

FreshResult FreshModel::updateById(
    const std::string &id,
    const JsonDocument &patch,
    FreshReturn returnMode
) {
	return updateOne(
	    [id](const JsonDocument &doc) {
		    const char *docId = doc["_id"] | "";
		    return id == docId;
	    },
	    patch,
	    returnMode
	);
}

FreshResult FreshModel::updateOne(
    FreshPredicate predicate,
    const JsonDocument &patch,
    FreshReturn returnMode
) {
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}
	return update(
	    [predicate, updated = false](const JsonDocument &doc) mutable {
		    if (updated || !predicate(doc)) {
			    return false;
		    }
		    updated = true;
		    return true;
	    },
	    patch,
	    returnMode
	);
}

FreshResult FreshModel::update(
    FreshPredicate predicate,
    const JsonDocument &patch,
    FreshReturn returnMode
) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}

	const uint64_t updateTime = _owner->now();
	std::vector<FreshEvent> events;
	FreshResult result = FreshResult::success("documents updated");
	{
		struct FreshUpdateCandidate {
			std::string id;
			JsonDocument doc;
			FreshPendingRecord record;
		};
		std::vector<FreshUpdateCandidate> candidates;
		FreshLock lock(*_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "update is only valid for general models"
		);
		if (!valid) {
			return valid;
		}

		uint64_t nextSequence = _owner->_nextPendingSequence;
		for (const auto &entry : _state->docs) {
			if (!predicate(entry.second)) {
				continue;
			}
			JsonDocument candidate;
			FreshResult cloneResult =
			    FreshCloneJson(candidate, entry.second.as<JsonVariantConst>(), "existing document");
			if (!cloneResult) {
				return cloneResult;
			}
			FreshMergePatch(candidate, patch);
			candidate["updatedAt"] = updateTime;

			JsonDocument storedCandidate;
			cloneResult = FreshCloneJson(
			    storedCandidate,
			    candidate.as<JsonVariantConst>(),
			    "updated document"
			);
			if (!cloneResult) {
				return cloneResult;
			}
			FreshResult sizeResult = _owner->checkPayloadSize(
			    measureMsgPack(storedCandidate),
			    _owner->_config.maxDocumentBytes,
			    "document"
			);
			if (!sizeResult) {
				return sizeResult;
			}
			if (_state->validator) {
				FreshValidationResult validation = _state->validator(storedCandidate);
				if (!validation) {
					return FreshResult::failure(
					    FreshStatus::ValidationFailed,
					    validation.message.c_str(),
					    result.affectedCount
					);
				}
			}

			FreshPendingRecord record;
			record.op = FreshJournalOp::Update;
			record.sequence = nextSequence++;
			record.id = entry.first;
			cloneResult = FreshCloneJson(
			    record.doc,
			    storedCandidate.as<JsonVariantConst>(),
			    "journal document"
			);
			if (!cloneResult) {
				return cloneResult;
			}
			JsonDocument recordDoc = _owner->recordToJson(record);
			sizeResult = _owner->checkPayloadSize(
			    measureMsgPack(recordDoc),
			    _owner->_config.maxJournalRecordBytes,
			    "journal record"
			);
			if (!sizeResult) {
				return sizeResult;
			}
			candidates.push_back(
			    FreshUpdateCandidate{
			        .id = entry.first,
			        .doc = std::move(storedCandidate),
			        .record = std::move(record)
			    }
			);
		}

		JsonDocument resultDoc(&FreshJsonAllocator());
		if (!candidates.empty() && returnMode == FreshReturn::ChangedDocs) {
			JsonArray array = resultDoc.to<JsonArray>();
			for (const FreshUpdateCandidate &candidate : candidates) {
				if (!array.add(candidate.doc.as<JsonVariantConst>()) || resultDoc.overflowed()) {
					return FreshResult::failure(
					    FreshStatus::OutOfMemory,
					    "failed to construct changed documents result"
					);
				}
			}
		} else if (!candidates.empty() && returnMode == FreshReturn::AllDocs) {
			JsonArray array = resultDoc.to<JsonArray>();
			for (const auto &entry : _state->docs) {
				const JsonDocument *document = &entry.second;
				for (const FreshUpdateCandidate &candidate : candidates) {
					if (candidate.id == entry.first) {
						document = &candidate.doc;
						break;
					}
				}
				if (!array.add(document->as<JsonVariantConst>()) || resultDoc.overflowed()) {
					return FreshResult::failure(
					    FreshStatus::OutOfMemory,
					    "failed to construct all documents result"
					);
				}
			}
		}
		if (!candidates.empty() && returnMode != FreshReturn::None) {
			resultDoc.shrinkToFit();
			result.doc = std::move(resultDoc);
		}

		_owner->_nextPendingSequence = nextSequence;
		for (FreshUpdateCandidate &candidate : candidates) {
			auto found = _state->docs.find(candidate.id);
			if (found == _state->docs.end()) {
				continue;
			}
			_state->lastSequence = candidate.record.sequence;
			found->second = std::move(candidate.doc);
			_state->pending.push_back(std::move(candidate.record));
			_state->dirty = true;
			result.affectedCount++;
			events.push_back({
			    .type = FreshEventType::DocumentUpdated,
			    .modelName = _state->name,
			    .documentId = candidate.id,
			    .affectedCount = 1,
			    .result = FreshResult::success("document updated", 1)
			});
		}
		if (result.affectedCount == 0) {
			result.message = "no documents updated";
		}
	}
	for (const FreshEvent &event : events) {
		_owner->emitEvent(event);
	}
	return result;
}

FreshResult FreshModel::deleteById(const char *id) {
	if (id == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "id is required");
	}
	return deleteById(std::string(id));
}

FreshResult FreshModel::deleteById(const std::string &id) {
	return deleteOne(
	    [id](const JsonDocument &doc) {
		    const char *docId = doc["_id"] | "";
		    return id == docId;
	    }
	);
}

FreshResult FreshModel::deleteOne(FreshPredicate predicate) {
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}
	return deleteMany(
	    [predicate, deleted = false](const JsonDocument &doc) mutable {
		    if (deleted || !predicate(doc)) {
			    return false;
		    }
		    deleted = true;
		    return true;
	    }
	);
}

FreshResult FreshModel::deleteMany(FreshPredicate predicate) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}

	std::vector<FreshEvent> events;
	FreshResult result = FreshResult::success("documents deleted");
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "delete is only valid for general models"
		);
		if (!valid) {
			return valid;
		}

		std::vector<std::string> ids;
		for (const auto &entry : _state->docs) {
			if (predicate(entry.second)) {
				ids.push_back(entry.first);
			}
		}

		std::vector<FreshPendingRecord> records;
		records.reserve(ids.size());
		uint64_t nextSequence = _owner->_nextPendingSequence;
		for (const std::string &id : ids) {
			FreshPendingRecord record;
			record.op = FreshJournalOp::Delete;
			record.sequence = nextSequence++;
			record.id = id;
			JsonDocument recordDoc = _owner->recordToJson(record);
			FreshResult sizeResult = _owner->checkPayloadSize(
			    measureMsgPack(recordDoc),
			    _owner->_config.maxJournalRecordBytes,
			    "journal record"
			);
			if (!sizeResult) {
				return sizeResult;
			}
			records.push_back(std::move(record));
		}

		_owner->_nextPendingSequence = nextSequence;
		for (FreshPendingRecord &record : records) {
			_state->docs.erase(record.id);
			_state->lastSequence = record.sequence;
			const std::string id = record.id;
			_state->pending.push_back(std::move(record));
			_state->dirty = true;
			result.affectedCount++;
			events.push_back({
			    .type = FreshEventType::DocumentDeleted,
			    .modelName = _state->name,
			    .documentId = id,
			    .affectedCount = 1,
			    .result = FreshResult::success("document deleted", 1)
			});
		}
		if (result.affectedCount == 0) {
			result.message = "no documents deleted";
		}
	}
	for (const FreshEvent &event : events) {
		_owner->emitEvent(event);
	}
	return result;
}

FreshResult FreshModel::retrieve() const {
	return retrieve(nullptr, FreshStreamRetrieveOptions());
}

FreshResult FreshModel::retrieve(const FreshStreamRetrieveOptions &options) const {
	return retrieve(nullptr, options);
}

FreshResult FreshModel::retrieve(
    FreshPredicate predicate,
    const FreshStreamRetrieveOptions &options
) const {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	FreshResult valid = validateLocked(
	    true,
	    FreshModelType::Stream,
	    "retrieve is only valid for stream models"
	);
	if (!valid) {
		return valid;
	}

	JsonDocument resultDoc(&FreshJsonAllocator());
	JsonArray array = resultDoc.to<JsonArray>();
	size_t skipped = 0;
	size_t affectedCount = 0;
	const size_t total = _state->streamEntries.size();
	for (size_t i = 0; i < total; ++i) {
		const size_t index = options.reverse ? total - 1 - i : i;
		const JsonDocument &entry = _state->streamEntries[index];
		if (predicate && !predicate(entry)) {
			continue;
		}
		if (skipped < options.offset) {
			skipped++;
			continue;
		}
		if (!array.add(entry.as<JsonVariantConst>()) || resultDoc.overflowed()) {
			return FreshResult::failure(
			    FreshStatus::OutOfMemory,
			    "failed to construct stream result"
			);
		}
		affectedCount++;
		if (options.limit > 0 && affectedCount >= options.limit) {
			break;
		}
	}
	resultDoc.shrinkToFit();
	FreshResult result = FreshResult::success(
	    affectedCount == 0 ? "no stream entries found" : "stream entries found",
	    affectedCount
	);
	result.doc = std::move(resultDoc);
	return result;
}

FreshResult FreshModel::streamTo(Print &out) const {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	FreshResult valid = validateLocked(
	    true,
	    FreshModelType::Stream,
	    "streamTo is only valid for stream models"
	);
	if (!valid) {
		return valid;
	}
	size_t count = 0;
	for (const JsonDocument &entry : _state->streamEntries) {
		count += serializeMsgPack(entry, out);
	}
	return FreshResult::success("stream exported", count);
}

FreshModelResult Fresh::createModel(const char *modelName) {
	FreshModelType type = FreshModelType::General;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return {
			    .result = false,
			    .status = FreshStatus::InternalError,
			    .message = "failed to lock database"
			};
		}
		if (!_initialized) {
			return {
			    .result = false,
			    .status = FreshStatus::NotInitialized,
			    .message = "database not initialized"
			};
		}
		type = _config.defaultModelType;
	}
	return createModel(modelName, type);
}

FreshModel Fresh::model(const char *modelName) {
	if (!FreshIsValidName(modelName)) {
		return FreshModel();
	}
	FreshLock lock(*_mutex);
	if (!lock || !_initialized || _stopping) {
		return FreshModel();
	}
	auto found = _models.find(modelName);
	if (found == _models.end() || found->second->dropped) {
		return FreshModel();
	}
	return FreshModel(this, found->second);
}

FreshModelResult Fresh::createModel(const char *modelName, FreshModelType type) {
	if (!FreshIsValidName(modelName)) {
		return {
		    .result = false,
		    .status = FreshStatus::InvalidArgument,
		    .message = "invalid model name"
		};
	}

	FreshEvent event;
	std::shared_ptr<FreshModel::State> state;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return {
			    .result = false,
			    .status = FreshStatus::InternalError,
			    .message = "failed to lock database"
			};
		}
		if (!_initialized) {
			return {
			    .result = false,
			    .status = FreshStatus::NotInitialized,
			    .message = "database not initialized"
			};
		}
		if (_stopping) {
			return {
			    .result = false,
			    .status = FreshStatus::Busy,
			    .message = "database is stopping"
			};
		}
		auto existing = _models.find(modelName);
		if (existing != _models.end()) {
			if (existing->second->dropped) {
				return {
				    .result = false,
				    .status = FreshStatus::InvalidModel,
				    .message = "model was dropped"
				};
			}
			if (existing->second->type != type) {
				return {
				    .result = false,
				    .status = FreshStatus::ModelExists,
				    .message = "model already exists with different type"
				};
			}
			return {
			    .result = true,
			    .status = FreshStatus::Ok,
			    .message = "model opened",
			    .model = FreshModel(this, existing->second),
			    .affectedCount = 1
			};
		}

		std::string storageId;
		bool duplicate = false;
		do {
			storageId = FreshMakeId();
			duplicate = LittleFS.exists(modelPath(storageId).c_str());
			for (const auto &entry : _models) {
				if (entry.second->storageId == storageId) {
					duplicate = true;
					break;
				}
			}
		} while (duplicate);

		state = std::make_shared<FreshModel::State>();
		state->name = modelName;
		state->storageId = storageId;
		state->type = type;
		state->dirty = true;
		_models[state->name] = state;
		_manifestDirty = true;
		_manifestEpoch++;
		event = {
		    .type = FreshEventType::ModelCreated,
		    .modelName = state->name,
		    .result = FreshResult::success("model created")
		};
	}
	emitEvent(event);
	return {
	    .result = true,
	    .status = FreshStatus::Ok,
	    .message = "model created",
	    .model = FreshModel(this, state),
	    .affectedCount = 1
	};
}

FreshResult Fresh::dropModel(const char *modelName) {
	if (!FreshIsValidName(modelName)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "invalid model name");
	}

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		auto found = _models.find(modelName);
		if (found == _models.end()) {
			return FreshResult::failure(FreshStatus::ModelNotFound, "model not found");
		}
		if (found->second->dropped) {
			return FreshResult::failure(FreshStatus::InvalidModel, "model was dropped");
		}
		found->second->dropped = true;
		found->second->dirty = true;
		found->second->snapshotRequired = false;
		found->second->pending.clear();
		found->second->storageEpoch++;
		_manifestDirty = true;
		_manifestEpoch++;
		result = FreshResult::success("model dropped", 1);
		event = {
		    .type = FreshEventType::ModelDropped,
		    .modelName = modelName,
		    .affectedCount = 1,
		    .result = result
		};
	}
	emitEvent(event);
	return result;
}

FreshResult Fresh::dropModels(std::initializer_list<const char *> modelNames) {
	size_t affected = 0;
	for (const char *name : modelNames) {
		FreshResult result = dropModel(name);
		if (result) {
			affected++;
		}
	}
	return FreshResult::success("models dropped", affected);
}

FreshResult Fresh::dropAllModels() {
	std::vector<std::string> names;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		for (const auto &entry : _models) {
			if (!entry.second->dropped) {
				names.push_back(entry.first);
			}
		}
	}
	size_t affected = 0;
	for (const std::string &name : names) {
		if (dropModel(name.c_str())) {
			affected++;
		}
	}
	return FreshResult::success("all models dropped", affected);
}

FreshResult Fresh::renameModel(const char *oldName, const char *newName) {
	if (!FreshIsValidName(oldName) || !FreshIsValidName(newName)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "invalid model name");
	}
	if (strcmp(oldName, newName) == 0) {
		return FreshResult::success("model name unchanged");
	}

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		auto found = _models.find(oldName);
		if (found == _models.end()) {
			return FreshResult::failure(FreshStatus::ModelNotFound, "model not found");
		}
		if (found->second->dropped) {
			return FreshResult::failure(FreshStatus::InvalidModel, "model was dropped");
		}
		if (_models.find(newName) != _models.end()) {
			return FreshResult::failure(FreshStatus::ModelExists, "target model already exists");
		}

		auto state = found->second;
		_models.erase(found);
		state->name = newName;
		_models[state->name] = state;
		_manifestDirty = true;
		_manifestEpoch++;
		result = FreshResult::success("model renamed", 1);
		event = {
		    .type = FreshEventType::ModelRenamed,
		    .modelName = newName,
		    .previousModelName = oldName,
		    .affectedCount = 1,
		    .result = result
		};
	}
	emitEvent(event);
	return result;
}
