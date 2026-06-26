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
	std::string previousJournalPath;
	FreshModelType type = FreshModelType::General;
	bool dropped = false;
	bool writeSnapshot = false;
	uint32_t storageEpoch = 0;
	std::vector<FreshJournalSyncRecord> records;
	JsonDocument snapshot;
	size_t snapshotPayloadBytes = 0;
};

struct FreshModelSyncResult {
	FreshResult result = FreshResult::success("model synced");
	size_t recordsWritten = 0;
	size_t bytesWritten = 0;
	bool snapshotWritten = false;
	bool dropped = false;
	bool renamed = false;
};

std::string FreshSlotPath(const std::string &basePath, const char *fileBaseName, char slot) {
	std::string fileName = fileBaseName;
	fileName += ".";
	fileName.push_back(slot);
	fileName += ".msgpack";
	return FreshJoinPath(basePath, fileName);
}

FreshResult FreshSerializePayload(const JsonDocument &payload, FreshByteVector &bytes, const char *label) {
	const size_t payloadBytes = measureMsgPack(payload);
	if (payloadBytes == 0) {
		return FreshResult::failure(FreshStatus::FileSystemError, label);
	}
	bytes.resize(payloadBytes);
	const size_t written = serializeMsgPack(payload, bytes.data(), bytes.size());
	if (written != payloadBytes) {
		bytes.clear();
		return FreshResult::failure(FreshStatus::FileSystemError, label);
	}
	return FreshResult::success();
}

FreshResult FreshReadSlotFile(
    const std::string &path,
    JsonDocument &payload,
    uint64_t &generation
) {
	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open durable slot");
	}

	uint32_t magic = 0;
	uint16_t version = 0;
	uint64_t slotGeneration = 0;
	uint32_t payloadSize = 0;
	uint32_t expectedChecksum = 0;
	const bool headerOk = FreshReadU32(file, magic) && FreshReadU16(file, version) &&
	                      FreshReadU64(file, slotGeneration) && FreshReadU32(file, payloadSize) &&
	                      FreshReadU32(file, expectedChecksum);
	if (!headerOk || magic != FreshSlotMagic || version != FreshSlotVersion || payloadSize == 0 ||
	    payloadSize > 1024 * 1024 ||
	    file.available() < static_cast<int>(payloadSize)) {
		file.close();
		return FreshResult::failure(FreshStatus::CorruptData, "invalid durable slot header");
	}

	FreshByteVector bytes(payloadSize);
	const int read = file.read(bytes.data(), bytes.size());
	file.close();
	if (read != static_cast<int>(bytes.size())) {
		return FreshResult::failure(FreshStatus::CorruptData, "truncated durable slot");
	}
	if (FreshChecksum(bytes.data(), bytes.size()) != expectedChecksum) {
		return FreshResult::failure(FreshStatus::CorruptData, "durable slot checksum mismatch");
	}

	JsonDocument decoded;
	DeserializationError error = deserializeMsgPack(decoded, bytes.data(), bytes.size());
	if (error) {
		return FreshResult::failure(FreshStatus::CorruptData, "failed to decode durable slot");
	}
	payload = std::move(decoded);
	generation = slotGeneration;
	return FreshResult::success("durable slot loaded");
}

FreshSlotReadResult readDurableSlot(const std::string &basePath, const char *fileBaseName) {
	FreshSlotReadResult result;
	const std::string slotAPath = FreshSlotPath(basePath, fileBaseName, 'a');
	const std::string slotBPath = FreshSlotPath(basePath, fileBaseName, 'b');
	const bool existsA = LittleFS.exists(slotAPath.c_str());
	const bool existsB = LittleFS.exists(slotBPath.c_str());
	if (!existsA && !existsB) {
		result.missing = true;
		result.result = FreshResult::success("durable slot missing");
		return result;
	}

	for (const std::string &path : {slotAPath, slotBPath}) {
		if (!LittleFS.exists(path.c_str())) {
			continue;
		}
		JsonDocument candidate;
		uint64_t candidateGeneration = 0;
		FreshResult readResult = FreshReadSlotFile(path, candidate, candidateGeneration);
		if (!readResult) {
			result.hadCorruptSlot = true;
			continue;
		}
		if (!result.hadValidSlot || candidateGeneration > result.generation) {
			result.payload = std::move(candidate);
			result.generation = candidateGeneration;
		}
		result.hadValidSlot = true;
	}

	result.missing = false;
	if (!result.hadValidSlot) {
		result.result = FreshResult::failure(FreshStatus::CorruptData, "no valid durable slot");
		return result;
	}
	result.result = FreshResult::success(
	    result.hadCorruptSlot ? "durable slot loaded with recovery" : "durable slot loaded"
	);
	return result;
}

FreshResult writeDurableSlot(
    const std::string &basePath,
    const char *fileBaseName,
    const JsonDocument &payload,
    uint64_t currentGeneration
) {
	FreshByteVector bytes;
	FreshResult serializeResult = FreshSerializePayload(payload, bytes, "failed to serialize durable slot");
	if (!serializeResult) {
		return serializeResult;
	}
	if (bytes.size() > UINT32_MAX) {
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, "durable slot payload is too large");
	}

	const uint64_t nextGeneration = currentGeneration + 1;
	const char targetSlot = nextGeneration % 2 == 0 ? 'a' : 'b';
	const std::string targetPath = FreshSlotPath(basePath, fileBaseName, targetSlot);
	File file = LittleFS.open(targetPath.c_str(), "w");
	if (!file) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open durable slot");
	}

	FreshWriteU32(file, FreshSlotMagic);
	FreshWriteU16(file, FreshSlotVersion);
	FreshWriteU64(file, nextGeneration);
	FreshWriteU32(file, static_cast<uint32_t>(bytes.size()));
	FreshWriteU32(file, FreshChecksum(bytes.data(), bytes.size()));
	const size_t written = file.write(bytes.data(), bytes.size());
	file.flush();
	file.close();
	if (written != bytes.size()) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to write durable slot");
	}

	JsonDocument verified;
	uint64_t verifiedGeneration = 0;
	FreshResult verifyResult = FreshReadSlotFile(targetPath, verified, verifiedGeneration);
	if (!verifyResult || verifiedGeneration != nextGeneration) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to verify durable slot");
	}
	return FreshResult::success("durable slot written");
}

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

	FreshSlotReadResult slot = readDurableSlot(batch.path, FreshSnapshotFile);
	if (!slot.result) {
		return slot.result;
	}
	FreshResult writeResult = writeDurableSlot(batch.path, FreshSnapshotFile, batch.snapshot, slot.generation);
	if (!writeResult) {
		return writeResult;
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
			LittleFS.remove(FreshSlotPath(batch.previousPath, FreshSnapshotFile, 'a').c_str());
			LittleFS.remove(FreshSlotPath(batch.previousPath, FreshSnapshotFile, 'b').c_str());
			LittleFS.rmdir(batch.previousPath.c_str());
		}
		LittleFS.remove(batch.journalPath.c_str());
		LittleFS.remove(FreshSlotPath(batch.path, FreshSnapshotFile, 'a').c_str());
		LittleFS.remove(FreshSlotPath(batch.path, FreshSnapshotFile, 'b').c_str());
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

FreshResult FreshLoadResult(FreshLoadStatus status, const char *message) {
	FreshResult result = FreshResult::success(message);
	result.affectedCount = static_cast<size_t>(status);
	return result;
}

FreshLoadStatus FreshLoadStatusFromResult(const FreshResult &result) {
	return static_cast<FreshLoadStatus>(result.affectedCount);
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

FreshResult Fresh::checkFreeSpace(size_t requiredBytes) const {
	const size_t total = LittleFS.totalBytes();
	const size_t used = LittleFS.usedBytes();
	const size_t freeBytes = total > used ? total - used : 0;
	if (freeBytes < _config.minFreeBytes || freeBytes - _config.minFreeBytes < requiredBytes) {
		return FreshResult::failure(FreshStatus::StorageFull, "not enough LittleFS space");
	}
	return FreshResult::success();
}

FreshResult Fresh::checkPayloadSize(size_t payloadBytes, size_t limit, const char *label) const {
	if (limit > 0 && payloadBytes > limit) {
		std::string message = label != nullptr ? label : "payload";
		message += " size limit exceeded";
		return FreshResult::failure(FreshStatus::SizeLimitExceeded, message.c_str(), payloadBytes);
	}
	return FreshResult::success();
}

FreshResult Fresh::readManifest() {
	FreshSlotReadResult manifestSlot = readDurableSlot(_rootPath, FreshManifestFile);
	if (!manifestSlot.result) {
		return manifestSlot.result;
	}
	if (manifestSlot.missing) {
		return FreshResult::success("manifest not found");
	}

	JsonArrayConst modelsArray = manifestSlot.payload["models"].as<JsonArrayConst>();
	bool failed = false;
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
		FreshResult loadResult = loadModel(state);
		FreshLoadStatus loadStatus = FreshLoadStatusFromResult(loadResult);
		FreshModelLoadInfo info;
		info.modelName = state->name;
		info.modelType = state->type;
		info.status = loadStatus;
		info.degraded = loadStatus != FreshLoadStatus::LoadedOk;
		info.message = loadResult.message;
		_diagnostics.modelLoads.push_back(info);
		if (info.degraded) {
			_diagnostics.degradedModelCount++;
		}
		if (!loadResult || loadStatus == FreshLoadStatus::FailedToLoad) {
			failed = true;
		}
	}

	if (failed) {
		return FreshResult::failure(FreshStatus::CorruptData, "failed to load one or more models");
	}
	if (_diagnostics.degradedModelCount > 0) {
		return FreshResult::success("manifest loaded with recovered models", _diagnostics.degradedModelCount);
	}
	if (manifestSlot.hadCorruptSlot) {
		return FreshResult::success("manifest loaded with recovered slot");
	}
	return FreshResult::success();
}

FreshResult Fresh::writeManifest(const JsonDocument &manifest) {
	FreshResult dirResult = ensureDir(_rootPath);
	if (!dirResult) {
		return dirResult;
	}

	FreshSlotReadResult slot = readDurableSlot(_rootPath, FreshManifestFile);
	if (!slot.result) {
		return slot.result;
	}
	FreshResult writeResult = writeDurableSlot(_rootPath, FreshManifestFile, manifest, slot.generation);
	if (!writeResult) {
		return writeResult;
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
			FreshResult cloneResult = FreshCloneJson(copy, record.doc.as<JsonVariantConst>(), "stream record");
			if (!cloneResult) {
				return cloneResult;
			}
			state->streamEntries.push_back(std::move(copy));
		}
		return FreshResult::success();
	}

	if (record.op == FreshJournalOp::Delete) {
		state->docs.erase(record.id);
		return FreshResult::success();
	}

	JsonDocument copy;
	FreshResult cloneResult = FreshCloneJson(copy, record.doc.as<JsonVariantConst>(), "journal document");
	if (!cloneResult) {
		return cloneResult;
	}
	state->docs[record.id] = std::move(copy);
	return FreshResult::success();
}

FreshResult Fresh::loadSnapshot(const std::shared_ptr<FreshModel::State> &state) {
	FreshSlotReadResult snapshotSlot = readDurableSlot(modelPath(state->name), FreshSnapshotFile);
	if (!snapshotSlot.result) {
		state->degraded = true;
		return snapshotSlot.result;
	}
	if (snapshotSlot.missing) {
		return FreshLoadResult(FreshLoadStatus::LoadedOk, "snapshot not found");
	}

	state->docs.clear();
	state->streamEntries.clear();
	if (state->type == FreshModelType::Stream) {
		for (JsonVariantConst entry : snapshotSlot.payload["entries"].as<JsonArrayConst>()) {
			JsonDocument copy;
			FreshResult cloneResult = FreshCloneJson(copy, entry, "snapshot stream entry");
			if (!cloneResult) {
				state->degraded = true;
				return cloneResult;
			}
			state->streamEntries.push_back(std::move(copy));
		}
		if (snapshotSlot.hadCorruptSlot) {
			state->degraded = true;
			return FreshLoadResult(FreshLoadStatus::LoadedWithCorruptSnapshot, "snapshot loaded with recovery");
		}
		return FreshLoadResult(FreshLoadStatus::LoadedOk, "snapshot loaded");
	}

	for (JsonVariantConst entry : snapshotSlot.payload["docs"].as<JsonArrayConst>()) {
		JsonDocument copy;
		FreshResult cloneResult = FreshCloneJson(copy, entry, "snapshot document");
		if (!cloneResult) {
			state->degraded = true;
			return cloneResult;
		}
		const char *id = copy["_id"] | "";
		if (*id != '\0') {
			state->docs[id] = std::move(copy);
		}
	}
	if (snapshotSlot.hadCorruptSlot) {
		state->degraded = true;
		return FreshLoadResult(FreshLoadStatus::LoadedWithCorruptSnapshot, "snapshot loaded with recovery");
	}
	return FreshLoadResult(FreshLoadStatus::LoadedOk, "snapshot loaded");
}

FreshResult Fresh::loadJournal(const std::shared_ptr<FreshModel::State> &state) {
	const std::string path = modelFile(state->name, FreshJournalFile);
	if (!LittleFS.exists(path.c_str())) {
		return FreshLoadResult(FreshLoadStatus::LoadedOk, "journal not found");
	}

	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		state->degraded = true;
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to open journal");
	}

	bool journalRecovered = false;
	while (file.available() > 0) {
		uint32_t magic = 0;
		uint16_t version = 0;
		uint8_t opByte = 0;
		uint8_t reserved = 0;
		uint32_t payloadSize = 0;
		uint32_t expectedChecksum = 0;
		if (!FreshReadU32(file, magic) || !FreshReadU16(file, version)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (file.available() < 10) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		opByte = file.read();
		reserved = file.read();
		(void)reserved;
		if (!FreshReadU32(file, payloadSize) || !FreshReadU32(file, expectedChecksum)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (magic != FreshJournalMagic || version != FreshJournalVersion || payloadSize > 1024 * 1024) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (file.available() < static_cast<int>(payloadSize)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}

		FreshByteVector payload(payloadSize);
		if (file.read(payload.data(), payloadSize) != static_cast<int>(payloadSize)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		if (FreshChecksum(payload.data(), payload.size()) != expectedChecksum) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}

		JsonDocument recordDoc;
		DeserializationError error = deserializeMsgPack(recordDoc, payload.data(), payload.size());
		if (error) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}

		FreshPendingRecord record;
		if (!FreshParseJournalOp(opByte, record.op)) {
			state->degraded = true;
			journalRecovered = true;
			break;
		}
		record.id = recordDoc["id"] | "";
		FreshResult cloneResult = FreshCloneJson(record.doc, recordDoc["doc"].as<JsonVariantConst>(), "journal document");
		if (!cloneResult) {
			state->degraded = true;
			file.close();
			return cloneResult;
		}
		FreshResult applyResult = applyRecord(state, record);
		if (!applyResult) {
			state->degraded = true;
			file.close();
			return applyResult;
		}
		state->recordsSinceSnapshot++;
		state->bytesSinceSnapshot += payloadSize;
	}
	file.close();
	return FreshLoadResult(
	    journalRecovered ? FreshLoadStatus::LoadedWithRecoveredJournal : FreshLoadStatus::LoadedOk,
	    journalRecovered ? "journal recovered with corruption" : "journal loaded"
	);
}

FreshResult Fresh::loadModel(const std::shared_ptr<FreshModel::State> &state) {
	FreshResult snapshotResult = loadSnapshot(state);
	const bool snapshotFailed = !snapshotResult;
	FreshResult journalResult = loadJournal(state);
	const bool journalFailed = !journalResult;
	const std::string journalMessage = journalResult.message;

	FreshLoadStatus status = FreshLoadStatus::LoadedOk;
	const char *message = "model loaded";
	if (snapshotFailed && journalFailed) {
		status = FreshLoadStatus::FailedToLoad;
		message = "failed to load snapshot and journal";
	} else if (snapshotFailed && journalMessage == "journal not found") {
		status = FreshLoadStatus::FailedToLoad;
		message = "failed to load snapshot and journal";
	} else if (snapshotFailed) {
		status = FreshLoadStatus::LoadedWithCorruptSnapshot;
		message = "model loaded with corrupt snapshot";
	} else if (journalFailed) {
		status = FreshLoadStatus::LoadedWithCorruptJournal;
		message = "model loaded with corrupt journal";
	} else {
		FreshLoadStatus snapshotStatus = FreshLoadStatusFromResult(snapshotResult);
		FreshLoadStatus journalStatus = FreshLoadStatusFromResult(journalResult);
		if (snapshotStatus == FreshLoadStatus::LoadedWithCorruptSnapshot) {
			status = snapshotStatus;
			message = "model loaded with recovered snapshot slot";
		} else if (journalStatus == FreshLoadStatus::LoadedWithRecoveredJournal) {
			status = journalStatus;
			message = "model loaded with recovered journal";
		}
	}

	state->dirty = false;
	state->pending.clear();
	state->snapshotRequired = false;
	state->degraded = status != FreshLoadStatus::LoadedOk;
	if (status == FreshLoadStatus::FailedToLoad) {
		return FreshResult::failure(FreshStatus::CorruptData, message, static_cast<size_t>(status));
	}
	return FreshLoadResult(status, message);
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
	size_t requiredBytes = 0;
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
			const size_t manifestPayloadBytes = measureMsgPack(manifest);
			if (manifestPayloadBytes == 0) {
				return FreshResult::failure(FreshStatus::InternalError, "empty manifest");
			}
			requiredBytes += FreshSlotHeaderSize + manifestPayloadBytes;
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
			if (!batch.previousName.empty()) {
				batch.previousJournalPath = modelFile(batch.previousName, FreshJournalFile);
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
					const size_t written = serializeMsgPack(recordDoc, record.payload.data(), record.payload.size());
					if (written != record.payload.size()) {
						return FreshResult::failure(FreshStatus::InternalError, "failed to serialize journal record");
					}
					FreshResult sizeResult =
					    checkPayloadSize(record.payload.size(), _config.maxJournalRecordBytes, "journal record");
					if (!sizeResult) {
						return sizeResult;
					}
					pendingBytes += record.payload.size();
					requiredBytes += FreshJournalHeaderSize + record.payload.size();
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
				batch.snapshotPayloadBytes = measureMsgPack(batch.snapshot);
				if (batch.snapshotPayloadBytes == 0) {
					return FreshResult::failure(FreshStatus::InternalError, "empty snapshot");
				}
				FreshResult snapshotSizeResult =
				    checkPayloadSize(batch.snapshotPayloadBytes, _config.maxSnapshotBytes, "snapshot");
				if (!snapshotSizeResult) {
					return snapshotSizeResult;
				}
				requiredBytes += FreshSlotHeaderSize + batch.snapshotPayloadBytes;
			}

			batches.push_back(std::move(batch));
		}
		if (!shouldWriteManifest && batches.empty()) {
			return FreshResult::success("nothing dirty");
		}
	}

	emitEvent({.type = FreshEventType::SyncStarted, .result = FreshResult::success("sync started")});

	FreshResult last = FreshResult::success("nothing dirty");
	FreshResult spaceResult = checkFreeSpace(requiredBytes);
	if (!spaceResult) {
		emitSync(spaceResult);
		return spaceResult;
	}

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
	if (_stopping) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}
	_forceSyncRequested = true;
	if (_syncTaskHandle != nullptr) {
		xTaskNotifyGive(_syncTaskHandle);
	}
	return FreshResult::success("sync requested");
}

FreshResult Fresh::forceSync() {
	{
		FreshLock lock(*_mutex);
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}
	return syncDirty(true);
}
