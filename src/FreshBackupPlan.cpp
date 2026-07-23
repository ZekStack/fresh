#include "Fresh.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshInternal.h"

#include <limits>
#include <set>
#include <utility>

using namespace fresh_backup_v2;

FreshResult Fresh::validateBackupOptionsLocked(const FreshBackupOptions &options) const {
	std::set<std::string> selected;
	for (const std::string &name : options.modelNames) {
		if (!FreshIsValidName(name.c_str())) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "backup contains an invalid model name");
		}
		if (!selected.insert(name).second) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "backup contains a duplicate model name");
		}
		auto current = _models.find(name);
		if (current == _models.end() || current->second->dropped) {
			return FreshResult::failure(FreshStatus::ModelNotFound, "backup model was not found");
		}
	}
	return FreshResult::success("backup options valid", selected.size());
}

FreshResult Fresh::prepareBackupPlan(
    const FreshBackupOptions &options,
    FreshBackupPlan &plan
) const {
	FreshLock lock(*_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	if (!_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_stopping || _lifecycle != Lifecycle::Running) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}

	FreshResult validation = validateBackupOptionsLocked(options);
	if (!validation) return validation;

	FreshBackupPlan prepared;
	prepared.databaseRevision = _databaseRevision;
	prepared.totalBytes = ContainerHeaderSize;

	std::set<std::string> selected(options.modelNames.begin(), options.modelNames.end());
	const bool includeAll = selected.empty();
	const size_t requestedCount = includeAll ? _models.size() : selected.size();
	if (requestedCount > UINT32_MAX) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup has too many models");
	}
	prepared.models.reserve(requestedCount);

	for (const auto &entry : _models) {
		const auto &state = entry.second;
		if (state->dropped || (!includeAll && selected.find(entry.first) == selected.end())) {
			continue;
		}
		if (state->name.empty() || state->name.size() > UINT16_MAX) {
			return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup model name is too long");
		}

		const uint64_t modelRecords = state->type == FreshModelType::Stream
		                                  ? static_cast<uint64_t>(state->streamEntries.size())
		                                  : static_cast<uint64_t>(state->docs.size());
		if (!addSize(prepared.recordCount, modelRecords) ||
		    !addSize(prepared.totalBytes, frameBytes(ModelBeginFixedPayloadSize + state->name.size())) ||
		    !addSize(prepared.totalBytes, frameBytes(ModelEndPayloadSize))) {
			return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup size calculation overflow");
		}

		if (state->type == FreshModelType::Stream) {
			for (const JsonDocument &doc : state->streamEntries) {
				const size_t payloadBytes = measureMsgPack(doc);
				if (payloadBytes == 0 || payloadBytes > UINT32_MAX) {
					return FreshResult::failure(FreshStatus::InternalError, "invalid backup stream entry size");
				}
				if (payloadBytes > _config.maxDocumentBytes) {
					return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup stream entry is too large");
				}
				if (!addSize(prepared.totalBytes, frameBytes(payloadBytes))) {
					return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup size calculation overflow");
				}
			}
		} else {
			for (const auto &docEntry : state->docs) {
				const size_t payloadBytes = measureMsgPack(docEntry.second);
				if (payloadBytes == 0 || payloadBytes > UINT32_MAX) {
					return FreshResult::failure(FreshStatus::InternalError, "invalid backup document size");
				}
				if (payloadBytes > _config.maxDocumentBytes) {
					return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup document is too large");
				}
				if (!addSize(prepared.totalBytes, frameBytes(payloadBytes))) {
					return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup size calculation overflow");
				}
			}
		}

		prepared.models.push_back({
		    .state = state,
		    .name = state->name,
		    .type = state->type,
		    .revision = state->revision,
		    .recordCount = modelRecords,
		});
	}

	if (!addSize(prepared.totalBytes, frameBytes(ArchiveEndPayloadSize))) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup size calculation overflow");
	}
	if (prepared.totalBytes > SIZE_MAX || prepared.recordCount > SIZE_MAX) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup is too large for this platform");
	}

	plan = std::move(prepared);
	return FreshResult::success("backup plan prepared", plan.models.size());
}

FreshResult Fresh::estimateBackup(
    const FreshBackupOptions &options,
    FreshBackupEstimate &estimate
) const {
	estimate = FreshBackupEstimate();
	FreshBackupPlan plan;
	FreshResult result = prepareBackupPlan(options, plan);
	if (!result) return result;

	estimate.totalBytes = static_cast<size_t>(plan.totalBytes);
	estimate.modelCount = plan.models.size();
	estimate.recordCount = static_cast<size_t>(plan.recordCount);
	return FreshResult::success("backup estimated", estimate.modelCount);
}
