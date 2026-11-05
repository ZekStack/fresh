#pragma once
#include "freertos/FreeRTOS.h"
#include <condition_variable>
#include <memory>
#include <mutex>
namespace Strata::FreeRTOS {
class BinarySemaphore {
	struct State {
		std::mutex mutex;
		std::condition_variable cv;
		bool ready = false;
	};
	std::shared_ptr<State> value;

  public:
	static BinarySemaphore create() {
		BinarySemaphore result;
		result.value = std::make_shared<State>();
		return result;
	}
	bool valid() const {
		return !!value;
	}
	bool give() {
		std::lock_guard lock(value->mutex);
		value->ready = true;
		value->cv.notify_all();
		return true;
	}
	bool tryTake() {
		return take(0);
	}
	bool take(TickType_t ticks = portMAX_DELAY) {
		std::unique_lock lock(value->mutex);
		if (ticks == portMAX_DELAY)
			value->cv.wait(lock, [&] { return value->ready; });
		else if (!value->cv.wait_for(lock, std::chrono::milliseconds(ticks), [&] {
			         return value->ready;
		         }))
			return false;
		value->ready = false;
		return true;
	}
};
} // namespace Strata::FreeRTOS
