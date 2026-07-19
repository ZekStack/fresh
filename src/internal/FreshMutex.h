#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class FreshMutex {
  public:
	FreshMutex() {
		_handle = xSemaphoreCreateRecursiveMutex();
	}

	~FreshMutex() {
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
		}
	}

	FreshMutex(const FreshMutex &) = delete;
	FreshMutex &operator=(const FreshMutex &) = delete;

	bool lock(TickType_t timeout = portMAX_DELAY) {
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, timeout) == pdTRUE;
	}

	void unlock() {
		if (_handle != nullptr) {
			xSemaphoreGiveRecursive(_handle);
		}
	}

  private:
	SemaphoreHandle_t _handle = nullptr;
};

class FreshLock {
  public:
	explicit FreshLock(FreshMutex &mutex, TickType_t timeout = portMAX_DELAY)
	    : _mutex(mutex), _locked(mutex.lock(timeout)) {
	}

	~FreshLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	FreshLock(const FreshLock &) = delete;
	FreshLock &operator=(const FreshLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	FreshMutex &_mutex;
	bool _locked = false;
};
