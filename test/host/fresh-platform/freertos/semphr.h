#pragma once
#include "FreeRTOS.h"
#include <mutex>
using SemaphoreHandle_t = std::timed_mutex *;
inline auto xSemaphoreCreateMutex() {
	return new std::timed_mutex;
}
inline void vSemaphoreDelete(SemaphoreHandle_t mutex) {
	delete mutex;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks) {
	if (ticks == portMAX_DELAY) {
		mutex->lock();
		return pdTRUE;
	}
	return mutex->try_lock_for(std::chrono::milliseconds(ticks));
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
	mutex->unlock();
	return pdTRUE;
}
