#include "Fresh.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <cctype>
#include <cstring>
#include <esp_system.h>
#include <limits>
#include <utility>

uint32_t FreshChecksum(const uint8_t *data, size_t length) {
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < length; ++i) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

void FreshWriteU16(File &file, uint16_t value) {
	file.write(static_cast<uint8_t>(value & 0xff));
	file.write(static_cast<uint8_t>((value >> 8) & 0xff));
}

void FreshWriteU32(File &file, uint32_t value) {
	file.write(static_cast<uint8_t>(value & 0xff));
	file.write(static_cast<uint8_t>((value >> 8) & 0xff));
	file.write(static_cast<uint8_t>((value >> 16) & 0xff));
	file.write(static_cast<uint8_t>((value >> 24) & 0xff));
}

void FreshWriteU64(File &file, uint64_t value) {
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		file.write(static_cast<uint8_t>((value >> shift) & 0xff));
	}
}

bool FreshReadU16(File &file, uint16_t &value) {
	if (file.available() < 2) {
		return false;
	}
	uint8_t b0 = file.read();
	uint8_t b1 = file.read();
	value = static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
	return true;
}

bool FreshReadU32(File &file, uint32_t &value) {
	if (file.available() < 4) {
		return false;
	}
	uint32_t b0 = file.read();
	uint32_t b1 = file.read();
	uint32_t b2 = file.read();
	uint32_t b3 = file.read();
	value = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
	return true;
}

bool FreshReadU64(File &file, uint64_t &value) {
	if (file.available() < 8) {
		return false;
	}
	value = 0;
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		const uint64_t byte = static_cast<uint8_t>(file.read());
		value |= byte << shift;
	}
	return true;
}

std::string FreshJoinPath(const std::string &base, const std::string &name) {
	if (base.empty() || base == "/") {
		return "/" + name;
	}
	if (base.back() == '/') {
		return base + name;
	}
	return base + "/" + name;
}

bool FreshIsValidName(const char *name) {
	if (name == nullptr || *name == '\0') {
		return false;
	}
	for (const char *cursor = name; *cursor != '\0'; ++cursor) {
		const char c = *cursor;
		if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

const char *FreshModelTypeToString(FreshModelType type) {
	return type == FreshModelType::Stream ? "stream" : "general";
}

FreshModelType FreshModelTypeFromString(const char *type) {
	if (type != nullptr && strcmp(type, "stream") == 0) {
		return FreshModelType::Stream;
	}
	return FreshModelType::General;
}

bool FreshParseJournalOp(uint8_t value, FreshJournalOp &op) {
	switch (value) {
	case static_cast<uint8_t>(FreshJournalOp::Create):
		op = FreshJournalOp::Create;
		return true;
	case static_cast<uint8_t>(FreshJournalOp::Update):
		op = FreshJournalOp::Update;
		return true;
	case static_cast<uint8_t>(FreshJournalOp::Delete):
		op = FreshJournalOp::Delete;
		return true;
	case static_cast<uint8_t>(FreshJournalOp::Append):
		op = FreshJournalOp::Append;
		return true;
	default:
		return false;
	}
}

const char *FreshJournalOpToString(FreshJournalOp op) {
	switch (op) {
	case FreshJournalOp::Create:
		return "create";
	case FreshJournalOp::Update:
		return "update";
	case FreshJournalOp::Delete:
		return "delete";
	case FreshJournalOp::Append:
		return "append";
	}
	return "unknown";
}

std::string FreshMakeId() {
	uint8_t bytes[8] = {};
	for (uint8_t &byte : bytes) {
		byte = static_cast<uint8_t>(esp_random() & 0xff);
	}

	constexpr char hex[] = "0123456789abcdef";
	std::string id;
	id.reserve(sizeof(bytes) * 2);
	for (uint8_t byte : bytes) {
		id.push_back(hex[(byte >> 4) & 0x0f]);
		id.push_back(hex[byte & 0x0f]);
	}
	return id;
}

FreshResult FreshValidateJsonDocument(const JsonDocument &document, const char *label) {
	if (document.overflowed()) {
		return FreshJsonAllocationFailure(label);
	}
	return FreshResult::success();
}

FreshResult FreshCloneJson(JsonDocument &target, JsonVariantConst source, const char *label) {
	const char *name = label != nullptr ? label : "json";
	const size_t payloadBytes = measureMsgPack(source);
	if (payloadBytes == 0) {
		target.clear();
		std::string message = name;
		message += " clone is empty";
		return FreshResult::failure(FreshStatus::InternalError, message.c_str());
	}

	FreshBuffer buffer;
	if (!buffer.allocate(payloadBytes, FreshAllocationCategory::JsonCloneBuffer)) {
		target.clear();
		std::string message = "failed to allocate ";
		message += name;
		message += " clone buffer";
		return FreshResult::failure(FreshStatus::OutOfMemory, message.c_str());
	}

	const size_t written = serializeMsgPack(source, buffer.data(), buffer.size());
	if (written != payloadBytes) {
		target.clear();
		std::string message = "failed to serialize ";
		message += name;
		message += " clone";
		return FreshResult::failure(FreshStatus::InternalError, message.c_str());
	}

	JsonDocument decoded(&FreshJsonAllocator());
	const DeserializationError error = deserializeMsgPack(
	    decoded,
	    buffer.data(),
	    buffer.size()
	);
	if (error || decoded.overflowed()) {
		target.clear();
		std::string message = "failed to deserialize ";
		message += name;
		message += " clone";
		return FreshResult::failure(
		    error == DeserializationError::NoMemory || decoded.overflowed()
		        ? FreshStatus::OutOfMemory
		        : FreshStatus::InternalError,
		    message.c_str()
		);
	}
	decoded.shrinkToFit();

	target = std::move(decoded);
	return FreshResult::success("json cloned");
}

FreshResult FreshCopyJson(JsonDocument &target, const JsonDocument &source, const char *label) {
	return FreshCloneJson(target, source.as<JsonVariantConst>(), label);
}

FreshResult FreshMergePatch(JsonDocument &target, const JsonDocument &patch) {
	if (!patch.is<JsonObjectConst>()) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "patch must be an object");
	}
	if (!target.is<JsonObject>()) {
		return FreshResult::failure(FreshStatus::InternalError, "patch target must be an object");
	}

	JsonObject targetObject = target.as<JsonObject>();
	JsonObjectConst patchObject = patch.as<JsonObjectConst>();
	for (JsonPairConst pair : patchObject) {
		const char *key = pair.key().c_str();
		if (strcmp(key, "_id") == 0 || strcmp(key, "createdAt") == 0) {
			continue;
		}
		FreshResult setResult = FreshJsonSet(targetObject[key], pair.value(), target, "patch");
		if (!setResult) {
			return setResult;
		}
	}
	return FreshValidateJsonDocument(target, "patch");
}

FreshResult FreshNextRevision(uint64_t current, uint64_t &next, const char *label) {
	if (current == std::numeric_limits<uint64_t>::max()) {
		std::string message = label != nullptr ? label : "revision";
		message += " overflow";
		return FreshResult::failure(FreshStatus::InternalError, message.c_str());
	}
	next = current + 1;
	return FreshResult::success();
}
