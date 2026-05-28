#include "Fresh.h"

FreshModel::FreshModel(Fresh *owner, std::shared_ptr<State> state) : _owner(owner), _state(state) {
}

FreshModel::operator bool() const {
	return _owner != nullptr && _state != nullptr && !_state->dropped;
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
	FreshLock lock(_owner->_mutex);
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
	FreshLock lock(_owner->_mutex);
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
		FreshLock lock(_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
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
		FreshLock lock(_owner->_mutex);
		JsonDocument stored;
		FreshCopyJson(stored, doc);
		_state->streamEntries.push_back(stored);

		FreshPendingRecord record;
		record.op = FreshJournalOp::Append;
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
	FreshLock lock(_owner->_mutex);
	auto found = _state->docs.find(id);
	if (found == _state->docs.end()) {
		return FreshResult::failure(FreshStatus::ModelNotFound, "document not found");
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

	FreshLock lock(_owner->_mutex);
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

FreshResult FreshModel::updateById(const char *id, const JsonDocument &patch) {
	if (id == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "id is required");
	}
	return updateById(std::string(id), patch);
}

FreshResult FreshModel::updateById(const std::string &id, const JsonDocument &patch) {
	return updateOne(
	    [id](const JsonDocument &doc) {
		    const char *docId = doc["_id"] | "";
		    return id == docId;
	    },
	    patch
	);
}

FreshResult FreshModel::updateOne(FreshPredicate predicate, const JsonDocument &patch) {
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
	    patch
	);
}

FreshResult FreshModel::update(FreshPredicate predicate, const JsonDocument &patch) {
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
	{
		FreshLock lock(_owner->_mutex);
		for (auto &entry : _state->docs) {
			if (!predicate(entry.second)) {
				continue;
			}
			JsonDocument candidate;
			FreshCopyJson(candidate, entry.second);
			FreshMergePatch(candidate, patch);
			candidate["updatedAt"] = _owner->now();
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

			FreshCopyJson(entry.second, candidate);
			FreshPendingRecord record;
			record.op = FreshJournalOp::Update;
			record.id = entry.first;
			FreshCopyJson(record.doc, candidate);
			_state->pending.push_back(record);
			_state->dirty = true;
			result.affectedCount++;
			events.push_back({
			    .type = FreshEventType::DocumentUpdated,
			    .modelName = _state->name,
			    .documentId = entry.first,
			    .affectedCount = 1,
			    .result = FreshResult::success("document updated", 1)
			});
		}
		if (result.affectedCount > 0) {
			JsonArray array = result.doc.to<JsonArray>();
			for (const auto &entry : _state->docs) {
				array.add(entry.second.as<JsonVariantConst>());
			}
		} else {
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
		FreshLock lock(_owner->_mutex);
		for (auto it = _state->docs.begin(); it != _state->docs.end();) {
			if (!predicate(it->second)) {
				++it;
				continue;
			}
			const std::string id = it->first;
			it = _state->docs.erase(it);

			FreshPendingRecord record;
			record.op = FreshJournalOp::Delete;
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

	FreshLock lock(_owner->_mutex);
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
	FreshLock lock(_owner->_mutex);
	size_t count = 0;
	for (const JsonDocument &entry : _state->streamEntries) {
		count += serializeMsgPack(entry, out);
	}
	return FreshResult::success("stream exported", count);
}

FreshModel Fresh::createModel(const char *modelName) {
	return createModel(modelName, _config.defaultModelType);
}

FreshModel Fresh::model(const char *modelName) {
	if (!FreshIsValidName(modelName)) {
		return FreshModel();
	}

	FreshLock lock(_mutex);
	auto found = _models.find(modelName);
	if (!_initialized || found == _models.end() || found->second->dropped) {
		return FreshModel();
	}
	return FreshModel(this, found->second);
}

FreshModel Fresh::createModel(const char *modelName, FreshModelType type) {
	if (!FreshIsValidName(modelName)) {
		return FreshModel();
	}

	FreshEvent event;
	std::shared_ptr<FreshModel::State> state;
	{
		FreshLock lock(_mutex);
		if (!_initialized) {
			return FreshModel();
		}
		auto existing = _models.find(modelName);
		if (existing != _models.end()) {
			if (existing->second->dropped || existing->second->type != type) {
				return FreshModel();
			}
			return FreshModel(this, existing->second);
		}
		state = std::make_shared<FreshModel::State>();
		state->name = modelName;
		state->type = type;
		state->dirty = true;
		_models[state->name] = state;
		_manifestDirty = true;
		event = {
		    .type = FreshEventType::ModelCreated,
		    .modelName = state->name,
		    .result = FreshResult::success("model created")
		};
	}
	emitEvent(event);
	return FreshModel(this, state);
}

FreshResult Fresh::dropModel(const char *modelName) {
	if (!FreshIsValidName(modelName)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "invalid model name");
	}

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(_mutex);
		auto found = _models.find(modelName);
		if (found == _models.end()) {
			return FreshResult::failure(FreshStatus::ModelNotFound, "model not found");
		}
		found->second->dropped = true;
		found->second->dirty = true;
		_manifestDirty = true;
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
		FreshLock lock(_mutex);
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
		FreshLock lock(_mutex);
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
		_models[state->name] = state;
		_manifestDirty = true;
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
