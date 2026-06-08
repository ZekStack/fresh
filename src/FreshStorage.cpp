#include "Fresh.h"
#include "internal/FreshInternal.h"

#include <LittleFS.h>
#include <utility>

namespace {

struct FreshJournalSyncRecord {
	uint64_t sequence = 0;
	FreshJournalOp op = FreshJournalOp::Create;
	FreshByteVector payload;
};

struct FreshModelSyncBatch {
	std::shared_ptr<void> state;
	std::string name;
	std::string previousName;
	std::string path;
	std::string previousPath;
	std::string journalPath;
	std::string snapshotTempPath;
	std::string snapshotPath;
	std::string previousJournalPath;
	std::string previousSnapshotPath;
	FreshModelType type = FreshModelType::General;
	bool dropped = false;
	bool writeSnapshot = false;
	uint32_t storageEpoch = 0;
	std::vector<FreshJournalSyncRecord> records;
	JsonDocument snapshot;
};

struct FreshModelSyncResult {
	FreshResult result = FreshResult::success("model synced");
	size_t recordsWritten = 0;
	size_t bytesWritten = 0;
	bool snapshotWritten = false;
	bool dropped = false;
	bool renamed = false;
};

FreshResult FreshWriteJournalRecord(const FreshModelSyncBatch &batch, const FreshJournalSyncRecord &record) {
	if (!LittleFS.exists(batch.path.c_str()) && !LittleFS.mkdir(batch.path.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create directory");
	}

	File file = LittleFS.open(batch.journalPath.c_str(), "a");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open journal");
	}

	FreshWriteU32(file, FreshJournalMagic);
	FreshWriteU16(file, FreshJournalVersion);
	file.write(static_cast<uint8_t>(record.op));
	file.write(static_cast<uint8_t>(0));
	FreshWriteU32(file, static_cast<uint32_t>(record.payload.size()));
	FreshWriteU32(file, FreshChecksum(record.payload.data(), record.payload.size()));
	size_t written = file.write(record.payload.data(), record.payload.size());
	file.flush();
	file.close();

	if (written != record.payload.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write journal");
	}
	return FreshResult::success("journal record written");
}

FreshResult FreshWriteSnapshotBatch(const FreshModelSyncBatch &batch) {
	if (!LittleFS.exists(batch.path.c_str()) && !LittleFS.mkdir(batch.path.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create directory");
	}

	File file = LittleFS.open(batch.snapshotTempPath.c_str(), "w");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open snapshot");
	}
	size_t written = serializeMsgPack(batch.snapshot, file);
	file.close();
	if (written == 0) {
		LittleFS.remove(batch.snapshotTempPath.c_str());
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write snapshot");
	}
	LittleFS.remove(batch.snapshotPath.c_str());
	if (!LittleFS.rename(batch.snapshotTempPath.c_str(), batch.snapshotPath.c_str())) {
		LittleFS.remove(batch.snapshotTempPath.c_str());
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to replace snapshot");
	}
	LittleFS.remove(batch.journalPath.c_str());
	return FreshResult::success("snapshot written");
}

FreshModelSyncResult FreshWriteModelBatch(const FreshModelSyncBatch &batch) {
	FreshModelSyncResult result;
	result.result = FreshResult::success("model synced");

	if (batch.dropped) {
		if (!batch.previousName.empty()) {
			LittleFS.remove(batch.previousJournalPath.c_str());
			LittleFS.remove(batch.previousSnapshotPath.c_str());
			LittleFS.rmdir(batch.previousPath.c_str());
		}
		LittleFS.remove(batch.journalPath.c_str());
		LittleFS.remove(batch.snapshotPath.c_str());
		LittleFS.rmdir(batch.path.c_str());
		result.dropped = true;
		result.result = FreshResult::success("model dropped");
		return result;
	}

	if (!batch.previousName.empty()) {
		if (LittleFS.exists(batch.previousPath.c_str()) && !LittleFS.exists(batch.path.c_str())) {
			if (!LittleFS.rename(batch.previousPath.c_str(), batch.path.c_str())) {
				result.result = FreshResult::failure(FreshStatus::FileSystemError, "failed to rename model directory");
				return result;
			}
		}
		result.renamed = true;
	}

	for (const FreshJournalSyncRecord &record : batch.records) {
		FreshResult writeResult = FreshWriteJournalRecord(batch, record);
		if (!writeResult) {
			result.result = writeResult;
			return result;
		}
		result.recordsWritten++;
		result.bytesWritten += record.payload.size();
	}

	if (batch.writeSnapshot) {
		FreshResult snapshotResult = FreshWriteSnapshotBatch(batch);
		if (!snapshotResult) {
			result.result = snapshotResult;
			return result;
		}
		result.snapshotWritten = true;
		result.result = snapshotResult;
	}

	return result;
}

} // namespace

std::string Fresh::modelPath(const std::string &name) const {
	return FreshJoinPath(_rootPath, name);
}

std::string Fresh::modelFile(const std::string &name, const char *fileName) const {
	return FreshJoinPath(modelPath(name), fileName);
}

FreshResult Fresh::ensureDir(const std::string &path) {
	if (LittleFS.exists(path.c_str())) {
		return FreshResult::success();
	}
	if (!LittleFS.mkdir(path.c_str())) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to create directory");
	}
	return FreshResult::success();
}

FreshResult Fresh::readManifest() {
	const std::string path = FreshJoinPath(_rootPath, FreshManifestFile);
	if (!LittleFS.exists(path.c_str())) {
		return FreshResult::success("manifest not found");
	}

	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open manifest");
	}

	JsonDocument doc;
	DeserializationError error = deserializeMsgPack(doc, file);
	file.close();
	if (error) {
		return FreshResult::failure(FreshStatus::CorruptData, "failed to read manifest");
	}

	JsonArrayConst modelsArray = doc["models"].as<JsonArrayConst>();
	for (JsonObjectConst modelObject : modelsArray) {
		const char *name = modelObject["name"] | "";
		const char *type = modelObject["type"] | "general";
		if (!FreshIsValidName(name)) {
			continue;
		}
		auto state = std::make_shared<FreshModel::State>();
		state->name = name;
		state->type = FreshModelTypeFromString(type);
		_models[state->name] = state;
		loadModel(state);
	}

	return FreshResult::success();
}

FreshResult Fresh::writeManifest(const JsonDocument &manifest) {
	FreshResult dirResult = ensureDir(_rootPath);
	if (!dirResult) {
		return dirResult;
	}

	const std::string tempPath = FreshJoinPath(_rootPath, "manifest.tmp");
	const std::string finalPath = FreshJoinPath(_rootPath, FreshManifestFile);
	File file = LittleFS.open(tempPath.c_str(), "w");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write manifest");
	}
	size_t written = serializeMsgPack(manifest, file);
	file.close();
	if (written == 0) {
		LittleFS.remove(tempPath.c_str());
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to serialize manifest");
	}
	LittleFS.remove(finalPath.c_str());
	if (!LittleFS.rename(tempPath.c_str(), finalPath.c_str())) {
		LittleFS.remove(tempPath.c_str());
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to replace manifest");
	}
	return FreshResult::success("manifest written");
}

FreshResult Fresh::applyRecord(
    const std::shared_ptr<FreshModel::State> &state,
    const FreshPendingRecord &record
) {
	if (state->type == FreshModelType::Stream) {
		if (record.op == FreshJournalOp::Append) {
			JsonDocument copy;
			FreshCopyJson(copy, record.doc);
			state->streamEntries.push_back(copy);
		}
		return FreshResult::success();
	}

	if (record.op == FreshJournalOp::Delete) {
		state->docs.erase(record.id);
		return FreshResult::success();
	}

	JsonDocument copy;
	FreshCopyJson(copy, record.doc);
	state->docs[record.id] = copy;
	return FreshResult::success();
}

FreshResult Fresh::loadSnapshot(const std::shared_ptr<FreshModel::State> &state) {
	const std::string path = modelFile(state->name, FreshSnapshotFile);
	if (!LittleFS.exists(path.c_str())) {
		return FreshResult::success("snapshot not found");
	}

	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open snapshot");
	}

	JsonDocument snapshot;
	DeserializationError error = deserializeMsgPack(snapshot, file);
	file.close();
	if (error) {
		state->degraded = true;
		return FreshResult::failure(FreshStatus::CorruptData, "failed to read snapshot");
	}

	state->docs.clear();
	state->streamEntries.clear();
	if (state->type == FreshModelType::Stream) {
		for (JsonVariantConst entry : snapshot["entries"].as<JsonArrayConst>()) {
			JsonDocument copy;
			copy.set(entry);
			state->streamEntries.push_back(copy);
		}
		return FreshResult::success();
	}

	for (JsonVariantConst entry : snapshot["docs"].as<JsonArrayConst>()) {
		JsonDocument copy;
		copy.set(entry);
		const char *id = copy["_id"] | "";
		if (*id != '\0') {
			state->docs[id] = copy;
		}
	}
	return FreshResult::success();
}

FreshResult Fresh::loadJournal(const std::shared_ptr<FreshModel::State> &state) {
	const std::string path = modelFile(state->name, FreshJournalFile);
	if (!LittleFS.exists(path.c_str())) {
		return FreshResult::success("journal not found");
	}

	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open journal");
	}

	while (file.available() > 0) {
		uint32_t magic = 0;
		uint16_t version = 0;
		uint8_t opByte = 0;
		uint8_t reserved = 0;
		uint32_t payloadSize = 0;
		uint32_t expectedChecksum = 0;
		if (!FreshReadU32(file, magic) || !FreshReadU16(file, version)) {
			state->degraded = true;
			break;
		}
		if (file.available() < 10) {
			state->degraded = true;
			break;
		}
		opByte = file.read();
		reserved = file.read();
		(void)reserved;
		if (!FreshReadU32(file, payloadSize) || !FreshReadU32(file, expectedChecksum)) {
			state->degraded = true;
			break;
		}
		if (magic != FreshJournalMagic || version != FreshJournalVersion || payloadSize > 1024 * 1024) {
			state->degraded = true;
			break;
		}
		if (file.available() < static_cast<int>(payloadSize)) {
			state->degraded = true;
			break;
		}

		FreshByteVector payload(payloadSize);
		if (file.read(payload.data(), payloadSize) != static_cast<int>(payloadSize)) {
			state->degraded = true;
			break;
		}
		if (FreshChecksum(payload.data(), payload.size()) != expectedChecksum) {
			state->degraded = true;
			break;
		}

		JsonDocument recordDoc;
		DeserializationError error = deserializeMsgPack(recordDoc, payload.data(), payload.size());
		if (error) {
			state->degraded = true;
			break;
		}

		FreshPendingRecord record;
		record.op = static_cast<FreshJournalOp>(opByte);
		record.id = recordDoc["id"] | "";
		record.doc.set(recordDoc["doc"]);
		applyRecord(state, record);
		state->recordsSinceSnapshot++;
		state->bytesSinceSnapshot += payloadSize;
	}
	file.close();
	return FreshResult::success(state->degraded ? "journal recovered with corruption" : "journal loaded");
}

FreshResult Fresh::loadModel(const std::shared_ptr<FreshModel::State> &state) {
	FreshResult snapshotResult = loadSnapshot(state);
	FreshResult journalResult = loadJournal(state);
	if (!snapshotResult && !journalResult) {
		return snapshotResult;
	}
	state->dirty = false;
	state->pending.clear();
	state->snapshotRequired = false;
	return FreshResult::success();
}

JsonDocument Fresh::recordToJson(const FreshPendingRecord &record) {
	JsonDocument doc;
	doc["op"] = FreshJournalOpToString(record.op);
	doc["id"] = record.id;
	doc["doc"].set(record.doc.as<JsonVariantConst>());
	return doc;
}

FreshResult Fresh::syncDirty(bool force) {
	FreshLock syncLock(*_syncMutex);
	if (!syncLock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock sync");
	}

	JsonDocument manifest;
	bool shouldWriteManifest = false;
	uint32_t manifestEpoch = 0;
	std::vector<FreshModelSyncBatch> batches;
	{
		FreshLock lock(*_mutex);
		if (!lock || !_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		shouldWriteManifest = _manifestDirty;
		manifestEpoch = _manifestEpoch;
		if (shouldWriteManifest) {
			manifest["version"] = 1;
			JsonArray modelsArray = manifest["models"].to<JsonArray>();
			for (const auto &entry : _models) {
				const auto &state = entry.second;
				if (state->dropped) {
					continue;
				}
				JsonObject modelObject = modelsArray.add<JsonObject>();
				modelObject["name"] = state->name;
				modelObject["type"] = FreshModelTypeToString(state->type);
			}
		}

		for (const auto &entry : _models) {
			const auto &state = entry.second;
			if (!state->dirty && state->pending.empty() && !state->snapshotRequired) {
				continue;
			}

			FreshModelSyncBatch batch;
			batch.state = std::static_pointer_cast<void>(state);
			batch.name = state->name;
			batch.previousName = state->previousName;
			batch.path = modelPath(batch.name);
			batch.previousPath = batch.previousName.empty() ? std::string() : modelPath(batch.previousName);
			batch.journalPath = modelFile(batch.name, FreshJournalFile);
			batch.snapshotTempPath = modelFile(batch.name, "snapshot.tmp");
			batch.snapshotPath = modelFile(batch.name, FreshSnapshotFile);
			if (!batch.previousName.empty()) {
				batch.previousJournalPath = modelFile(batch.previousName, FreshJournalFile);
				batch.previousSnapshotPath = modelFile(batch.previousName, FreshSnapshotFile);
			}
			batch.type = state->type;
			batch.dropped = state->dropped;
			batch.storageEpoch = state->storageEpoch;

			size_t pendingBytes = 0;
			if (!batch.dropped) {
				for (const FreshPendingRecord &pending : state->pending) {
					JsonDocument recordDoc = recordToJson(pending);
					FreshJournalSyncRecord record;
					record.sequence = pending.sequence;
					record.op = pending.op;
					record.payload.resize(measureMsgPack(recordDoc));
					if (record.payload.empty()) {
						return FreshResult::failure(FreshStatus::InternalError, "empty journal record");
					}
					serializeMsgPack(recordDoc, record.payload.data(), record.payload.size());
					pendingBytes += record.payload.size();
					batch.records.push_back(std::move(record));
				}
			}

			batch.writeSnapshot = !batch.dropped &&
			                      (force || state->snapshotRequired ||
			                       state->recordsSinceSnapshot + batch.records.size() >= _config.snapshotRecordThreshold ||
			                       state->bytesSinceSnapshot + pendingBytes >= _config.snapshotBytesThreshold);
			if (batch.writeSnapshot) {
				batch.snapshot["version"] = 1;
				batch.snapshot["name"] = batch.name;
				batch.snapshot["type"] = FreshModelTypeToString(batch.type);

				if (batch.type == FreshModelType::Stream) {
					JsonArray entries = batch.snapshot["entries"].to<JsonArray>();
					for (const JsonDocument &streamEntry : state->streamEntries) {
						entries.add(streamEntry.as<JsonVariantConst>());
					}
				} else {
					JsonArray docs = batch.snapshot["docs"].to<JsonArray>();
					for (const auto &docEntry : state->docs) {
						docs.add(docEntry.second.as<JsonVariantConst>());
					}
				}
			}

			batches.push_back(std::move(batch));
		}
		if (!shouldWriteManifest && batches.empty()) {
			return FreshResult::success("nothing dirty");
		}
	}

	emitEvent({.type = FreshEventType::SyncStarted, .result = FreshResult::success("sync started")});

	FreshResult last = FreshResult::success("nothing dirty");
	if (shouldWriteManifest) {
		last = writeManifest(manifest);
		{
			FreshLock lock(*_mutex);
			if (last && _manifestEpoch == manifestEpoch) {
				_manifestDirty = false;
			}
		}
		if (!last) {
			emitSync(last);
			return last;
		}
	}

	for (const FreshModelSyncBatch &batch : batches) {
		FreshModelSyncResult syncResult = FreshWriteModelBatch(batch);
		FreshLock lock(*_mutex);

		auto state = std::static_pointer_cast<FreshModel::State>(batch.state);
		size_t recordsPopped = 0;
		while (recordsPopped < syncResult.recordsWritten && !state->pending.empty() &&
		       state->pending.front().sequence == batch.records[recordsPopped].sequence) {
			state->pending.pop_front();
			recordsPopped++;
		}

		if (state->storageEpoch == batch.storageEpoch) {
			if (syncResult.snapshotWritten) {
				state->recordsSinceSnapshot = 0;
				state->bytesSinceSnapshot = 0;
			} else {
				state->recordsSinceSnapshot += static_cast<uint32_t>(syncResult.recordsWritten);
				state->bytesSinceSnapshot += syncResult.bytesWritten;
			}
		}

		if (syncResult.result && state->storageEpoch == batch.storageEpoch) {
			if (syncResult.snapshotWritten) {
				state->snapshotRequired = false;
			}

			if (syncResult.renamed && state->previousName == batch.previousName) {
				state->previousName.clear();
			}

			if (syncResult.dropped) {
				state->dirty = false;
				state->pending.clear();
				state->snapshotRequired = false;
				auto found = _models.find(batch.name);
				if (found != _models.end() && found->second == state) {
					_models.erase(found);
				}
			} else {
				state->dirty = state->dropped || !state->pending.empty() || state->snapshotRequired ||
				               !state->previousName.empty();
			}
		} else {
			state->dirty = true;
		}

		last = syncResult.result;
		if (!last) {
			emitSync(last);
			return last;
		}
	}

	emitEvent({.type = FreshEventType::SyncFinished, .result = last});
	emitSync(last);
	return last;
}

FreshResult Fresh::forceSyncAsync() {
	FreshLock lock(*_mutex);
	if (!_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	_forceSyncRequested = true;
	if (_syncTaskHandle != nullptr) {
		xTaskNotifyGive(_syncTaskHandle);
	}
	return FreshResult::success("sync requested");
}

FreshResult Fresh::forceSync() {
	return syncDirty(true);
}
