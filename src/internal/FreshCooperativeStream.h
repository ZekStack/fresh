#pragma once

#include <Stream.h>

#include <cstddef>
#include <cstdint>

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

class FreshCooperativeStream : public Stream {
  public:
	explicit FreshCooperativeStream(
	    Stream &input,
	    size_t yieldEveryBytes = 32 * 1024,
	    uint32_t delayTicks = 1
	)
	    : _input(input),
	      _yieldEveryBytes(yieldEveryBytes),
	      _delayTicks(delayTicks) {
	}

	int available() override {
		return _input.available();
	}

	int read() override {
		const int value = _input.read();
		if (value >= 0) account(1);
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

	using Print::write;

  private:
	void account(size_t bytes) {
		if (_yieldEveryBytes == 0 || bytes == 0) return;
		_bytesSinceYield += bytes;
		if (_bytesSinceYield < _yieldEveryBytes) return;
		_bytesSinceYield %= _yieldEveryBytes;
#if defined(ESP32)
		const TickType_t ticks = _delayTicks == 0
		                           ? static_cast<TickType_t>(1)
		                           : static_cast<TickType_t>(_delayTicks);
		vTaskDelay(ticks);
#endif
	}

	Stream &_input;
	size_t _yieldEveryBytes;
	uint32_t _delayTicks;
	size_t _bytesSinceYield = 0;
};
