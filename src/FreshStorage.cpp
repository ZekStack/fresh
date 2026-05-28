#include "Fresh.h"
#include "internal/FreshInternal.h"

#include <LittleFS.h>

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

FreshResult Fresh::writeManifest() {
	FreshResult dirResult = ensureDir(_rootPath);
	if (!dirResult) {
		return dirResult;
	}

	JsonDocument doc;
	doc["version"] = 1;
	JsonArray modelsArray = doc["models"].to<JsonArray>();
	for (const auto &entry : _models) {
		const auto &state = entry.second;
		if (state->dropped) {
			continue;
		}
		JsonObject modelObject = modelsArray.add<JsonObject>();
		modelObject["name"] = state->name;
		modelObject["type"] = FreshModelTypeToString(state->type);
	}

	const std::string tempPath = FreshJoinPath(_rootPath, "manifest.tmp");
	const std::string finalPath = FreshJoinPath(_rootPath, FreshManifestFile);
	File file = LittleFS.open(tempPath.c_str(), "w");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write manifest");
	}
	size_t written = serializeMsgPack(doc, file);
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
	_manifestDirty = false;
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

FreshResult Fresh::appendJournalRecord(
    const std::shared_ptr<FreshModel::State> &state,
    const FreshPendingRecord &record
) {
	FreshResult dirResult = ensureDir(modelPath(state->name));
	if (!dirResult) {
		return dirResult;
	}

	JsonDocument recordDoc = recordToJson(record);
	FreshByteVector payload;
	payload.resize(measureMsgPack(recordDoc));
	if (payload.empty()) {
		return FreshResult::failure(FreshStatus::InternalError, "empty journal record");
	}
	serializeMsgPack(recordDoc, payload.data(), payload.size());

	const std::string path = modelFile(state->name, FreshJournalFile);
	File file = LittleFS.open(path.c_str(), "a");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open journal");
	}

	FreshWriteU32(file, FreshJournalMagic);
	FreshWriteU16(file, FreshJournalVersion);
	file.write(static_cast<uint8_t>(record.op));
	file.write(static_cast<uint8_t>(0));
	FreshWriteU32(file, static_cast<uint32_t>(payload.size()));
	FreshWriteU32(file, FreshChecksum(payload.data(), payload.size()));
	size_t written = file.write(payload.data(), payload.size());
	file.flush();
	file.close();

	if (written != payload.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write journal");
	}
	state->recordsSinceSnapshot++;
	state->bytesSinceSnapshot += payload.size();
	return FreshResult::success("journal record written");
}

FreshResult Fresh::writeSnapshot(const std::shared_ptr<FreshModel::State> &state) {
	FreshResult dirResult = ensureDir(modelPath(state->name));
	if (!dirResult) {
		return dirResult;
	}

	JsonDocument snapshot;
	snapshot["version"] = 1;
	snapshot["name"] = state->name;
	snapshot["type"] = FreshModelTypeToString(state->type);

	if (state->type == FreshModelType::Stream) {
		JsonArray entries = snapshot["entries"].to<JsonArray>();
		for (const JsonDocument &entry : state->streamEntries) {
			entries.add(entry.as<JsonVariantConst>());
		}
	} else {
		JsonArray docs = snapshot["docs"].to<JsonArray>();
		for (const auto &entry : state->docs) {
			docs.add(entry.second.as<JsonVariantConst>());
		}
	}

	const std::string tempPath = modelFile(state->name, "snapshot.tmp");
	const std::string finalPath = modelFile(state->name, FreshSnapshotFile);
	File file = LittleFS.open(tempPath.c_str(), "w");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open snapshot");
	}
	size_t written = serializeMsgPack(snapshot, file);
	file.close();
	if (written == 0) {
		LittleFS.remove(tempPath.c_str());
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write snapshot");
	}
	LittleFS.remove(finalPath.c_str());
	if (!LittleFS.rename(tempPath.c_str(), finalPath.c_str())) {
		LittleFS.remove(tempPath.c_str());
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to replace snapshot");
	}
	LittleFS.remove(modelFile(state->name, FreshJournalFile).c_str());
	state->recordsSinceSnapshot = 0;
	state->bytesSinceSnapshot = 0;
	state->snapshotRequired = false;
	return FreshResult::success("snapshot written");
}

FreshResult Fresh::syncModel(const std::shared_ptr<FreshModel::State> &state) {
	if (!state->dirty && state->pending.empty() && !state->snapshotRequired) {
		return FreshResult::success("model clean");
	}
	if (state->dropped) {
		const std::string path = modelPath(state->name);
		if (!state->previousName.empty()) {
			LittleFS.remove(modelFile(state->previousName, FreshJournalFile).c_str());
			LittleFS.remove(modelFile(state->previousName, FreshSnapshotFile).c_str());
			LittleFS.rmdir(modelPath(state->previousName).c_str());
		}
		LittleFS.remove(modelFile(state->name, FreshJournalFile).c_str());
		LittleFS.remove(modelFile(state->name, FreshSnapshotFile).c_str());
		LittleFS.rmdir(path.c_str());
		state->dirty = false;
		state->pending.clear();
		state->snapshotRequired = false;
		return FreshResult::success("model dropped");
	}

	if (!state->previousName.empty()) {
		const std::string oldPath = modelPath(state->previousName);
		const std::string newPath = modelPath(state->name);
		if (LittleFS.exists(oldPath.c_str()) && !LittleFS.exists(newPath.c_str())) {
			if (!LittleFS.rename(oldPath.c_str(), newPath.c_str())) {
				return FreshResult::failure(FreshStatus::FileSystemError, "failed to rename model directory");
			}
		}
		state->previousName.clear();
	}

	while (!state->pending.empty()) {
		FreshPendingRecord record = state->pending.front();
		FreshResult result = appendJournalRecord(state, record);
		if (!result) {
			return result;
		}
		state->pending.pop_front();
	}

	if (state->snapshotRequired || state->recordsSinceSnapshot >= _config.snapshotRecordThreshold ||
	    state->bytesSinceSnapshot >= _config.snapshotBytesThreshold) {
		FreshResult snapshotResult = writeSnapshot(state);
		if (!snapshotResult) {
			return snapshotResult;
		}
	}

	state->dirty = false;
	return FreshResult::success("model synced");
}

FreshResult Fresh::syncDirty(bool force) {
	(void)force;
	std::vector<std::shared_ptr<FreshModel::State>> snapshot;
	bool shouldWriteManifest = false;
	{
		FreshLock lock(*_mutex);
		if (!lock || !_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		shouldWriteManifest = _manifestDirty;
		for (const auto &entry : _models) {
			const auto &state = entry.second;
			if (state->dirty || !state->pending.empty()) {
				snapshot.push_back(state);
			}
		}
		if (!shouldWriteManifest && snapshot.empty()) {
			return FreshResult::success("nothing dirty");
		}
	}

	emitEvent({.type = FreshEventType::SyncStarted, .result = FreshResult::success("sync started")});

	FreshResult last = FreshResult::success("nothing dirty");
	if (shouldWriteManifest) {
		FreshLock lock(*_mutex);
		last = writeManifest();
		if (!last) {
			emitSync(last);
			return last;
		}
	}

	for (const auto &state : snapshot) {
		FreshLock lock(*_mutex);
		last = syncModel(state);
		if (!last) {
			emitSync(last);
			return last;
		}
		if (state->dropped) {
			_models.erase(state->name);
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
	if (_syncTaskHandle != nullptr) {
		xTaskNotifyGive(_syncTaskHandle);
	}
	return FreshResult::success("sync requested");
}

FreshResult Fresh::forceSync() {
	return syncDirty(true);
}
