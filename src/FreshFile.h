#pragma once

#include <Stream.h>

#include <cstddef>
#include <cstdio>

struct FreshResult;
class FreshStorage;

class FreshFile final : public Stream {
  public:
	FreshFile() = default;
	~FreshFile() override;

	FreshFile(const FreshFile &) = delete;
	FreshFile &operator=(const FreshFile &) = delete;

	FreshFile(FreshFile &&other) noexcept;
	FreshFile &operator=(FreshFile &&other) noexcept;

	explicit operator bool() const {
		return _file != nullptr;
	}

	int available() override;
	int read() override;
	int read(uint8_t *buffer, size_t size);
	int peek() override;

	size_t write(uint8_t byte) override;
	size_t write(const uint8_t *buffer, size_t size) override;
	void flush() override;

	bool seek(size_t position);
	size_t position() const;
	size_t size() const;
	int error() const {
		return _lastError;
	}

	FreshResult sync();
	FreshResult close();

  private:
	friend class FreshStorage;

	void attach(FILE *file, FreshStorage *storage);

	FILE *_file = nullptr;
	FreshStorage *_storage = nullptr;
	int _lastError = 0;
};
