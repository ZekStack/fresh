#pragma once

#include <Arduino.h>
#include <strata/freertos/Mutex.h>

class FreshMutex {
  public:
	FreshMutex() noexcept : _mutex(Strata::FreeRTOS::RecursiveMutex::create()) {
	}

	FreshMutex(const FreshMutex &) = delete;
	FreshMutex &operator=(const FreshMutex &) = delete;

	bool lock(TickType_t timeout = portMAX_DELAY) {
		return _mutex.lock(timeout);
	}

	void unlock() {
		_mutex.unlock();
	}

	bool valid() const {
		return _mutex.valid();
	}

  private:
	Strata::FreeRTOS::RecursiveMutex _mutex;
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
