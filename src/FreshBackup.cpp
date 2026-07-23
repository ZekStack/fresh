#include "Fresh.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

using namespace fresh_backup_v2;

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
			if (write(buffer[i]) != 1) break;
			written++;
		}
		return written;
	}

	using Print::write;

  private:
	Fresh &_db;
};

namespace {

FreshBackupError backupErrorForResult(const FreshResult &result) {
	if (result.status == FreshStatus::Cancelled) return FreshBackupError::Cancelled;
	if (result.status == FreshStatus::OutOfMemory) return FreshBackupError::OutOfMemory;
	if (result.status == FreshStatus::FileSystemError || result.status == FreshStatus::StorageFull) {
		return FreshBackupError::FileSystemError;
	}
	return FreshBackupError::SerializationFailed;
}

} // namespace

bool Fresh::backupWriteByte(uint8_t byte) {
	while (true) {
		bool wrote = false;
		bool shouldEmitProgress = false;
		FreshBackupInfo info;
		{
			FreshLock lock(_backup->mutex);
			if (_backup->cancelled || _backup->buffer.empty()) return false;
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
		if (shouldEmitProgress) callBackupProgress(info);
		if (wrote) return true;
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

void Fresh::runBackupIfRequested() {
	{
		FreshLock lock(_backup->mutex);
		if (!_backup->requested || _backup->running) return;
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

	struct ModelSnapshot {
		std::shared_ptr<FreshModel::State> state;
		std::string name;
		FreshModelType type = FreshModelType::General;
		uint64_t revision = 0;
		uint64_t recordCount = 0;
	};

	FreshResult result = FreshResult::success();
	std::vector<ModelSnapshot> models;
	uint64_t recordCount = 0;
	uint64_t totalBytes = ContainerHeaderSize;
	const uint64_t generatedAt = now();
	uint64_t capturedDatabaseRevision = 0;

	{
		FreshLock lock(*_mutex);
		if (!lock) {
			result = FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		} else if (!_initialized || _stopping || _lifecycle != Lifecycle::Running) {
			result = FreshResult::failure(FreshStatus::Busy, "database is stopping");
		} else {
			capturedDatabaseRevision = _databaseRevision;
			size_t activeModelCount = 0;
			for (const auto &entry : _models) {
				if (!entry.second->dropped) activeModelCount++;
			}
			if (activeModelCount > UINT32_MAX) {
				result = FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup has too many models");
			} else {
				models.reserve(activeModelCount);
			}

			for (const auto &entry : _models) {
				if (!result) break;
				const auto &state = entry.second;
				if (state->dropped) continue;
				if (state->name.empty() || state->name.size() > UINT16_MAX) {
					result = FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup model name is too long");
					break;
				}
				const uint64_t modelRecords = state->type == FreshModelType::Stream
				                                  ? static_cast<uint64_t>(state->streamEntries.size())
				                                  : static_cast<uint64_t>(state->docs.size());
				if (!addSize(recordCount, modelRecords) ||
				    !addSize(totalBytes, frameBytes(ModelBeginFixedPayloadSize + state->name.size())) ||
				    !addSize(totalBytes, frameBytes(ModelEndPayloadSize))) {
					result = FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup size calculation overflow");
					break;
				}

				if (state->type == FreshModelType::Stream) {
					for (const JsonDocument &doc : state->streamEntries) {
						const size_t payloadBytes = measureMsgPack(doc);
						if (payloadBytes == 0 || payloadBytes > UINT32_MAX ||
						    payloadBytes > _config.maxDocumentBytes ||
						    !addSize(totalBytes, frameBytes(payloadBytes))) {
							result = FreshResult::failure(
							    payloadBytes > _config.maxDocumentBytes ? FreshStatus::SizeLimitExceeded
							                                             : FreshStatus::InternalError,
							    "invalid backup stream entry size"
							);
							break;
						}
					}
				} else {
					for (const auto &docEntry : state->docs) {
						const size_t payloadBytes = measureMsgPack(docEntry.second);
						if (payloadBytes == 0 || payloadBytes > UINT32_MAX ||
						    payloadBytes > _config.maxDocumentBytes ||
						    !addSize(totalBytes, frameBytes(payloadBytes))) {
							result = FreshResult::failure(
							    payloadBytes > _config.maxDocumentBytes ? FreshStatus::SizeLimitExceeded
							                                             : FreshStatus::InternalError,
							    "invalid backup document size"
							);
							break;
						}
					}
				}
				if (!result) break;
				models.push_back({
				    .state = state,
				    .name = state->name,
				    .type = state->type,
				    .revision = state->revision,
				    .recordCount = modelRecords,
				});
			}
		}
	}

	if (result && !addSize(totalBytes, frameBytes(ArchiveEndPayloadSize))) {
		result = FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup size calculation overflow");
	}
	if (result && totalBytes > SIZE_MAX) {
		result = FreshResult::failure(FreshStatus::SizeLimitExceeded, "backup is too large for this platform");
	}

	if (result) {
		{
			FreshLock lock(_backup->mutex);
			_backup->total = static_cast<size_t>(totalBytes);
		}
		FreshBackupInfo startInfo;
		startInfo.estimatedSize = static_cast<size_t>(totalBytes);
		startInfo.total = static_cast<size_t>(totalBytes);
		callBackupStart(startInfo);
		emitEvent({.type = FreshEventType::BackupStarted, .result = FreshResult::success("backup started")});

		{
			FreshLock lock(*_mutex);
			if (!lock || !_initialized || _stopping || _lifecycle != Lifecycle::Running ||
			    _databaseRevision != capturedDatabaseRevision) {
				result = FreshResult::failure(FreshStatus::Busy, "database changed during backup");
			}
		}

		FreshBackupPrint output(*this);
		Writer writer(output);
		ContainerHeader header{
		    .generatedAt = generatedAt,
		    .modelCount = static_cast<uint32_t>(models.size()),
		    .recordCount = recordCount,
		    .totalBytes = totalBytes,
		};
		if (result && !writer.writeContainerHeader(header)) {
			result = isBackupCancelled()
			             ? FreshResult::failure(FreshStatus::Cancelled, "backup cancelled")
			             : FreshResult::failure(FreshStatus::InternalError, "failed to write backup header");
		}

		uint64_t writtenRecords = 0;
		for (const ModelSnapshot &snapshot : models) {
			if (!result) break;
			{
				FreshLock lock(*_mutex);
				auto current = _models.find(snapshot.name);
				if (!lock || !_initialized || _stopping || _lifecycle != Lifecycle::Running ||
				    current == _models.end() || current->second != snapshot.state ||
				    snapshot.state->dropped || snapshot.state->revision != snapshot.revision) {
					result = FreshResult::failure(FreshStatus::Busy, "database changed during backup");
				}
			}
			if (!result) break;
			if (!writer.writeModelBegin(snapshot.name, snapshot.type, snapshot.recordCount)) {
				result = isBackupCancelled()
				             ? FreshResult::failure(FreshStatus::Cancelled, "backup cancelled")
				             : FreshResult::failure(FreshStatus::InternalError, "failed to write backup model header");
				break;
			}

			std::string previousId;
			for (uint64_t index = 0; index < snapshot.recordCount; ++index) {
				JsonDocument record(&FreshJsonAllocator());
				{
					FreshLock lock(*_mutex);
					auto current = _models.find(snapshot.name);
					if (!lock || !_initialized || _stopping || _lifecycle != Lifecycle::Running ||
					    current == _models.end() || current->second != snapshot.state ||
					    snapshot.state->dropped || snapshot.state->revision != snapshot.revision) {
						result = FreshResult::failure(FreshStatus::Busy, "database changed during backup");
					} else if (snapshot.type == FreshModelType::Stream) {
						if (snapshot.state->streamEntries.size() != snapshot.recordCount ||
						    index >= snapshot.state->streamEntries.size()) {
							result = FreshResult::failure(FreshStatus::Busy, "stream changed during backup");
						} else {
							result = FreshCloneJson(
							    record,
							    snapshot.state->streamEntries[static_cast<size_t>(index)].as<JsonVariantConst>(),
							    "backup stream entry"
							);
						}
					} else {
						if (snapshot.state->docs.size() != snapshot.recordCount) {
							result = FreshResult::failure(FreshStatus::Busy, "model changed during backup");
						} else {
							auto doc = index == 0 ? snapshot.state->docs.begin()
							                      : snapshot.state->docs.upper_bound(previousId);
							if (doc == snapshot.state->docs.end()) {
								result = FreshResult::failure(FreshStatus::Busy, "model changed during backup");
							} else {
								previousId = doc->first;
								result = FreshCloneJson(
								    record,
								    doc->second.as<JsonVariantConst>(),
								    "backup document"
								);
							}
						}
					}
				}
				if (!result) break;
				const size_t payloadSize = measureMsgPack(record);
				if (payloadSize == 0 || payloadSize > _config.maxDocumentBytes ||
				    !writer.writeRecord(record, payloadSize)) {
					result = isBackupCancelled()
					             ? FreshResult::failure(FreshStatus::Cancelled, "backup cancelled")
					             : FreshResult::failure(FreshStatus::InternalError, "failed to write backup record");
					break;
				}
				writtenRecords++;
			}
			if (!result) break;
			if (!writer.writeModelEnd(snapshot.recordCount)) {
				result = isBackupCancelled()
				             ? FreshResult::failure(FreshStatus::Cancelled, "backup cancelled")
				             : FreshResult::failure(FreshStatus::InternalError, "failed to write backup model trailer");
				break;
			}
		}

		if (result) {
			FreshLock lock(*_mutex);
			if (!lock || !_initialized || _stopping || _lifecycle != Lifecycle::Running ||
			    _databaseRevision != capturedDatabaseRevision) {
				result = FreshResult::failure(FreshStatus::Busy, "database changed during backup");
			} else {
				for (const ModelSnapshot &snapshot : models) {
					auto current = _models.find(snapshot.name);
					const uint64_t currentCount = snapshot.type == FreshModelType::Stream
					                                  ? static_cast<uint64_t>(snapshot.state->streamEntries.size())
					                                  : static_cast<uint64_t>(snapshot.state->docs.size());
					if (current == _models.end() || current->second != snapshot.state ||
					    snapshot.state->dropped || snapshot.state->revision != snapshot.revision ||
					    currentCount != snapshot.recordCount) {
						result = FreshResult::failure(FreshStatus::Busy, "database changed during backup");
						break;
					}
				}
			}
		}

		if (result && writtenRecords != recordCount) {
			result = FreshResult::failure(FreshStatus::InternalError, "backup record count changed");
		}
		if (result && !writer.writeArchiveEnd(static_cast<uint32_t>(models.size()), recordCount)) {
			result = isBackupCancelled()
			             ? FreshResult::failure(FreshStatus::Cancelled, "backup cancelled")
			             : FreshResult::failure(FreshStatus::InternalError, "failed to finish backup archive");
		}
		if (result && writer.bytesWritten() != totalBytes) {
			result = FreshResult::failure(FreshStatus::InternalError, "backup byte count mismatch");
		}
		if (result) result = FreshResult::success("backup finished", static_cast<size_t>(writer.bytesWritten()));
	}

	const size_t progress = [&]() {
		FreshLock lock(_backup->mutex);
		return _backup->progress;
	}();
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
	endInfo.progress = progress;
	endInfo.total = result ? progress : static_cast<size_t>(totalBytes <= SIZE_MAX ? totalBytes : 0);
	endInfo.size = progress;
	endInfo.result = result;
	if (result) {
		callBackupEnd(endInfo);
		emitEvent({.type = FreshEventType::BackupFinished, .result = result});
	} else if (result.status == FreshStatus::Cancelled) {
		endInfo.error = FreshBackupError::Cancelled;
		callBackupError(endInfo);
		emitEvent({.type = FreshEventType::BackupCancelled, .result = result});
	} else {
		endInfo.error = backupErrorForResult(result);
		callBackupError(endInfo);
		emitEvent({.type = FreshEventType::BackupError, .result = result});
	}
}

bool Fresh::isBackupCancelled() {
	FreshLock lock(_backup->mutex);
	return _backup->cancelled;
}

size_t Fresh::estimateBackupSize() {
	FreshLock lock(*_mutex);
	if (!lock || !_initialized || _stopping || _lifecycle != Lifecycle::Running) return 0;
	uint64_t total = ContainerHeaderSize;
	for (const auto &entry : _models) {
		const auto &state = entry.second;
		if (state->dropped || state->name.size() > UINT16_MAX) continue;
		if (!addSize(total, frameBytes(ModelBeginFixedPayloadSize + state->name.size())) ||
		    !addSize(total, frameBytes(ModelEndPayloadSize))) return 0;
		if (state->type == FreshModelType::Stream) {
			for (const JsonDocument &doc : state->streamEntries) {
				if (!addSize(total, frameBytes(measureMsgPack(doc)))) return 0;
			}
		} else {
			for (const auto &docEntry : state->docs) {
				if (!addSize(total, frameBytes(measureMsgPack(docEntry.second)))) return 0;
			}
		}
	}
	if (!addSize(total, frameBytes(ArchiveEndPayloadSize)) || total > SIZE_MAX) return 0;
	return static_cast<size_t>(total);
}

void Fresh::callBackupStart(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupStart;
	}
	if (callback) callback(info);
}

void Fresh::callBackupProgress(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupProgress;
	}
	if (callback) callback(info);
}

void Fresh::callBackupEnd(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupEnd;
	}
	if (callback) callback(info);
}

void Fresh::callBackupError(FreshBackupInfo info) {
	FreshBackupCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onBackupError;
	}
	if (callback) callback(info);
}

FreshResult Fresh::startBackup() {
	FreshLock dbLock(*_mutex);
	if (!_initialized) return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	if (_stopping || _lifecycle != Lifecycle::Running) {
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
	if (_syncTaskHandle != nullptr) xTaskNotifyGive(_syncTaskHandle);
	return FreshResult::success("backup queued");
}

size_t Fresh::readBackup(uint8_t *buffer, size_t length, uint32_t timeoutMS) {
	if (buffer == nullptr || length == 0) return 0;

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
			if (read > 0 || _backup->done || (!_backup->running && !_backup->requested)) return read;
		}
		if (timeoutMS == 0 || millis() - start >= timeoutMS) return read;
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
