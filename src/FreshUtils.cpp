#include "Fresh.h"
#include "internal/FreshInternal.h"

#include <cctype>
#include <cstring>
#include <esp_system.h>

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
		if (!(isalnum(c) || c == '_' || c == '-')) {
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

void FreshCopyJson(JsonDocument &target, const JsonDocument &source) {
	target.clear();
	target.set(source.as<JsonVariantConst>());
}

void FreshMergePatch(JsonDocument &target, const JsonDocument &patch) {
	JsonObject targetObject = target.as<JsonObject>();
	JsonObjectConst patchObject = patch.as<JsonObjectConst>();
	for (JsonPairConst pair : patchObject) {
		const char *key = pair.key().c_str();
		if (strcmp(key, "_id") == 0 || strcmp(key, "createdAt") == 0) {
			continue;
		}
		targetObject[key].set(pair.value());
	}
}
