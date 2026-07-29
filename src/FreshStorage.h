#pragma once

#include <Arduino.h>

#if defined(ESP32)
#include <driver/gpio.h>
#include <driver/spi_master.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct FreshResult;
struct FreshStorageInfo;
class Fresh;
class FreshFile;

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

enum class FreshOpenMode : uint8_t {
	Read,
	Write,
	Append,
};

struct FreshLittleFSConfig {
	const char *partitionLabel = "spiffs";
	// Keep the Arduino LittleFS default mount point for 0.1.x compatibility.
	const char *mountPath = "/littlefs";
	size_t maxOpenFiles = 10;
	bool formatOnMountFailure = false;
	bool growOnMount = true;
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
	size_t allocationUnitSize = 16 * 1024;
	bool formatOnMountFailure = false;
	FreshSDSPIConfig spi;
	FreshSDMMCConfig sdmmc;
};

struct FreshDirectoryEntry {
	std::string name;
	bool isDirectory = false;
	size_t size = 0;
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

	const char *mountPath() const {
		return _mountPath.c_str();
	}

	size_t openFileCount() const {
		return _openFileCount.load();
	}

	FreshResult open(const char *path, FreshOpenMode mode, FreshFile &file);
	FreshResult exists(const char *path, bool &result) const;
	FreshResult createDirectory(const char *path);
	FreshResult removeFile(const char *path);
	FreshResult removeDirectory(const char *path);
	FreshResult listDirectory(const char *path, std::vector<FreshDirectoryEntry> &entries) const;

	virtual const char *name() const = 0;
	virtual FreshStorageInfo info() const = 0;

  protected:
	friend class Fresh;
	friend class FreshFile;

	FreshStorage(FreshStorageType type, const char *mountPath);

	virtual FreshResult mount() = 0;
	virtual FreshResult unmount() = 0;

	FreshResult validateCanUnmount() const;
	FreshResult resolvePath(const char *logicalPath, std::string &resolvedPath) const;

	void setState(FreshStorageState state) {
		_state = state;
	}

  private:
	void releaseFileHandle();

	FreshStorageType _type;
	FreshStorageState _state = FreshStorageState::Uninitialized;
	std::string _mountPath;
	std::atomic<size_t> _openFileCount{0};
};
