#pragma once

#include <Arduino.h>

#if defined(ESP32)
#include <driver/gpio.h>
#include <driver/spi_master.h>
#endif

#include <cstddef>
#include <cstdint>

struct FreshResult;
struct FreshStorageInfo;
class Fresh;

enum class FreshStorageType : uint8_t {
	LittleFS,
	SD,
};

enum class FreshStorageState : uint8_t {
	Uninitialized,
	Mounting,
	Mounted,
	Unmounting,
	Error,
};

enum class FreshSDInterface : uint8_t {
	SPI,
	SDMMC,
};

enum class FreshSPIBusOwnership : uint8_t {
	Managed,
	External,
};

struct FreshLittleFSConfig {
	const char *partitionLabel = "spiffs";
	const char *mountPath = "/fresh-littlefs";
	size_t maxOpenFiles = 10;
	bool formatOnMountFailure = false;
};

struct FreshSDSPIConfig {
#if defined(ESP32)
	spi_host_device_t host = SPI2_HOST;
	gpio_num_t chipSelectPin = GPIO_NUM_NC;
	gpio_num_t clockPin = GPIO_NUM_NC;
	gpio_num_t mosiPin = GPIO_NUM_NC;
	gpio_num_t misoPin = GPIO_NUM_NC;
#else
	int host = 0;
	int chipSelectPin = -1;
	int clockPin = -1;
	int mosiPin = -1;
	int misoPin = -1;
#endif
	uint32_t frequencyHz = 20'000'000;
	FreshSPIBusOwnership busOwnership = FreshSPIBusOwnership::Managed;
};

struct FreshSDMMCConfig {
	int slot = 0;
	bool oneBitMode = false;

#if defined(ESP32)
	gpio_num_t clockPin = GPIO_NUM_NC;
	gpio_num_t commandPin = GPIO_NUM_NC;
	gpio_num_t data0Pin = GPIO_NUM_NC;
	gpio_num_t data1Pin = GPIO_NUM_NC;
	gpio_num_t data2Pin = GPIO_NUM_NC;
	gpio_num_t data3Pin = GPIO_NUM_NC;
#else
	int clockPin = -1;
	int commandPin = -1;
	int data0Pin = -1;
	int data1Pin = -1;
	int data2Pin = -1;
	int data3Pin = -1;
#endif
};

struct FreshSDConfig {
	FreshSDInterface interface = FreshSDInterface::SPI;
	const char *mountPath = "/fresh-sd";
	size_t maxOpenFiles = 8;
	bool formatOnMountFailure = false;
	FreshSDSPIConfig spi;
	FreshSDMMCConfig sdmmc;
};

class FreshStorage {
  public:
	virtual ~FreshStorage() = default;

	FreshStorage(const FreshStorage &) = delete;
	FreshStorage &operator=(const FreshStorage &) = delete;

	FreshStorageType type() const {
		return _type;
	}

	FreshStorageState state() const {
		return _state;
	}

	bool isMounted() const {
		return _state == FreshStorageState::Mounted;
	}

	virtual const char *name() const = 0;
	virtual FreshStorageInfo info() const = 0;

  protected:
	friend class Fresh;

	explicit FreshStorage(FreshStorageType type) : _type(type) {
	}

	virtual FreshResult mount() = 0;
	virtual FreshResult unmount() = 0;

	void setState(FreshStorageState state) {
		_state = state;
	}

  private:
	FreshStorageType _type;
	FreshStorageState _state = FreshStorageState::Uninitialized;
};
