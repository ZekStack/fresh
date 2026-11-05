#pragma once
#include <cstdint>
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int tskNO_AFFINITY = -1;
constexpr int pdTRUE = 1;
constexpr int pdFALSE = 0;
#define pdMS_TO_TICKS(x) (x)

constexpr int configMAX_PRIORITIES = 32;
constexpr int portNUM_PROCESSORS = 2;
