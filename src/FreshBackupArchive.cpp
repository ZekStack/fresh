#include "internal/FreshBackupArchive.h"
#include "internal/FreshBackupFormat.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <climits>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>

using namespace fresh_backup_v2;

namespace {

FreshResult backupCorrupt(const char *message) {
	return FreshResult::failure(FreshStatus::CorruptData, message);
}

FreshResult backupOutOfMemory(const char *message) {
	return FreshResult::failure(FreshStatus::OutOfMemory, message);
}

FreshResult backupTooLarge(const char *message) {
	return FreshResult::failure(FreshStatus::SizeLimitExceeded, message);
}

bool readPrefix(Stream &input, uint8_t prefix[4]) {
	return input.readBytes(reinterpret_cast<char *>(prefix), 4) == 4;
}

class CountingStream : public Stream {
  public:
	explicit CountingStream(Stream &input) : _input(input) {
	}

	int available() override {
		return _input.available();
	}

	int read() override {
		const int value = _input.read();
		if (value >= 0) _bytesRead++;
		return value;
	}

	int peek() override {
		return _input.peek();
	}

	void flush() override {
		_input.flush();
	}

	size_t write(uint8_t) override {
		return 0;
	}

	uint64_t bytesRead() const {
		return _bytesRead;
	}

	using Print::write;

  private:
	Stream &_input;
	uint64_t _bytesRead = 0;
};

FreshResult validateDecodedRecord(
    JsonDocument &document,
    FreshModelType type,
    size_t maxDocumentBytes,
    std::set<std::string> &documentIds
) {
	if (!document.is<JsonObjectConst>()) {
		return backupCorrupt("backup record must be an object");
	}
	const size_t measuredBytes = measureMsgPack(document);
	if (measuredBytes == 0 || measuredBytes > maxDocumentBytes) {
		return backupTooLarge(
		    type == FreshModelType::Stream ? "backup stream entry is too large"
		                                   : "backup document is too large"
		);
	}
	if (type == FreshModelType::General) {
		const char *id = document["_id"] | "";
		if (*id == '\0' || !documentIds.insert(id).second) {
			return backupCorrupt("backup contains invalid document id");
		}
	}
	return FreshResult::success();
}

FreshResult readFramedArchive(
    PrefixStream &input,
    size_t maxDocumentBytes,
    FreshBackupArchiveVisitor &visitor,
    FreshBackupMetadata &metadata
) {
	ContainerHeader header;
	Reader reader(input);
	const char *parseError = nullptr;
	if (!reader.readContainerHeader(header, parseError)) return backupCorrupt(parseError);
	if (header.modelCount > SIZE_MAX || header.recordCount > SIZE_MAX || header.totalBytes > SIZE_MAX) {
		return backupTooLarge("backup counters exceed platform limits");
	}

	metadata.containerVersion = Version;
	metadata.generatedAt = header.generatedAt;
	metadata.totalBytes = static_cast<size_t>(header.totalBytes);
	metadata.modelCount = static_cast<size_t>(header.modelCount);
	metadata.recordCount = static_cast<size_t>(header.recordCount);
	metadata.legacyFormat = false;
	metadata.models.clear();

	FreshResult callbackResult = visitor.onArchiveBegin(metadata);
	if (!callbackResult) return callbackResult;

	FreshBackupModelMetadata currentModel;
	bool modelOpen = false;
	uint64_t currentReadRecords = 0;
	uint64_t parsedRecords = 0;
	uint32_t parsedModels = 0;
	std::set<std::string> documentIds;
	bool finished = false;

	while (!finished) {
		if (reader.bytesRead() >= header.totalBytes) {
			return backupCorrupt("backup is missing archive trailer");
		}

		FrameHeader frame;
		if (!reader.readFrameHeader(frame, parseError)) return backupCorrupt(parseError);
		switch (frame.type) {
		case FrameType::ModelBegin:
			if (modelOpen || frame.payloadSize < ModelBeginFixedPayloadSize ||
			    frame.payloadSize > MaxMetadataPayloadSize) {
				return backupCorrupt("invalid model-begin frame");
			}
			break;
		case FrameType::Record:
			if (!modelOpen || frame.payloadSize == 0 || frame.payloadSize > maxDocumentBytes) {
				return frame.payloadSize > maxDocumentBytes
			           ? backupTooLarge("backup record is too large")
			           : backupCorrupt("invalid record frame");
			}
			break;
		case FrameType::ModelEnd:
			if (!modelOpen || frame.payloadSize != ModelEndPayloadSize) {
				return backupCorrupt("invalid model-end frame");
			}
			break;
		case FrameType::ArchiveEnd:
			if (modelOpen || frame.payloadSize != ArchiveEndPayloadSize) {
				return backupCorrupt("invalid archive-end frame");
			}
			break;
		}

		FreshBuffer payload;
		if (!reader.readFramePayload(frame, payload, parseError)) {
			if (std::strcmp(parseError, "failed to allocate backup frame") == 0) {
				return backupOutOfMemory(parseError);
			}
			return backupCorrupt(parseError);
		}
		if (reader.bytesRead() > header.totalBytes) {
			return backupCorrupt("backup exceeds declared byte count");
		}

		switch (frame.type) {
		case FrameType::ModelBegin: {
			const uint8_t typeByte = payload[0];
			if (typeByte > 1 || payload[1] != 0) {
				return backupCorrupt("invalid backup model type");
			}
			const uint16_t nameLength = getU16(payload.data() + 2);
			const uint64_t declaredRecords = getU64(payload.data() + 4);
			if (nameLength == 0 || payload.size() != ModelBeginFixedPayloadSize + nameLength ||
			    declaredRecords > SIZE_MAX) {
				return backupCorrupt("invalid backup model metadata");
			}
			std::string name(
			    reinterpret_cast<const char *>(payload.data() + ModelBeginFixedPayloadSize),
			    nameLength
			);
			if (!FreshIsValidName(name.c_str()) || parsedModels >= header.modelCount) {
				return backupCorrupt("backup contains invalid model");
			}
			for (const FreshBackupModelMetadata &model : metadata.models) {
				if (model.name == name) return backupCorrupt("backup contains duplicate model");
			}

			currentModel.name = std::move(name);
			currentModel.type = typeByte == 1 ? FreshModelType::Stream : FreshModelType::General;
			currentModel.recordCount = static_cast<size_t>(declaredRecords);
			metadata.models.push_back(currentModel);
			modelOpen = true;
			currentReadRecords = 0;
			documentIds.clear();
			callbackResult = visitor.onModelBegin(currentModel);
			if (!callbackResult) return callbackResult;
			break;
		}
		case FrameType::Record: {
			if (currentReadRecords >= currentModel.recordCount || parsedRecords >= header.recordCount) {
				return backupCorrupt("backup contains too many records");
			}
			JsonDocument document(&FreshJsonAllocator());
			DeserializationError error = deserializeMsgPack(document, payload.data(), payload.size());
			if (error || document.overflowed()) {
				return FreshResult::failure(
				    error == DeserializationError::NoMemory || document.overflowed()
				        ? FreshStatus::OutOfMemory
				        : FreshStatus::CorruptData,
				    "failed to decode backup record"
				);
			}
			FreshResult recordResult = validateDecodedRecord(
			    document,
			    currentModel.type,
			    maxDocumentBytes,
			    documentIds
			);
			if (!recordResult) return recordResult;
			callbackResult = visitor.onRecord(currentModel.type, std::move(document));
			if (!callbackResult) return callbackResult;
			currentReadRecords++;
			parsedRecords++;
			break;
		}
		case FrameType::ModelEnd: {
			const uint64_t endCount = getU64(payload.data());
			if (endCount != currentModel.recordCount || currentReadRecords != currentModel.recordCount) {
				return backupCorrupt("backup model record count mismatch");
			}
			callbackResult = visitor.onModelEnd(currentModel);
			if (!callbackResult) return callbackResult;
			currentModel = FreshBackupModelMetadata();
			modelOpen = false;
			currentReadRecords = 0;
			documentIds.clear();
			parsedModels++;
			break;
		}
		case FrameType::ArchiveEnd: {
			const uint32_t endModels = getU32(payload.data());
			const uint64_t endRecords = getU64(payload.data() + 4);
			if (endModels != header.modelCount || endRecords != header.recordCount ||
			    parsedModels != header.modelCount || parsedRecords != header.recordCount ||
			    metadata.models.size() != header.modelCount) {
				return backupCorrupt("backup archive count mismatch");
			}
			finished = true;
			break;
		}
		}
	}

	if (reader.bytesRead() != header.totalBytes) return backupCorrupt("backup byte count mismatch");
	if (input.available() > 0) return backupCorrupt("backup contains trailing bytes");
	callbackResult = visitor.onArchiveEnd(metadata);
	if (!callbackResult) return callbackResult;
	return FreshResult::success("backup archive valid", metadata.modelCount);
}

FreshResult readLegacyArchive(
    PrefixStream &prefixed,
    size_t maxDocumentBytes,
    FreshBackupArchiveVisitor &visitor,
    FreshBackupMetadata &metadata
) {
	CountingStream input(prefixed);
	JsonDocument archive(&FreshJsonAllocator());
	DeserializationError error = deserializeMsgPack(archive, input);
	if (error || archive.overflowed()) {
		return FreshResult::failure(
		    error == DeserializationError::NoMemory || archive.overflowed()
		        ? FreshStatus::OutOfMemory
		        : FreshStatus::CorruptData,
		    "failed to read legacy backup"
		);
	}
	if (input.available() > 0) return backupCorrupt("legacy backup contains trailing bytes");
	if (!archive.is<JsonObjectConst>() || (archive["version"] | 0U) != FreshBackupVersion ||
	    !archive["modelCount"].is<uint64_t>() || !archive["models"].is<JsonArrayConst>()) {
		return backupCorrupt("unsupported or corrupt legacy backup");
	}
	if (!archive["generatedAt"].isNull() && !archive["generatedAt"].is<uint64_t>()) {
		return backupCorrupt("legacy backup timestamp is invalid");
	}

	const uint64_t declaredModelCount = archive["modelCount"].as<uint64_t>();
	const JsonArrayConst modelArray = archive["models"].as<JsonArrayConst>();
	if (declaredModelCount > SIZE_MAX || static_cast<size_t>(declaredModelCount) != modelArray.size() ||
	    input.bytesRead() > SIZE_MAX) {
		return backupCorrupt("legacy backup model count mismatch");
	}

	metadata.containerVersion = FreshBackupVersion;
	metadata.generatedAt = archive["generatedAt"] | 0ULL;
	metadata.totalBytes = static_cast<size_t>(input.bytesRead());
	metadata.modelCount = static_cast<size_t>(declaredModelCount);
	metadata.recordCount = 0;
	metadata.legacyFormat = true;
	metadata.models.clear();

	std::set<std::string> modelNames;
	uint64_t totalRecords = 0;
	for (JsonObjectConst modelObject : modelArray) {
		const char *name = modelObject["name"] | "";
		const char *typeName = modelObject["type"] | "";
		if (!FreshIsValidName(name) ||
		    (std::strcmp(typeName, "general") != 0 && std::strcmp(typeName, "stream") != 0) ||
		    !modelObject["recordCount"].is<uint64_t>()) {
			return backupCorrupt("legacy backup contains invalid model metadata");
		}
		if (!modelNames.insert(name).second) return backupCorrupt("legacy backup contains duplicate model");
		const FreshModelType type = FreshModelTypeFromString(typeName);
		const char *arrayName = type == FreshModelType::Stream ? "entries" : "docs";
		if (!modelObject[arrayName].is<JsonArrayConst>()) {
			return backupCorrupt("legacy backup records are missing");
		}
		const uint64_t declaredRecordCount = modelObject["recordCount"].as<uint64_t>();
		const JsonArrayConst records = modelObject[arrayName].as<JsonArrayConst>();
		if (declaredRecordCount > SIZE_MAX || static_cast<size_t>(declaredRecordCount) != records.size() ||
		    declaredRecordCount > std::numeric_limits<uint64_t>::max() - totalRecords) {
			return backupCorrupt("legacy backup record count mismatch");
		}
		totalRecords += declaredRecordCount;
		metadata.models.push_back({
		    .name = name,
		    .type = type,
		    .recordCount = static_cast<size_t>(declaredRecordCount),
		});
	}
	if (totalRecords > SIZE_MAX) return backupTooLarge("legacy backup record count exceeds platform limits");
	metadata.recordCount = static_cast<size_t>(totalRecords);

	FreshResult callbackResult = visitor.onArchiveBegin(metadata);
	if (!callbackResult) return callbackResult;

	size_t modelIndex = 0;
	for (JsonObjectConst modelObject : modelArray) {
		const FreshBackupModelMetadata &model = metadata.models[modelIndex++];
		callbackResult = visitor.onModelBegin(model);
		if (!callbackResult) return callbackResult;
		const char *arrayName = model.type == FreshModelType::Stream ? "entries" : "docs";
		const JsonArrayConst records = modelObject[arrayName].as<JsonArrayConst>();
		std::set<std::string> documentIds;
		for (JsonVariantConst entry : records) {
			JsonDocument document(&FreshJsonAllocator());
			FreshResult cloneResult = FreshCloneJson(
			    document,
			    entry,
			    model.type == FreshModelType::Stream ? "legacy backup stream entry"
			                                         : "legacy backup document"
			);
			if (!cloneResult) return cloneResult;
			FreshResult recordResult = validateDecodedRecord(
			    document,
			    model.type,
			    maxDocumentBytes,
			    documentIds
			);
			if (!recordResult) return recordResult;
			callbackResult = visitor.onRecord(model.type, std::move(document));
			if (!callbackResult) return callbackResult;
		}
		callbackResult = visitor.onModelEnd(model);
		if (!callbackResult) return callbackResult;
	}

	callbackResult = visitor.onArchiveEnd(metadata);
	if (!callbackResult) return callbackResult;
	return FreshResult::success("legacy backup archive valid", metadata.modelCount);
}

} // namespace

FreshResult FreshReadBackupArchive(
    Stream &input,
    size_t maxDocumentBytes,
    FreshBackupArchiveVisitor &visitor,
    FreshBackupMetadata &metadata
) {
	metadata = FreshBackupMetadata();
	if (maxDocumentBytes == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "document size limit is required");
	}
	uint8_t prefix[4]{};
	if (!readPrefix(input, prefix)) return backupCorrupt("truncated backup");
	PrefixStream prefixed(prefix, sizeof(prefix), input);
	return hasMagic(prefix)
	           ? readFramedArchive(prefixed, maxDocumentBytes, visitor, metadata)
	           : readLegacyArchive(prefixed, maxDocumentBytes, visitor, metadata);
}
