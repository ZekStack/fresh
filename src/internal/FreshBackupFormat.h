#pragma once

#include "../Fresh.h"
#include "FreshBuffer.h"
#include "FreshMemory.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Print.h>
#include <Stream.h>

#include <cstddef>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace fresh_backup_v2 {

constexpr uint32_t Magic = 0x4b425246; // FRBK, little-endian on the wire.
constexpr uint16_t Version = 2;
constexpr uint16_t Flags = 0;
constexpr size_t ContainerHeaderSize = 40;
constexpr size_t FrameHeaderSize = 8;
constexpr size_t FrameChecksumSize = 4;
constexpr size_t FrameOverhead = FrameHeaderSize + FrameChecksumSize;
constexpr size_t ModelBeginFixedPayloadSize = 12;
constexpr size_t ModelEndPayloadSize = 8;
constexpr size_t ArchiveEndPayloadSize = 12;

// Keep fixed metadata frames small even when parsing untrusted input.
constexpr size_t MaxMetadataPayloadSize = 64 * 1024;

enum class FrameType : uint8_t {
	ModelBegin = 1,
	Record = 2,
	ModelEnd = 3,
	ArchiveEnd = 4,
};

struct ContainerHeader {
	uint64_t generatedAt = 0;
	uint32_t modelCount = 0;
	uint64_t recordCount = 0;
	uint64_t totalBytes = 0;
};

struct FrameHeader {
	FrameType type = FrameType::ModelBegin;
	uint32_t payloadSize = 0;
	uint8_t raw[FrameHeaderSize]{};
};

inline void putU16(uint8_t *target, uint16_t value) {
	target[0] = static_cast<uint8_t>(value & 0xff);
	target[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

inline void putU32(uint8_t *target, uint32_t value) {
	target[0] = static_cast<uint8_t>(value & 0xff);
	target[1] = static_cast<uint8_t>((value >> 8) & 0xff);
	target[2] = static_cast<uint8_t>((value >> 16) & 0xff);
	target[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

inline void putU64(uint8_t *target, uint64_t value) {
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		target[shift / 8] = static_cast<uint8_t>((value >> shift) & 0xff);
	}
}

inline uint16_t getU16(const uint8_t *source) {
	return static_cast<uint16_t>(source[0]) |
	       (static_cast<uint16_t>(source[1]) << 8);
}

inline uint32_t getU32(const uint8_t *source) {
	return static_cast<uint32_t>(source[0]) |
	       (static_cast<uint32_t>(source[1]) << 8) |
	       (static_cast<uint32_t>(source[2]) << 16) |
	       (static_cast<uint32_t>(source[3]) << 24);
}

inline uint64_t getU64(const uint8_t *source) {
	uint64_t value = 0;
	for (uint8_t shift = 0; shift < 64; shift += 8) {
		value |= static_cast<uint64_t>(source[shift / 8]) << shift;
	}
	return value;
}

inline uint32_t checksumBegin() {
	return 2166136261u;
}

inline uint32_t checksumUpdate(uint32_t checksum, const uint8_t *data, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		checksum ^= data[i];
		checksum *= 16777619u;
	}
	return checksum;
}

inline uint32_t checksum(const uint8_t *data, size_t length) {
	return checksumUpdate(checksumBegin(), data, length);
}

inline bool addSize(uint64_t &total, uint64_t value) {
	if (value > std::numeric_limits<uint64_t>::max() - total) return false;
	total += value;
	return true;
}

class PrefixStream : public Stream {
  public:
	PrefixStream(const uint8_t *prefix, size_t prefixSize, Stream &input)
	    : _prefix(prefix), _prefixSize(prefixSize), _input(input) {
	}

	int available() override {
		const size_t remaining = _prefixOffset < _prefixSize ? _prefixSize - _prefixOffset : 0;
		const int inputAvailable = _input.available();
		const size_t total = remaining + (inputAvailable > 0 ? static_cast<size_t>(inputAvailable) : 0);
		return total > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(total);
	}

	int read() override {
		if (_prefixOffset < _prefixSize) return _prefix[_prefixOffset++];
		return _input.read();
	}

	int peek() override {
		if (_prefixOffset < _prefixSize) return _prefix[_prefixOffset];
		return _input.peek();
	}

	void flush() override {
		_input.flush();
	}

	size_t write(uint8_t) override {
		return 0;
	}

	using Print::write;

  private:
	const uint8_t *_prefix = nullptr;
	size_t _prefixSize = 0;
	size_t _prefixOffset = 0;
	Stream &_input;
};

class MemoryStream : public Stream {
  public:
	MemoryStream(const uint8_t *data, size_t length) : _data(data), _length(length) {
	}

	int available() override {
		const size_t remaining = _offset < _length ? _length - _offset : 0;
		return remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
	}

	int read() override {
		if (_offset >= _length) return -1;
		return _data[_offset++];
	}

	int peek() override {
		if (_offset >= _length) return -1;
		return _data[_offset];
	}

	void flush() override {
	}

	size_t write(uint8_t) override {
		return 0;
	}

	using Print::write;

  private:
	const uint8_t *_data = nullptr;
	size_t _length = 0;
	size_t _offset = 0;
};

class ChecksumPrint : public Print {
  public:
	ChecksumPrint(Print &output, uint32_t &checksum) : _output(output), _checksum(checksum) {
	}

	size_t write(uint8_t byte) override {
		if (_output.write(byte) != 1) return 0;
		_checksum = checksumUpdate(_checksum, &byte, 1);
		_written++;
		return 1;
	}

	size_t write(const uint8_t *buffer, size_t size) override {
		if (buffer == nullptr || size == 0) return 0;
		const size_t written = _output.write(buffer, size);
		if (written > 0) {
			_checksum = checksumUpdate(_checksum, buffer, written);
			_written += written;
		}
		return written;
	}

	size_t written() const {
		return _written;
	}

	using Print::write;

  private:
	Print &_output;
	uint32_t &_checksum;
	size_t _written = 0;
};

class Writer {
  public:
	explicit Writer(Print &output) : _output(output) {
	}

	bool writeContainerHeader(const ContainerHeader &header) {
		uint8_t raw[ContainerHeaderSize]{};
		putU32(raw, Magic);
		putU16(raw + 4, Version);
		putU16(raw + 6, Flags);
		putU64(raw + 8, header.generatedAt);
		putU32(raw + 16, header.modelCount);
		putU64(raw + 20, header.recordCount);
		putU64(raw + 28, header.totalBytes);
		putU32(raw + 36, checksum(raw, 36));
		return writeRaw(raw, sizeof(raw));
	}

	bool writeModelBegin(const std::string &name, FreshModelType type, uint64_t recordCount) {
		if (name.empty() || name.size() > UINT16_MAX) return false;
		const uint32_t payloadSize = static_cast<uint32_t>(ModelBeginFixedPayloadSize + name.size());
		uint32_t frameChecksum = 0;
		if (!beginFrame(FrameType::ModelBegin, payloadSize, frameChecksum)) return false;
		uint8_t fixed[ModelBeginFixedPayloadSize]{};
		fixed[0] = type == FreshModelType::Stream ? 1 : 0;
		putU16(fixed + 2, static_cast<uint16_t>(name.size()));
		putU64(fixed + 4, recordCount);
		if (!writePayload(fixed, sizeof(fixed), frameChecksum) ||
		    !writePayload(reinterpret_cast<const uint8_t *>(name.data()), name.size(), frameChecksum)) {
			return false;
		}
		return endFrame(frameChecksum);
	}

	bool writeRecord(const JsonDocument &document, size_t payloadSize) {
		if (payloadSize == 0 || payloadSize > UINT32_MAX) return false;
		uint32_t frameChecksum = 0;
		if (!beginFrame(FrameType::Record, static_cast<uint32_t>(payloadSize), frameChecksum)) return false;
		ChecksumPrint payload(_output, frameChecksum);
		const size_t written = serializeMsgPack(document, payload);
		_bytesWritten += written;
		if (written != payloadSize || payload.written() != payloadSize) return false;
		return endFrame(frameChecksum);
	}

	bool writeModelEnd(uint64_t recordCount) {
		uint8_t payload[ModelEndPayloadSize]{};
		putU64(payload, recordCount);
		return writeFixedFrame(FrameType::ModelEnd, payload, sizeof(payload));
	}

	bool writeArchiveEnd(uint32_t modelCount, uint64_t recordCount) {
		uint8_t payload[ArchiveEndPayloadSize]{};
		putU32(payload, modelCount);
		putU64(payload + 4, recordCount);
		return writeFixedFrame(FrameType::ArchiveEnd, payload, sizeof(payload));
	}

	uint64_t bytesWritten() const {
		return _bytesWritten;
	}

  private:
	bool writeRaw(const uint8_t *data, size_t length) {
		if (length == 0) return true;
		const size_t written = _output.write(data, length);
		_bytesWritten += written;
		return written == length;
	}

	bool beginFrame(FrameType type, uint32_t payloadSize, uint32_t &frameChecksum) {
		uint8_t header[FrameHeaderSize]{};
		header[0] = static_cast<uint8_t>(type);
		putU32(header + 4, payloadSize);
		frameChecksum = checksum(header, sizeof(header));
		return writeRaw(header, sizeof(header));
	}

	bool writePayload(const uint8_t *data, size_t length, uint32_t &frameChecksum) {
		if (length == 0) return true;
		const size_t written = _output.write(data, length);
		_bytesWritten += written;
		if (written > 0) frameChecksum = checksumUpdate(frameChecksum, data, written);
		return written == length;
	}

	bool endFrame(uint32_t frameChecksum) {
		uint8_t trailer[FrameChecksumSize]{};
		putU32(trailer, frameChecksum);
		return writeRaw(trailer, sizeof(trailer));
	}

	bool writeFixedFrame(FrameType type, const uint8_t *payload, size_t payloadSize) {
		if (payloadSize > UINT32_MAX) return false;
		uint32_t frameChecksum = 0;
		if (!beginFrame(type, static_cast<uint32_t>(payloadSize), frameChecksum)) return false;
		if (!writePayload(payload, payloadSize, frameChecksum)) return false;
		return endFrame(frameChecksum);
	}

	Print &_output;
	uint64_t _bytesWritten = 0;
};

class Reader {
  public:
	explicit Reader(Stream &input) : _input(input) {
	}

	bool readContainerHeader(ContainerHeader &header, const char *&error) {
		uint8_t raw[ContainerHeaderSize]{};
		if (!readRaw(raw, sizeof(raw))) {
			error = "truncated backup header";
			return false;
		}
		if (getU32(raw) != Magic || getU16(raw + 4) != Version || getU16(raw + 6) != Flags) {
			error = "unsupported backup container";
			return false;
		}
		if (getU32(raw + 36) != checksum(raw, 36)) {
			error = "backup header checksum mismatch";
			return false;
		}
		header.generatedAt = getU64(raw + 8);
		header.modelCount = getU32(raw + 16);
		header.recordCount = getU64(raw + 20);
		header.totalBytes = getU64(raw + 28);
		if (header.totalBytes < ContainerHeaderSize + FrameOverhead + ArchiveEndPayloadSize) {
			error = "invalid backup byte count";
			return false;
		}
		return true;
	}

	bool readFrameHeader(FrameHeader &header, const char *&error) {
		if (!readRaw(header.raw, sizeof(header.raw))) {
			error = "truncated backup frame header";
			return false;
		}
		if (header.raw[1] != 0 || getU16(header.raw + 2) != 0) {
			error = "unsupported backup frame flags";
			return false;
		}
		switch (header.raw[0]) {
		case static_cast<uint8_t>(FrameType::ModelBegin):
		case static_cast<uint8_t>(FrameType::Record):
		case static_cast<uint8_t>(FrameType::ModelEnd):
		case static_cast<uint8_t>(FrameType::ArchiveEnd):
			header.type = static_cast<FrameType>(header.raw[0]);
			break;
		default:
			error = "unknown backup frame type";
			return false;
		}
		header.payloadSize = getU32(header.raw + 4);
		return true;
	}

	bool readFramePayload(const FrameHeader &header, FreshBuffer &payload, const char *&error) {
		if (!payload.allocate(header.payloadSize, FreshAllocationCategory::BackupBuffer)) {
			error = "failed to allocate backup frame";
			return false;
		}
		if (header.payloadSize > 0 && !readRaw(payload.data(), payload.size())) {
			error = "truncated backup frame payload";
			return false;
		}
		uint8_t trailer[FrameChecksumSize]{};
		if (!readRaw(trailer, sizeof(trailer))) {
			error = "truncated backup frame checksum";
			return false;
		}
		uint32_t actual = checksum(header.raw, sizeof(header.raw));
		actual = checksumUpdate(actual, payload.data(), payload.size());
		if (getU32(trailer) != actual) {
			error = "backup frame checksum mismatch";
			return false;
		}
		return true;
	}

	uint64_t bytesRead() const {
		return _bytesRead;
	}

  private:
	bool readRaw(uint8_t *target, size_t length) {
		if (length == 0) return true;
		const size_t read = _input.readBytes(reinterpret_cast<char *>(target), length);
		_bytesRead += read;
		return read == length;
	}

	Stream &_input;
	uint64_t _bytesRead = 0;
};

inline uint64_t frameBytes(uint64_t payloadBytes) {
	return static_cast<uint64_t>(FrameOverhead) + payloadBytes;
}

inline bool hasMagic(const uint8_t prefix[4]) {
	return getU32(prefix) == Magic;
}

} // namespace fresh_backup_v2
