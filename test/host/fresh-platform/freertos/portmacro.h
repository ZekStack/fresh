#pragma once
#include <mutex>
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED                                                               \
	{}
#define portENTER_CRITICAL(x) (x)->lock()
#define portEXIT_CRITICAL(x) (x)->unlock()
