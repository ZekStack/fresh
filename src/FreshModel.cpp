#include "Fresh.h"
#include "internal/FreshInternal.h"

FreshModel::FreshModel(Fresh *owner, std::shared_ptr<State> state) : _owner(owner), _state(state) {
}

FreshModel::operator bool() const {
	if (_owner == nullptr || _state == nullptr) {
		return false;
	}
	FreshLock lock(*_owner->_mutex);
	return lock && _owner->_initialized && !_owner->_stopping && !_state->dropped;
}

const std::string &FreshModel::name() const {
	static const std::string empty;
	return _state ? _state->name : empty;
}

FreshModelType FreshModel::type() const {
	return _state ? _state->type : FreshModelType::General;
}

FreshResult FreshModel::setValidator(FreshBoolValidator validator) {
	if (!_owner || !_state) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!_owner->_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
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
	if (!_owner || !_state) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!_owner->_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	_state->validator = validator;
	return FreshResult::success("validator set");
}

FreshResult FreshModel::create(JsonDocument &doc) {
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (_state->type != FreshModelType::General) {
		return FreshResult::failure(FreshStatus::UnsupportedOperation, "create is only valid for general models");
	}

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_owner->_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_owner->_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}

		std::string id = doc["_id"] | "";
		if (id.empty() || _state->docs.find(id) != _state->docs.end()) {
			do {
				id = FreshMakeId();
			} while (_state->docs.find(id) != _state->docs.end());
			doc["_id"] = id;
		}

		uint64_t time = _owner->now();
		doc["createdAt"] = time;
		doc["updatedAt"] = time;

		FreshResult sizeResult =
		    _owner->checkPayloadSize(measureMsgPack(doc), _owner->_config.maxDocumentBytes, "document");
		if (!sizeResult) {
			return sizeResult;
		}

		if (_state->validator) {
			FreshValidationResult validation = _state->validator(doc);
			if (!validation) {
				return FreshResult::failure(FreshStatus::ValidationFailed, validation.message.c_str());
			}
		}

		JsonDocument stored;
		FreshCopyJson(stored, doc);
		_state->docs[id] = stored;

		FreshPendingRecord record;
		record.op = FreshJournalOp::Create;
		record.sequence = _owner->_nextPendingSequence++;
		record.id = id;
		FreshCopyJson(record.doc, stored);
		_state->pending.push_back(record);
		_state->dirty = true;

		result = FreshResult::success("document created", 1);
		FreshCopyJson(result.doc, stored);
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
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (_state->type != FreshModelType::Stream) {
		return FreshResult::failure(FreshStatus::UnsupportedOperation, "append is only valid for stream models");
	}

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_owner->_mutex);
		if (!_owner->_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_owner->_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		JsonDocument stored;
		FreshCopyJson(stored, doc);
		FreshResult sizeResult =
		    _owner->checkPayloadSize(measureMsgPack(stored), _owner->_config.maxDocumentBytes, "stream entry");
		if (!sizeResult) {
			return sizeResult;
		}
		_state->streamEntries.push_back(stored);

		FreshPendingRecord record;
		record.op = FreshJournalOp::Append;
		record.sequence = _owner->_nextPendingSequence++;
		FreshCopyJson(record.doc, stored);
		_state->pending.push_back(record);
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
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	FreshLock lock(*_owner->_mutex);
	if (!_owner->_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	auto found = _state->docs.find(id);
	if (found == _state->docs.end()) {
		return FreshResult::failure(FreshStatus::DocumentNotFound, "document not found");
	}
	FreshResult result = FreshResult::success("document found", 1);
	FreshCopyJson(result.doc, found->second);
	return result;
}

FreshResult FreshModel::find(FreshPredicate predicate, bool stopAtFirst) const {
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (_state->type != FreshModelType::General) {
		return FreshResult::failure(FreshStatus::UnsupportedOperation, "find is only valid for general models");
	}
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}

	FreshLock lock(*_owner->_mutex);
	if (!_owner->_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	FreshResult result = FreshResult::success("documents found");
	JsonArray array = result.doc.to<JsonArray>();
	for (const auto &entry : _state->docs) {
		if (!predicate(entry.second)) {
			continue;
		}
		array.add(entry.second.as<JsonVariantConst>());
		result.affectedCount++;
		if (stopAtFirst) {
			break;
		}
	}
	if (result.affectedCount == 0) {
		result.message = "no documents found";
	}
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
		    if (updated) {
			    return false;
		    }
		    if (!predicate(doc)) {
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
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (_state->type != FreshModelType::General) {
		return FreshResult::failure(FreshStatus::UnsupportedOperation, "update is only valid for general models");
	}
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}

	std::vector<FreshEvent> events;
	FreshResult result = FreshResult::success("documents updated");
	bool changedDocsCreated = false;
	{
		struct FreshUpdateCandidate {
			std::string id;
			JsonDocument doc;
		};
		std::vector<FreshUpdateCandidate> candidates;
		FreshLock lock(*_owner->_mutex);
		if (!_owner->_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_owner->_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		for (auto &entry : _state->docs) {
			if (!predicate(entry.second)) {
				continue;
			}
			JsonDocument candidate;
			FreshCopyJson(candidate, entry.second);
			FreshMergePatch(candidate, patch);
			candidate["updatedAt"] = _owner->now();
			FreshResult sizeResult =
			    _owner->checkPayloadSize(measureMsgPack(candidate), _owner->_config.maxDocumentBytes, "document");
			if (!sizeResult) {
				return sizeResult;
			}
			if (_state->validator) {
				FreshValidationResult validation = _state->validator(candidate);
				if (!validation) {
					return FreshResult::failure(
					    FreshStatus::ValidationFailed,
					    validation.message.c_str(),
					    result.affectedCount
					);
				}
			}
			candidates.push_back(FreshUpdateCandidate{.id = entry.first, .doc = std::move(candidate)});
		}

		for (const FreshUpdateCandidate &candidate : candidates) {
			auto found = _state->docs.find(candidate.id);
			if (found == _state->docs.end()) {
				continue;
			}
			FreshCopyJson(found->second, candidate.doc);
			FreshPendingRecord record;
			record.op = FreshJournalOp::Update;
			record.sequence = _owner->_nextPendingSequence++;
			record.id = candidate.id;
			FreshCopyJson(record.doc, candidate.doc);
			_state->pending.push_back(record);
			_state->dirty = true;
			result.affectedCount++;
			if (returnMode == FreshReturn::ChangedDocs) {
				JsonArray changedDocs =
				    changedDocsCreated ? result.doc.as<JsonArray>() : result.doc.to<JsonArray>();
				changedDocsCreated = true;
				changedDocs.add(candidate.doc.as<JsonVariantConst>());
			}
			events.push_back({
			    .type = FreshEventType::DocumentUpdated,
			    .modelName = _state->name,
			    .documentId = candidate.id,
			    .affectedCount = 1,
			    .result = FreshResult::success("document updated", 1)
			});
		}
		if (result.affectedCount > 0 && returnMode == FreshReturn::AllDocs) {
			JsonArray array = result.doc.to<JsonArray>();
			for (const auto &entry : _state->docs) {
				array.add(entry.second.as<JsonVariantConst>());
			}
		} else if (result.affectedCount == 0) {
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
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (_state->type != FreshModelType::General) {
		return FreshResult::failure(FreshStatus::UnsupportedOperation, "delete is only valid for general models");
	}
	if (!predicate) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "predicate is required");
	}

	std::vector<FreshEvent> events;
	FreshResult result = FreshResult::success("documents deleted");
	{
		FreshLock lock(*_owner->_mutex);
		if (!_owner->_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_owner->_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		for (auto it = _state->docs.begin(); it != _state->docs.end();) {
			if (!predicate(it->second)) {
				++it;
				continue;
			}
			const std::string id = it->first;
			it = _state->docs.erase(it);

			FreshPendingRecord record;
			record.op = FreshJournalOp::Delete;
			record.sequence = _owner->_nextPendingSequence++;
			record.id = id;
			_state->pending.push_back(record);
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
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (_state->type != FreshModelType::Stream) {
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "retrieve is only valid for stream models"
		);
	}

	FreshLock lock(*_owner->_mutex);
	if (!_owner->_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	FreshResult result = FreshResult::success("stream entries found");
	JsonArray array = result.doc.to<JsonArray>();
	size_t skipped = 0;

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
		array.add(entry.as<JsonVariantConst>());
		result.affectedCount++;
		if (options.limit > 0 && result.affectedCount >= options.limit) {
			break;
		}
	}
	if (result.affectedCount == 0) {
		result.message = "no stream entries found";
	}
	return result;
}

FreshResult FreshModel::streamTo(Print &out) const {
	if (!_owner || !_state || _state->dropped) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (_state->type != FreshModelType::Stream) {
		return FreshResult::failure(FreshStatus::UnsupportedOperation, "streamTo is only valid for stream models");
	}
	FreshLock lock(*_owner->_mutex);
	if (!_owner->_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	size_t count = 0;
	for (const JsonDocument &entry : _state->streamEntries) {
		count += serializeMsgPack(entry, out);
	}
	return FreshResult::success("stream exported", count);
}

FreshModelResult Fresh::createModel(const char *modelName) {
	return createModel(modelName, _config.defaultModelType);
}

FreshModel Fresh::model(const char *modelName) {
	if (!FreshIsValidName(modelName)) {
		return FreshModel();
	}

	FreshLock lock(*_mutex);
	auto found = _models.find(modelName);
	if (!_initialized || _stopping || found == _models.end() || found->second->dropped) {
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
		state = std::make_shared<FreshModel::State>();
		state->name = modelName;
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
		auto found = _models.find(modelName);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		if (found == _models.end()) {
			return FreshResult::failure(FreshStatus::ModelNotFound, "model not found");
		}
		found->second->dropped = true;
		found->second->dirty = true;
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
		for (const auto &entry : _models) {
			names.push_back(entry.first);
		}
	}
	size_t affected = 0;
	for (const std::string &name : names) {
		FreshResult result = dropModel(name.c_str());
		if (result) {
			affected++;
		}
	}
	return FreshResult::success("all models dropped", affected);
}

FreshResult Fresh::renameModel(const char *oldName, const char *newName) {
	if (!FreshIsValidName(oldName) || !FreshIsValidName(newName)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "invalid model name");
	}

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_mutex);
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
		if (_models.find(newName) != _models.end()) {
			return FreshResult::failure(FreshStatus::ModelExists, "target model already exists");
		}
		auto state = found->second;
		_models.erase(found);
		state->previousName = oldName;
		state->name = newName;
		state->dirty = true;
		state->storageEpoch++;
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
