#include "Fresh.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <utility>

FreshModelListResult Fresh::listModels() const {
	FreshModelListResult result;
	FreshLock lock(*_mutex);
	if (!lock) {
		result.status = FreshStatus::InternalError;
		result.message = "failed to lock database";
		return result;
	}
	if (!_initialized) {
		result.status = FreshStatus::NotInitialized;
		result.message = "database not initialized";
		return result;
	}
	if (_stopping || _lifecycle != Lifecycle::Running) {
		result.status = FreshStatus::Busy;
		result.message = "database is stopping";
		return result;
	}

	result.models.reserve(_models.size());
	for (const auto &entry : _models) {
		const std::shared_ptr<FreshModel::State> &state = entry.second;
		if (state == nullptr || state->dropped) continue;

		FreshModelInfo info;
		info.name = state->name;
		info.type = state->type;
		info.recordCount = state->type == FreshModelType::Stream
		                       ? state->streamEntries.size()
		                       : state->docs.size();
		result.models.push_back(info);
	}

	result.result = true;
	result.status = FreshStatus::Ok;
	result.affectedCount = result.models.size();
	result.message = result.models.empty() ? "no models found" : "models listed";
	return result;
}

FreshResult FreshModel::listRecords(const FreshRecordRetrieveOptions &options) const {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}

	std::vector<JsonDocument> snapshot;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		FreshResult valid = validateLocked();
		if (!valid) return valid;

		const size_t count = _state->type == FreshModelType::Stream
		                         ? _state->streamEntries.size()
		                         : _state->docs.size();
		snapshot.reserve(count);
		if (_state->type == FreshModelType::Stream) {
			for (const JsonDocument &record : _state->streamEntries) {
				JsonDocument copy;
				FreshResult cloneResult = FreshCloneJson(
				    copy,
				    record.as<JsonVariantConst>(),
				    "record list snapshot"
				);
				if (!cloneResult) return cloneResult;
				snapshot.push_back(std::move(copy));
			}
		} else {
			for (const auto &entry : _state->docs) {
				JsonDocument copy;
				FreshResult cloneResult = FreshCloneJson(
				    copy,
				    entry.second.as<JsonVariantConst>(),
				    "record list snapshot"
				);
				if (!cloneResult) return cloneResult;
				snapshot.push_back(std::move(copy));
			}
		}
	}

	JsonDocument resultDoc(&FreshJsonAllocator());
	JsonArray records = resultDoc.to<JsonArray>();
	if (records.isNull() || resultDoc.overflowed()) {
		return FreshJsonAllocationFailure("record list");
	}
	size_t skipped = 0;
	size_t affectedCount = 0;
	for (size_t i = 0; i < snapshot.size(); ++i) {
		const size_t index = options.reverse ? snapshot.size() - 1 - i : i;
		if (skipped < options.offset) {
			skipped++;
			continue;
		}
		FreshResult addResult = FreshJsonAdd(
		    records,
		    snapshot[index].as<JsonVariantConst>(),
		    resultDoc,
		    "record list"
		);
		if (!addResult) return addResult;
		affectedCount++;
		if (options.limit > 0 && affectedCount >= options.limit) break;
	}

	resultDoc.shrinkToFit();
	FreshResult result = FreshResult::success(
	    affectedCount == 0 ? "no records found" : "records listed",
	    affectedCount
	);
	result.doc = std::move(resultDoc);
	return result;
}

FreshResult FreshModel::replaceById(const char *id, const JsonDocument &replacement) {
	if (id == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "id is required");
	}
	return replaceById(std::string(id), replacement);
}

FreshResult FreshModel::replaceById(const std::string &id, const JsonDocument &replacement) {
	if (_owner == nullptr || _state == nullptr) {
		return FreshResult::failure(FreshStatus::InvalidModel, "invalid model");
	}
	if (id.empty()) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "id is required");
	}
	if (!replacement.is<JsonObjectConst>()) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "replacement must be an object");
	}

	const uint64_t updateTime = _owner->now();
	uint64_t capturedRevision = 0;
	uint64_t capturedSequence = 0;
	FreshResultValidator validator;
	JsonDocument existing;
	{
		FreshLock lock(*_owner->_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		FreshResult valid = validateLocked(
		    true,
		    FreshModelType::General,
		    "replaceById is only valid for general models"
		);
		if (!valid) return valid;
		auto found = _state->docs.find(id);
		if (found == _state->docs.end()) {
			return FreshResult::failure(FreshStatus::DocumentNotFound, "document not found");
		}
		if (_owner->_nextPendingSequence == UINT64_MAX) {
			return FreshResult::failure(FreshStatus::InternalError, "pending sequence overflow");
		}
		FreshResult cloneResult = FreshCloneJson(
		    existing,
		    found->second.as<JsonVariantConst>(),
		    "replacement source"
		);
		if (!cloneResult) return cloneResult;
		capturedRevision = _state->revision;
		capturedSequence = _owner->_nextPendingSequence;
		validator = _state->validator;
	}

	JsonDocument candidate(&FreshJsonAllocator());
	FreshResult result = FreshCloneJson(
	    candidate,
	    replacement.as<JsonVariantConst>(),
	    "replacement document"
	);
	if (!result) return result;
	result = FreshJsonSet(candidate["_id"], existing["_id"], candidate, "replacement id");
	if (!result) return result;
	result = FreshJsonSet(
	    candidate["createdAt"],
	    existing["createdAt"],
	    candidate,
	    "replacement createdAt"
	);
	if (!result) return result;
	result = FreshJsonSet(candidate["updatedAt"], updateTime, candidate, "replacement updatedAt");
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

	FreshPendingRecord record;
	record.op = FreshJournalOp::Update;
	record.sequence = capturedSequence;
	record.id = id;
	result = FreshCloneJson(record.doc, candidate.as<JsonVariantConst>(), "journal document");
	if (!result) return result;
	JsonDocument recordDoc(&FreshJsonAllocator());
	result = _owner->recordToJson(record, recordDoc);
	if (!result) return result;
	result = _owner->checkPayloadSize(
	    measureMsgPack(recordDoc),
	    _owner->_config.maxJournalRecordBytes,
	    "journal record"
	);
	if (!result) return result;

	JsonDocument resultDoc;
	result = FreshCloneJson(resultDoc, candidate.as<JsonVariantConst>(), "result document");
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
		    "replaceById is only valid for general models"
		);
		if (!valid) return valid;
		if (_state->revision != capturedRevision ||
		    _owner->_nextPendingSequence != capturedSequence) {
			return FreshResult::failure(FreshStatus::Busy, "model changed while validator was running");
		}
		auto found = _state->docs.find(id);
		if (found == _state->docs.end()) {
			return FreshResult::failure(FreshStatus::Busy, "model changed while replacement was committing");
		}
		uint64_t nextRevision = 0;
		result = FreshNextRevision(_state->revision, nextRevision, "model revision");
		if (!result) return result;

		_owner->_nextPendingSequence++;
		_state->lastSequence = record.sequence;
		found->second = std::move(candidate);
		_state->pending.push_back(std::move(record));
		_state->dirty = true;
		_state->revision = nextRevision;

		result = FreshResult::success("document replaced", 1);
		result.doc = std::move(resultDoc);
		event = {
		    .type = FreshEventType::DocumentUpdated,
		    .modelName = _state->name,
		    .documentId = id,
		    .affectedCount = 1,
		    .result = FreshResult::success("document replaced", 1)
		};
	}

	_owner->emitEvent(event);
	return result;
}
