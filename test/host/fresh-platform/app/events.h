#pragma once
#include <Signal.h>
enum class AppEvent {
	ControllerStateChanged,
	ControllersChanged,
	DevicePresenceStateChanged,
	DevicePresenceChanged,
	HolidaysChanged,
	WeatherChanged
};
struct AppControllerStateChangedPayload {};
struct AppControllersChangedPayload {};
struct AppDevicePresenceStateChangedPayload {};
struct AppDevicePresenceChangedPayload {};
struct AppHolidaysChangedPayload {};
struct AppWeatherChangedPayload {};
