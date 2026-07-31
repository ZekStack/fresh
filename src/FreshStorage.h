#pragma once

#include <Arduino.h>

#if defined(ESP32)
#include <driver/gpio.h>
#include <driver/spi_master.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct FreshResult;
struct FreshStorageInfo;
struct FreshFileState;
struct FreshStorageFileRegistry;
enum class FreshFileOrigin : uint8_t;
class Fresh;
class FreshFile;
class FreshFileBackend;
class FreshStorageAccess;

enum class FreshStorageType : uint8_t {
	LittleFS,
	SD,
	EMMC,
	Custom,
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
	int slot = 1;
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
	std::string path;
	bool isDirectory = false;
	size_t size = 0;
};

class FreshStorageAccess {
  public:
	FreshStorageAccess() = default;

	bool available() const;
	FreshResult open(const char *path, FreshOpenMode mode, FreshFile &file) const;
	bool exists(const char *path) const;
	FreshResult exists(const char *path, bool &result) const;
	FreshResult fileSize(const char *path, size_t &size) const;
	FreshResult ensureDirectory(const char *path) const;
	FreshResult createDirectory(const char *path) const;
	FreshResult removeFile(const char *path) const;
	FreshResult removeDirectory(const char *path) const;
	FreshResult rename(const char *source, const char *target, bool replaceExisting = false) const;
	FreshResult listDirectory(const char *path, std::vector<FreshDirectoryEntry> &entries) const;
	FreshResult writeFile(const char *path, const uint8_t *data, size_t length) const;
	FreshResult readFile(
	    const char *path,
	    uint8_t *buffer,
	    size_t capacity,
	    size_t &bytesRead
	) const;
	FreshResult info(FreshStorageInfo &result) const;
	FreshStorageInfo info() const;

  private:
	friend class Fresh;
	explicit FreshStorageAccess(Fresh *owner) : _owner(owner) {
	}

	Fresh *_owner = nullptr;
};

// Fresh owns the backend and controls mount/unmount. Application files are
// accessed through Fresh::storage(); each FreshFile owns a mutex-protected file
// state and participates in shutdown/open-handle tracking.
class FreshStorage {
  public:
	virtual ~FreshStorage();

	FreshStorage(const FreshStorage &) = delete;
	FreshStorage &operator=(const FreshStorage &) = delete;
	FreshStorage(FreshStorage &&other) noexcept;
	FreshStorage &operator=(FreshStorage &&other) = delete;

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

	size_t openFileCount() const;
	size_t applicationOpenFileCount() const;
	size_t internalOpenFileCount() const;
	size_t maxOpenFiles() const;

	FreshResult open(const char *path, FreshOpenMode mode, FreshFile &file);
	FreshResult exists(const char *path, bool &result) const;
	FreshResult createDirectory(const char *path);
	FreshResult removeFile(const char *path);
	FreshResult removeDirectory(const char *path);
	FreshResult rename(const char *source, const char *target, bool replaceExisting = false);
	FreshResult listDirectory(const char *path, std::vector<FreshDirectoryEntry> &entries) const;

	virtual const char *name() const = 0;
	virtual FreshStorageInfo info() const = 0;
	FreshResult readInfo(FreshStorageInfo &result) const;
	virtual int nativeError() const {
		return 0;
	}

  protected:
	friend class Fresh;
	friend class FreshStorageAccess;

	explicit FreshStorage(
	    FreshStorageType type,
	    const char *mountPath = nullptr,
	    size_t maxOpenFiles = SIZE_MAX
	);

	virtual FreshResult mount() = 0;
	virtual FreshResult unmount() = 0;

	virtual FreshResult readInfoBackend(FreshStorageInfo &result) const;
	virtual FreshResult openBackend(
	    const char *logicalPath,
	    FreshOpenMode mode,
	    std::unique_ptr<FreshFileBackend> &backend
	);
	virtual FreshResult existsBackend(const char *logicalPath, bool &result) const;
	virtual FreshResult createDirectoryBackend(const char *logicalPath);
	virtual FreshResult removeFileBackend(const char *logicalPath);
	virtual FreshResult removeDirectoryBackend(const char *logicalPath);
	virtual FreshResult renameBackend(
	    const char *source,
	    const char *target,
	    bool replaceExisting
	);
	virtual FreshResult listDirectoryBackend(
	    const char *logicalPath,
	    std::vector<FreshDirectoryEntry> &entries
	) const;

	FreshResult validateCanUnmount() const;
	FreshResult resolvePath(const char *logicalPath, std::string &resolvedPath) const;

	void setState(FreshStorageState state) {
		_state = state;
	}

  private:
	FreshResult normalizeLogicalPath(const char *path, std::string &normalized) const;
	FreshResult setProtectedPath(const std::string &path);
	FreshResult validatePathAccess(const std::string &path) const;
	FreshResult reserveFileHandle(FreshFileOrigin origin);
	void releaseReservedFileHandle(FreshFileOrigin origin);
	FreshResult registerFileState(const std::shared_ptr<FreshFileState> &state);
	void setApplicationFileAcceptance(bool accepted);
	FreshResult closeApplicationFiles(bool syncBeforeClose);
	FreshResult closeAllFiles(bool syncBeforeClose);

	FreshStorageType _type;
	FreshStorageState _state = FreshStorageState::Uninitialized;
	std::string _mountPath;
	std::string _protectedPath;
	std::shared_ptr<FreshStorageFileRegistry> _fileRegistry;
};
