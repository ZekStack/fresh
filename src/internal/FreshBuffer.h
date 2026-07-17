#pragma once

#include "FreshMemory.h"

#include <Arduino.h>

#include <utility>

class FreshBuffer {
  public:
	FreshBuffer() = default;

	explicit FreshBuffer(size_t size) {
		allocate(size);
	}

	~FreshBuffer() {
		reset();
	}

	FreshBuffer(const FreshBuffer &) = delete;
	FreshBuffer &operator=(const FreshBuffer &) = delete;

	FreshBuffer(FreshBuffer &&other) noexcept {
		moveFrom(std::move(other));
	}

	FreshBuffer &operator=(FreshBuffer &&other) noexcept {
		if (this != &other) {
			reset();
			moveFrom(std::move(other));
		}
		return *this;
	}

	bool allocate(size_t size) {
		reset();
		if (size == 0) {
			return true;
		}

		_data = static_cast<uint8_t *>(FreshAllocate(size));
		if (_data == nullptr) {
			return false;
		}

		_size = size;
		return true;
	}

	void reset() {
		FreshDeallocate(_data);
		_data = nullptr;
		_size = 0;
	}

	uint8_t *data() {
		return _data;
	}

	const uint8_t *data() const {
		return _data;
	}

	size_t size() const {
		return _size;
	}

	bool empty() const {
		return _size == 0;
	}

	uint8_t &operator[](size_t index) {
		return _data[index];
	}

	const uint8_t &operator[](size_t index) const {
		return _data[index];
	}

  private:
	void moveFrom(FreshBuffer &&other) {
		_data = other._data;
		_size = other._size;
		other._data = nullptr;
		other._size = 0;
	}

	uint8_t *_data = nullptr;
	size_t _size = 0;
};
