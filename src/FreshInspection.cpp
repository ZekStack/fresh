#include "Fresh.h"
#include "internal/FreshInternal.h"

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
	if (_stopping) {
		result.status = FreshStatus::Busy;
		result.message = "database is stopping";
		return result;
	}

	result.models.reserve(_models.size());
	for (const auto &entry : _models) {
		const std::shared_ptr<FreshModel::State> &state = entry.second;
		if (state == nullptr || state->dropped) {
			continue;
		}

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

	FreshLock lock(*_owner->_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	FreshResult valid = validateLocked();
	if (!valid) {
		return valid;
	}

	FreshResult result = FreshResult::success("records listed");
	JsonArray records = result.doc.to<JsonArray>();
	size_t skipped = 0;

	auto appendRecord = [&](const JsonDocument &record) {
		if (skipped < options.offset) {
			skipped++;
			return true;
		}
		records.add(record.as<JsonVariantConst>());
		result.affectedCount++;
		return options.limit == 0 || result.affectedCount < options.limit;
	};

	if (_state->type == FreshModelType::Stream) {
		if (options.reverse) {
			for (auto it = _state->streamEntries.rbegin(); it != _state->streamEntries.rend(); ++it) {
				if (!appendRecord(*it)) break;
			}
		} else {
			for (const JsonDocument &record : _state->streamEntries) {
				if (!appendRecord(record)) break;
			}
		}
	} else if (options.reverse) {
		for (auto it = _state->docs.rbegin(); it != _state->docs.rend(); ++it) {
			if (!appendRecord(it->second)) break;
		}
	} else {
		for (const auto &entry : _state->docs) {
			if (!appendRecord(entry.second)) break;
		}
	}

	if (result.affectedCount == 0) {
		result.message = "no records found";
	}
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
	if (!replacement.is<JsonObject>()) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "replacement must be an object");
	}

	const uint64_t updateTime = _owner->now();
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
		    "replaceById is only valid for general models"
		);
		if (!valid) {
			return valid;
		}

		auto found = _state->docs.find(id);
		if (found == _state->docs.end()) {
			return FreshResult::failure(FreshStatus::DocumentNotFound, "document not found");
		}

		JsonDocument candidate;
		FreshResult cloneResult = FreshCloneJson(
		    candidate,
		    replacement.as<JsonVariantConst>(),
		    "replacement document"
		);
		if (!cloneResult) {
			return cloneResult;
		}

		candidate["_id"].set(found->second["_id"]);
		candidate["createdAt"].set(found->second["createdAt"]);
		candidate["updatedAt"] = updateTime;

		FreshResult sizeResult = _owner->checkPayloadSize(
		    measureMsgPack(candidate),
		    _owner->_config.maxDocumentBytes,
		    "document"
		);
		if (!sizeResult) {
			return sizeResult;
		}
		if (_state->validator) {
			FreshValidationResult validation = _state->validator(candidate);
			if (!validation) {
				return FreshResult::failure(
				    FreshStatus::ValidationFailed,
				    validation.message.c_str()
				);
			}
		}

		FreshPendingRecord record;
		record.op = FreshJournalOp::Update;
		record.sequence = _owner->_nextPendingSequence;
		record.id = id;
		cloneResult = FreshCloneJson(
		    record.doc,
		    candidate.as<JsonVariantConst>(),
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

		JsonDocument resultDoc;
		cloneResult = FreshCloneJson(
		    resultDoc,
		    candidate.as<JsonVariantConst>(),
		    "result document"
		);
		if (!cloneResult) {
			return cloneResult;
		}

		_owner->_nextPendingSequence++;
		_state->lastSequence = record.sequence;
		found->second = std::move(candidate);
		_state->pending.push_back(std::move(record));
		_state->dirty = true;

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
