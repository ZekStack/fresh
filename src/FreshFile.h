#pragma once

#include <Stream.h>

#include <cstddef>
#include <cstdint>
#include <memory>

struct FreshResult;
class FreshStorage;

class FreshFileBackend {
  public:
	virtual ~FreshFileBackend() = default;

	FreshFileBackend(const FreshFileBackend &) = delete;
	FreshFileBackend &operator=(const FreshFileBackend &) = delete;

	virtual bool isOpen() const = 0;
	virtual int available() = 0;
	virtual int read() = 0;
	virtual int read(uint8_t *buffer, size_t size) = 0;
	virtual int peek() = 0;
	virtual size_t write(uint8_t byte) = 0;
	virtual size_t write(const uint8_t *buffer, size_t size) = 0;
	virtual bool seek(size_t position) = 0;
	virtual size_t position() const = 0;
	virtual size_t size() const = 0;
	virtual FreshResult sync() = 0;
	virtual FreshResult close() = 0;
	virtual int error() const = 0;

  protected:
	FreshFileBackend() = default;
};

class FreshFile final : public Stream {
  public:
	FreshFile() = default;
	~FreshFile() override;

	FreshFile(const FreshFile &) = delete;
	FreshFile &operator=(const FreshFile &) = delete;

	FreshFile(FreshFile &&other) noexcept;
	FreshFile &operator=(FreshFile &&other) noexcept;

	explicit operator bool() const;

	int available() override;
	int read() override;
	int read(uint8_t *buffer, size_t size) override;
	int peek() override;

	size_t write(uint8_t byte) override;
	size_t write(const uint8_t *buffer, size_t size) override;
	void flush() override;

	bool seek(size_t position);
	size_t position() const;
	size_t size() const;
	int error() const;

	FreshResult sync();
	FreshResult close();

  private:
	friend class FreshStorage;

	void attach(std::unique_ptr<FreshFileBackend> backend, FreshStorage *storage);

	std::unique_ptr<FreshFileBackend> _backend;
	FreshStorage *_storage = nullptr;
};
