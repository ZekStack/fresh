#pragma once
#include "Strata.h"
#include "freertos/task.h"
namespace Strata::FreeRTOS {
struct TaskConfig {
	const char *name;
	uint32_t stackBytes;
	Placement stackPlacement;
	UBaseType_t priority;
	int32_t affinity;
};
class Task {
	std::shared_ptr<HostTask> value;

  public:
	static Task create(void (*fn)(void *), void *arg, const TaskConfig &) {
		Task result;
		result.value = std::make_shared<HostTask>();
		auto state = result.value;
		state->worker = std::thread([state, fn, arg] {
			hostCurrentTask = state.get();
			fn(arg);
		});
		return result;
	}
	bool valid() const {
		return !!value;
	}
	TaskHandle_t handle() const {
		return value.get();
	}
	Region stackRegion() const {
		return Region::Internal;
	}
	size_t stackHighWaterMarkBytes() const {
		return 0;
	}
	void reset() {
		if (value && value->worker.joinable())
			value->worker.join();
		value.reset();
	}
};
} // namespace Strata::FreeRTOS
