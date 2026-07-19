#include "Fresh.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <LittleFS.h>

#include <limits>
#include <utility>

namespace {

struct FreshDocumentSnapshot {
	std::string id;
	JsonDocument doc;
};

FreshResult FreshBuildJournalRecord(
    Fresh &owner,
    FreshPendingRecord &record,
    size_t maxJournalRecordBytes
) {
	JsonDocument recordDoc(&FreshJsonAllocator());
	FreshResult result = owner.recordToJson(record, recordDoc);
	if (!result) {
		return result;
	}
	return owner.checkPayloadSize(
	    measureMsgPack(recordDoc),
	    maxJournalRecordBytes,
	    "journal record"
	);
}

} // namespace

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
	if (!_owner->_initialized || _owner->_lifecycle == Fresh::Lifecycle::Uninitialized ||
	    _owner->_lifecycle == Fresh::Lifecycle::Stopped) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_owner->_stopping || _owner->_lifecycle != Fresh::Lifecycle::Running) {
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
	FreshResultValidator wrapped = [validator](const JsonDocument &doc) {
		if (!validator || validator(doc)) {
			return FreshValidationResult{.result = true, .message = "ok"};
		}
		return FreshValidationResult{.result = false, .message = "validation failed"};
	};
	return setValidator(std::move(wrapped));
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
	uint64_t nextRevision = 0;
	FreshResult revisionResult = FreshNextRevision(_state->revision, nextRevision, "model revision");
	if (!revisionResult) {
		return revisionResult;
	}
	_state->validator = std::move(validator);
	_state->revision = nextRevision;
	return FreshResult::success("validator set");
}

FreshResult FreshModel::create(JsonDocument &doc) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (!doc.is<JsonObjectConst>()) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "document must be an object");
	}

	const uint64_t time = _owner->now();
	std::string id;
	uint64_t capturedRevision = 0;
	uint64_t capturedSequence = 0;
	FreshResultValidator validator;
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
		if (!valid) return valid;

		id = doc["_id"] | "";
		if (id.empty() || _state->docs.find(id) != _state->docs.end()) {
			do {
				id = FreshMakeId();
			} while (_state->docs.find(id) != _state->docs.end());
		}
		if (_owner->_nextPendingSequence == UINT64_MAX) {
			return FreshResult::failure(FreshStatus::InternalError, "pending sequence overflow");
		}
		capturedRevision = _state->revision;
		capturedSequence = _owner->_nextPendingSequence;
		validator = _state->validator;
	}

	JsonDocument candidate(&FreshJsonAllocator());
	FreshResult result = FreshCloneJson(candidate, doc.as<JsonVariantConst>(), "document");
	if (!result) return result;
	result = FreshJsonSet(candidate["_id"], id, candidate, "document id");
	if (!result) return result;
	result = FreshJsonSet(candidate["createdAt"], time, candidate, "document createdAt");
	if (!result) return result;
	result = FreshJsonSet(candidate["updatedAt"], time, candidate, "document updatedAt");
	if (!result) return result;
	result = _owner->checkPayloadSize(
	    measureMsgPack(candidate),
	    _owner->_config.maxDocumentBytes,
	    "document"
	);
	if (!result) return result;
	if (validator) {
		FreshValidationResult validation = validator(candidate);
		if (!validation) {
			return FreshResult::failure(FreshStatus::ValidationFailed, validation.message.c_str());
		}
	}

	JsonDocument stored;
	result = FreshCloneJson(stored, candidate.as<JsonVariantConst>(), "stored document");
	if (!result) return result;
	JsonDocument resultDoc;
	result = FreshCloneJson(resultDoc, candidate.as<JsonVariantConst>(), "result document");
	if (!result) return result;
	JsonDocument callerDoc;
	result = FreshCloneJson(callerDoc, candidate.as<JsonVariantConst>(), "caller document");
	if (!result) return result;

	FreshPendingRecord record;
	record.op = FreshJournalOp::Create;
	record.sequence = capturedSequence;
	record.id = id;
	result = FreshCloneJson(record.doc, candidate.as<JsonVariantConst>(), "journal document");
	if (!result) return result;
	result = FreshBuildJournalRecord(*_owner, record, _owner->_config.maxJournalRecordBytes);
	if (!result) return result;

	FreshEvent event;
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
		if (!valid) return valid;
		if (_state->revision != capturedRevision ||
		    _owner->_nextPendingSequence != capturedSequence ||
		    _state->docs.find(id) != _state->docs.end()) {
			return FreshResult::failure(FreshStatus::Busy, "model changed while validator was running");
		}
		uint64_t nextRevision = 0;
		result = FreshNextRevision(_state->revision, nextRevision, "model revision");
		if (!result) return result;

		_owner->_nextPendingSequence++;
		_state->lastSequence = record.sequence;
		_state->docs[id] = std::move(stored);
		_state->pending.push_back(std::move(record));
		_state->dirty = true;
		_state->revision = nextRevision;
		doc = std::move(callerDoc);

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

	uint64_t capturedRevision = 0;
	uint64_t capturedSequence = 0;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::Stream,
		    "append is only valid for stream models"
		);
		if (!valid) return valid;
		if (_owner->_nextPendingSequence == UINT64_MAX) {
			return FreshResult::failure(FreshStatus::InternalError, "pending sequence overflow");
		}
		capturedRevision = _state->revision;
		capturedSequence = _owner->_nextPendingSequence;
	}

	JsonDocument stored;
	FreshResult result = FreshCloneJson(stored, doc.as<JsonVariantConst>(), "stream entry");
	if (!result) return result;
	result = _owner->checkPayloadSize(
	    measureMsgPack(stored),
	    _owner->_config.maxDocumentBytes,
	    "stream entry"
	);
	if (!result) return result;
	JsonDocument callerDoc;
	result = FreshCloneJson(callerDoc, stored.as<JsonVariantConst>(), "caller stream entry");
	if (!result) return result;

	FreshPendingRecord record;
	record.op = FreshJournalOp::Append;
	record.sequence = capturedSequence;
	record.maxEntries = options.maxEntries;
	result = FreshCloneJson(record.doc, stored.as<JsonVariantConst>(), "journal stream entry");
	if (!result) return result;
	result = FreshBuildJournalRecord(*_owner, record, _owner->_config.maxJournalRecordBytes);
	if (!result) return result;

	FreshEvent event;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::Stream,
		    "append is only valid for stream models"
		);
		if (!valid) return valid;
		if (_state->revision != capturedRevision || _owner->_nextPendingSequence != capturedSequence) {
			return FreshResult::failure(FreshStatus::Busy, "model changed while append was being prepared");
		}
		uint64_t nextRevision = 0;
		result = FreshNextRevision(_state->revision, nextRevision, "model revision");
		if (!result) return result;

		_owner->_nextPendingSequence++;
		_state->streamEntries.push_back(std::move(stored));
		while (options.maxEntries > 0 && _state->streamEntries.size() > options.maxEntries) {
			_state->streamEntries.pop_front();
		}
		_state->lastSequence = record.sequence;
		_state->pending.push_back(std::move(record));
		_state->dirty = true;
		_state->revision = nextRevision;
		doc = std::move(callerDoc);

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
	if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	FreshResult valid = validateLocked(
	    true,
	    FreshModelType::General,
	    "findById is only valid for general models"
	);
	if (!valid) return valid;
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

	std::vector<FreshDocumentSnapshot> snapshot;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "find is only valid for general models"
		);
		if (!valid) return valid;
		snapshot.reserve(_state->docs.size());
		for (const auto &entry : _state->docs) {
			FreshDocumentSnapshot item;
			item.id = entry.first;
			FreshResult cloneResult = FreshCloneJson(
			    item.doc,
			    entry.second.as<JsonVariantConst>(),
			    "predicate snapshot"
			);
			if (!cloneResult) return cloneResult;
			snapshot.push_back(std::move(item));
		}
	}

	JsonDocument resultDoc(&FreshJsonAllocator());
	JsonArray array = resultDoc.to<JsonArray>();
	if (array.isNull() || resultDoc.overflowed()) {
		return FreshJsonAllocationFailure("find result");
	}
	size_t affectedCount = 0;
	for (const FreshDocumentSnapshot &entry : snapshot) {
		if (!predicate(entry.doc)) continue;
		FreshResult addResult = FreshJsonAdd(
		    array,
		    entry.doc.as<JsonVariantConst>(),
		    resultDoc,
		    "find result"
		);
		if (!addResult) return addResult;
		affectedCount++;
		if (stopAtFirst) break;
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
		    if (updated || !predicate(doc)) return false;
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
	if (!patch.is<JsonObjectConst>()) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "patch must be an object");
	}

	const uint64_t updateTime = _owner->now();
	uint64_t capturedRevision = 0;
	uint64_t capturedSequence = 0;
	FreshResultValidator validator;
	std::vector<FreshDocumentSnapshot> snapshot;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "update is only valid for general models"
		);
		if (!valid) return valid;
		capturedRevision = _state->revision;
		capturedSequence = _owner->_nextPendingSequence;
		validator = _state->validator;
		snapshot.reserve(_state->docs.size());
		for (const auto &entry : _state->docs) {
			FreshDocumentSnapshot item;
			item.id = entry.first;
			FreshResult cloneResult = FreshCloneJson(
			    item.doc,
			    entry.second.as<JsonVariantConst>(),
			    "update snapshot"
			);
			if (!cloneResult) return cloneResult;
			snapshot.push_back(std::move(item));
		}
	}

	struct FreshUpdateCandidate {
		std::string id;
		JsonDocument doc;
		FreshPendingRecord record;
	};
	std::vector<FreshUpdateCandidate> candidates;
	for (const FreshDocumentSnapshot &entry : snapshot) {
		if (!predicate(entry.doc)) continue;
		if (capturedSequence == UINT64_MAX ||
		    candidates.size() > UINT64_MAX - capturedSequence) {
			return FreshResult::failure(FreshStatus::InternalError, "pending sequence overflow");
		}

		FreshUpdateCandidate candidate;
		candidate.id = entry.id;
		FreshResult result = FreshCloneJson(
		    candidate.doc,
		    entry.doc.as<JsonVariantConst>(),
		    "existing document"
		);
		if (!result) return result;
		result = FreshMergePatch(candidate.doc, patch);
		if (!result) return result;
		result = FreshJsonSet(candidate.doc["updatedAt"], updateTime, candidate.doc, "updatedAt");
		if (!result) return result;
		result = _owner->checkPayloadSize(
		    measureMsgPack(candidate.doc),
		    _owner->_config.maxDocumentBytes,
		    "document"
		);
		if (!result) return result;
		if (validator) {
			FreshValidationResult validation = validator(candidate.doc);
			if (!validation) {
				return FreshResult::failure(
				    FreshStatus::ValidationFailed,
				    validation.message.c_str()
				);
			}
		}
		candidate.record.op = FreshJournalOp::Update;
		candidate.record.sequence = capturedSequence + candidates.size();
		candidate.record.id = entry.id;
		result = FreshCloneJson(
		    candidate.record.doc,
		    candidate.doc.as<JsonVariantConst>(),
		    "journal document"
		);
		if (!result) return result;
		result = FreshBuildJournalRecord(
		    *_owner,
		    candidate.record,
		    _owner->_config.maxJournalRecordBytes
		);
		if (!result) return result;
		candidates.push_back(std::move(candidate));
	}

	JsonDocument resultDoc(&FreshJsonAllocator());
	if (!candidates.empty() && returnMode != FreshReturn::None) {
		JsonArray array = resultDoc.to<JsonArray>();
		if (array.isNull() || resultDoc.overflowed()) {
			return FreshJsonAllocationFailure("update result");
		}
		if (returnMode == FreshReturn::ChangedDocs) {
			for (const FreshUpdateCandidate &candidate : candidates) {
				FreshResult addResult = FreshJsonAdd(
				    array,
				    candidate.doc.as<JsonVariantConst>(),
				    resultDoc,
				    "changed documents result"
				);
				if (!addResult) return addResult;
			}
		} else {
			for (const FreshDocumentSnapshot &entry : snapshot) {
				const JsonDocument *selected = &entry.doc;
				for (const FreshUpdateCandidate &candidate : candidates) {
					if (candidate.id == entry.id) {
						selected = &candidate.doc;
						break;
					}
				}
				FreshResult addResult = FreshJsonAdd(
				    array,
				    selected->as<JsonVariantConst>(),
				    resultDoc,
				    "all documents result"
				);
				if (!addResult) return addResult;
			}
		}
		resultDoc.shrinkToFit();
	}

	std::vector<FreshEvent> events;
	FreshResult result = FreshResult::success(
	    candidates.empty() ? "no documents updated" : "documents updated",
	    candidates.size()
	);
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "update is only valid for general models"
		);
		if (!valid) return valid;
		if (_state->revision != capturedRevision || _owner->_nextPendingSequence != capturedSequence) {
			return FreshResult::failure(FreshStatus::Busy, "model changed while predicate was running");
		}
		uint64_t nextRevision = _state->revision;
		if (!candidates.empty()) {
			FreshResult revisionResult = FreshNextRevision(_state->revision, nextRevision, "model revision");
			if (!revisionResult) return revisionResult;
		}

		_owner->_nextPendingSequence = capturedSequence + candidates.size();
		for (FreshUpdateCandidate &candidate : candidates) {
			auto found = _state->docs.find(candidate.id);
			if (found == _state->docs.end()) {
				return FreshResult::failure(FreshStatus::Busy, "model changed while update was committing");
			}
			_state->lastSequence = candidate.record.sequence;
			found->second = std::move(candidate.doc);
			_state->pending.push_back(std::move(candidate.record));
			_state->dirty = true;
			events.push_back({
			    .type = FreshEventType::DocumentUpdated,
			    .modelName = _state->name,
			    .documentId = candidate.id,
			    .affectedCount = 1,
			    .result = FreshResult::success("document updated", 1)
			});
		}
		_state->revision = nextRevision;
		if (!candidates.empty() && returnMode != FreshReturn::None) {
			result.doc = std::move(resultDoc);
		}
	}
	for (const FreshEvent &event : events) _owner->emitEvent(event);
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
		    if (deleted || !predicate(doc)) return false;
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

	uint64_t capturedRevision = 0;
	uint64_t capturedSequence = 0;
	std::vector<FreshDocumentSnapshot> snapshot;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "delete is only valid for general models"
		);
		if (!valid) return valid;
		capturedRevision = _state->revision;
		capturedSequence = _owner->_nextPendingSequence;
		snapshot.reserve(_state->docs.size());
		for (const auto &entry : _state->docs) {
			FreshDocumentSnapshot item;
			item.id = entry.first;
			FreshResult cloneResult = FreshCloneJson(
			    item.doc,
			    entry.second.as<JsonVariantConst>(),
			    "delete snapshot"
			);
			if (!cloneResult) return cloneResult;
			snapshot.push_back(std::move(item));
		}
	}

	std::vector<FreshPendingRecord> records;
	for (const FreshDocumentSnapshot &entry : snapshot) {
		if (!predicate(entry.doc)) continue;
		if (capturedSequence == UINT64_MAX || records.size() > UINT64_MAX - capturedSequence) {
			return FreshResult::failure(FreshStatus::InternalError, "pending sequence overflow");
		}
		FreshPendingRecord record;
		record.op = FreshJournalOp::Delete;
		record.sequence = capturedSequence + records.size();
		record.id = entry.id;
		FreshResult recordResult = FreshBuildJournalRecord(
		    *_owner,
		    record,
		    _owner->_config.maxJournalRecordBytes
		);
		if (!recordResult) return recordResult;
		records.push_back(std::move(record));
	}

	std::vector<FreshEvent> events;
	FreshResult result = FreshResult::success(
	    records.empty() ? "no documents deleted" : "documents deleted",
	    records.size()
	);
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "delete is only valid for general models"
		);
		if (!valid) return valid;
		if (_state->revision != capturedRevision || _owner->_nextPendingSequence != capturedSequence) {
			return FreshResult::failure(FreshStatus::Busy, "model changed while predicate was running");
		}
		uint64_t nextRevision = _state->revision;
		if (!records.empty()) {
			FreshResult revisionResult = FreshNextRevision(_state->revision, nextRevision, "model revision");
			if (!revisionResult) return revisionResult;
		}

		_owner->_nextPendingSequence = capturedSequence + records.size();
		for (FreshPendingRecord &record : records) {
			if (_state->docs.erase(record.id) != 1) {
				return FreshResult::failure(FreshStatus::Busy, "model changed while delete was committing");
			}
			_state->lastSequence = record.sequence;
			const std::string id = record.id;
			_state->pending.push_back(std::move(record));
			_state->dirty = true;
			events.push_back({
			    .type = FreshEventType::DocumentDeleted,
			    .modelName = _state->name,
			    .documentId = id,
			    .affectedCount = 1,
			    .result = FreshResult::success("document deleted", 1)
			});
		}
		_state->revision = nextRevision;
	}
	for (const FreshEvent &event : events) _owner->emitEvent(event);
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

	std::vector<JsonDocument> snapshot;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::Stream,
		    "retrieve is only valid for stream models"
		);
		if (!valid) return valid;
		snapshot.reserve(_state->streamEntries.size());
		for (const JsonDocument &entry : _state->streamEntries) {
			JsonDocument copy;
			FreshResult cloneResult = FreshCloneJson(copy, entry.as<JsonVariantConst>(), "stream snapshot");
			if (!cloneResult) return cloneResult;
			snapshot.push_back(std::move(copy));
		}
	}

	JsonDocument resultDoc(&FreshJsonAllocator());
	JsonArray array = resultDoc.to<JsonArray>();
	if (array.isNull() || resultDoc.overflowed()) {
		return FreshJsonAllocationFailure("stream result");
	}
	size_t skipped = 0;
	size_t affectedCount = 0;
	const size_t total = snapshot.size();
	for (size_t i = 0; i < total; ++i) {
		const size_t index = options.reverse ? total - 1 - i : i;
		const JsonDocument &entry = snapshot[index];
		if (predicate && !predicate(entry)) continue;
		if (skipped < options.offset) {
			skipped++;
			continue;
		}
		FreshResult addResult = FreshJsonAdd(
		    array,
		    entry.as<JsonVariantConst>(),
		    resultDoc,
		    "stream result"
		);
		if (!addResult) return addResult;
		affectedCount++;
		if (options.limit > 0 && affectedCount >= options.limit) break;
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
	std::vector<JsonDocument> snapshot;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::Stream,
		    "streamTo is only valid for stream models"
		);
		if (!valid) return valid;
		snapshot.reserve(_state->streamEntries.size());
		for (const JsonDocument &entry : _state->streamEntries) {
			JsonDocument copy;
			FreshResult cloneResult = FreshCloneJson(copy, entry.as<JsonVariantConst>(), "stream export snapshot");
			if (!cloneResult) return cloneResult;
			snapshot.push_back(std::move(copy));
		}
	}

	size_t bytes = 0;
	for (const JsonDocument &entry : snapshot) {
		const size_t expected = measureMsgPack(entry);
		const size_t written = serializeMsgPack(entry, out);
		if (written != expected) {
			return FreshResult::failure(FreshStatus::FileSystemError, "stream export was truncated", bytes);
		}
		bytes += written;
	}
	return FreshResult::success("stream exported", bytes);
}

FreshModelResult Fresh::createModel(const char *modelName) {
	FreshModelType type = FreshModelType::General;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return {.result = false, .status = FreshStatus::InternalError, .message = "failed to lock database"};
		}
		if (!_initialized) {
			return {.result = false, .status = FreshStatus::NotInitialized, .message = "database not initialized"};
		}
		type = _config.defaultModelType;
	}
	return createModel(modelName, type);
}

FreshModel Fresh::model(const char *modelName) {
	if (!FreshIsValidName(modelName)) return FreshModel();
	FreshLock lock(*_mutex);
	if (!lock || !_initialized || _stopping || _lifecycle != Lifecycle::Running) return FreshModel();
	auto found = _models.find(modelName);
	if (found == _models.end() || found->second->dropped) return FreshModel();
	return FreshModel(this, found->second);
}

FreshModelResult Fresh::createModel(const char *modelName, FreshModelType type) {
	if (!FreshIsValidName(modelName)) {
		return {.result = false, .status = FreshStatus::InvalidArgument, .message = "invalid model name"};
	}

	FreshEvent event;
	std::shared_ptr<FreshModel::State> state;
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return {.result = false, .status = FreshStatus::InternalError, .message = "failed to lock database"};
		}
		if (!_initialized) {
			return {.result = false, .status = FreshStatus::NotInitialized, .message = "database not initialized"};
		}
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return {.result = false, .status = FreshStatus::Busy, .message = "database is stopping"};
		}
		auto existing = _models.find(modelName);
		if (existing != _models.end()) {
			if (existing->second->dropped) {
				return {.result = false, .status = FreshStatus::InvalidModel, .message = "model was dropped"};
			}
			if (existing->second->type != type) {
				return {.result = false, .status = FreshStatus::ModelExists, .message = "model already exists with different type"};
			}
			return {
			    .result = true,
			    .status = FreshStatus::Ok,
			    .message = "model opened",
			    .model = FreshModel(this, existing->second),
			    .affectedCount = 1
			};
		}
		uint64_t nextDatabaseRevision = 0;
		FreshResult revisionResult = FreshNextRevision(_databaseRevision, nextDatabaseRevision, "database revision");
		if (!revisionResult) {
			return {.result = false, .status = revisionResult.status, .message = revisionResult.message};
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
		if (!state) {
			return {.result = false, .status = FreshStatus::OutOfMemory, .message = "failed to allocate model state"};
		}
		state->name = modelName;
		state->storageId = storageId;
		state->type = type;
		state->dirty = true;
		_models[state->name] = state;
		_manifestDirty = true;
		_manifestEpoch++;
		_databaseRevision = nextDatabaseRevision;
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
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		auto found = _models.find(modelName);
		if (found == _models.end()) return FreshResult::failure(FreshStatus::ModelNotFound, "model not found");
		if (found->second->dropped) return FreshResult::failure(FreshStatus::InvalidModel, "model was dropped");

		uint64_t nextStateRevision = 0;
		uint64_t nextDatabaseRevision = 0;
		result = FreshNextRevision(found->second->revision, nextStateRevision, "model revision");
		if (!result) return result;
		result = FreshNextRevision(_databaseRevision, nextDatabaseRevision, "database revision");
		if (!result) return result;
		found->second->dropped = true;
		found->second->dirty = true;
		found->second->snapshotRequired = false;
		found->second->pending.clear();
		found->second->storageEpoch++;
		found->second->revision = nextStateRevision;
		_manifestDirty = true;
		_manifestEpoch++;
		_databaseRevision = nextDatabaseRevision;
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
		if (!result) return FreshResult::failure(result.status, result.message.c_str(), affected);
		affected++;
	}
	return FreshResult::success("models dropped", affected);
}

FreshResult Fresh::dropAllModels() {
	std::vector<std::string> names;
	{
		FreshLock lock(*_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		for (const auto &entry : _models) {
			if (!entry.second->dropped) names.push_back(entry.first);
		}
	}
	size_t affected = 0;
	for (const std::string &name : names) {
		FreshResult result = dropModel(name.c_str());
		if (!result) return FreshResult::failure(result.status, result.message.c_str(), affected);
		affected++;
	}
	return FreshResult::success("all models dropped", affected);
}

FreshResult Fresh::renameModel(const char *oldName, const char *newName) {
	if (!FreshIsValidName(oldName) || !FreshIsValidName(newName)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "invalid model name");
	}
	if (strcmp(oldName, newName) == 0) return FreshResult::success("model name unchanged");

	FreshEvent event;
	FreshResult result;
	{
		FreshLock lock(*_mutex);
		if (!lock) return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
		auto found = _models.find(oldName);
		if (found == _models.end()) return FreshResult::failure(FreshStatus::ModelNotFound, "model not found");
		if (found->second->dropped) return FreshResult::failure(FreshStatus::InvalidModel, "model was dropped");
		if (_models.find(newName) != _models.end()) {
			return FreshResult::failure(FreshStatus::ModelExists, "target model already exists");
		}
		uint64_t nextStateRevision = 0;
		uint64_t nextDatabaseRevision = 0;
		result = FreshNextRevision(found->second->revision, nextStateRevision, "model revision");
		if (!result) return result;
		result = FreshNextRevision(_databaseRevision, nextDatabaseRevision, "database revision");
		if (!result) return result;

		auto state = found->second;
		_models.erase(found);
		state->name = newName;
		state->revision = nextStateRevision;
		_models[state->name] = state;
		_manifestDirty = true;
		_manifestEpoch++;
		_databaseRevision = nextDatabaseRevision;
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
