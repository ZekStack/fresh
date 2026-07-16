#include "Fresh.h"
#include "internal/FreshInternal.h"

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
