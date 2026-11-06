#pragma once
#include "freertos/FreeRTOS.h"
#include <memory>
#include <mutex>
namespace Strata::FreeRTOS {
class RecursiveMutex {
	std::shared_ptr<std::recursive_timed_mutex> value;

  public:
	static RecursiveMutex create() {
		RecursiveMutex result;
		result.value = std::make_shared<std::recursive_timed_mutex>();
		return result;
	}
	bool valid() const {
		return !!value;
	}
	bool lock(TickType_t ticks) {
		if (!value)
			return false;
		if (ticks == portMAX_DELAY) {
			value->lock();
			return true;
		}
		return value->try_lock_for(std::chrono::milliseconds(ticks));
	}
	void unlock() {
		value->unlock();
	}
};
} // namespace Strata::FreeRTOS
