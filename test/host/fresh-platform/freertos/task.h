#pragma once
#include "FreeRTOS.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
struct HostTask {
	std::mutex mutex;
	std::condition_variable cv;
	unsigned notifications = 0;
	std::thread worker;
};
using TaskHandle_t = HostTask *;
inline thread_local HostTask *hostCurrentTask = nullptr;
inline TaskHandle_t xTaskGetCurrentTaskHandle() {
	return hostCurrentTask;
}
inline void xTaskNotifyGive(TaskHandle_t task) {
	if (task) {
		std::lock_guard lock(task->mutex);
		++task->notifications;
		task->cv.notify_all();
	}
}
inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
	auto task = hostCurrentTask;
	if (!task)
		return 0;
	std::unique_lock lock(task->mutex);
	task->cv.wait_for(lock, std::chrono::milliseconds(ticks), [&] {
		return task->notifications > 0;
	});
	auto count = task->notifications;
	if (clear)
		task->notifications = 0;
	else if (count)
		--task->notifications;
	return count;
}
inline void vTaskSuspend(TaskHandle_t) {
}
inline void vTaskDelay(TickType_t ticks) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
