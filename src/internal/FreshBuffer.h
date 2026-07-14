#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cstdlib>
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

		_data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
		if (_data != nullptr) {
			_size = size;
			_allocation = Allocation::HeapCaps;
			return true;
		}

		_data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
		if (_data != nullptr) {
			_size = size;
			_allocation = Allocation::HeapCaps;
			return true;
		}

		_data = static_cast<uint8_t *>(malloc(size));
		if (_data != nullptr) {
			_size = size;
			_allocation = Allocation::Malloc;
			return true;
		}

		return false;
	}

	void reset() {
		if (_data != nullptr) {
			if (_allocation == Allocation::Malloc) {
				free(_data);
			} else {
				heap_caps_free(_data);
			}
		}
		_data = nullptr;
		_size = 0;
		_allocation = Allocation::None;
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
	enum class Allocation : uint8_t {
		None,
		HeapCaps,
		Malloc,
	};

	void moveFrom(FreshBuffer &&other) {
		_data = other._data;
		_size = other._size;
		_allocation = other._allocation;
		other._data = nullptr;
		other._size = 0;
		other._allocation = Allocation::None;
	}

	uint8_t *_data = nullptr;
	size_t _size = 0;
	Allocation _allocation = Allocation::None;
};
