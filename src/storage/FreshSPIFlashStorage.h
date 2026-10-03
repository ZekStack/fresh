#pragma once

#include "../FreshStorage.h"

#include <string>

#if defined(ESP32)
#include <esp_flash.h>
#include <esp_partition.h>
#include <wear_levelling.h>
#endif

struct FreshSPIFlashConfig {
	const char *mountPath = "/fresh-flash";
	const char *partitionLabel = "fresh-flash";
	size_t maxOpenFiles = 8;
	size_t allocationUnitSize = 4 * 1024;
	bool formatIfBlank = false;
	bool formatOnMountFailure = false;

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
	size_t partitionOffset = 0;
	// Zero uses the remainder of the detected flash chip after partitionOffset.
	size_t partitionSize = 0;
};

class FreshSPIFlashStorage final : public FreshStorage {
  public:
	explicit FreshSPIFlashStorage(
	    const FreshSPIFlashConfig &config = FreshSPIFlashConfig()
	);

	const char *name() const override;
	FreshStorageInfo info() const override;
	int nativeError() const override {
		return _nativeError;
	}
	uint32_t flashId() const {
		return _flashId;
	}
	uint32_t flashSizeBytes() const {
		return _flashSizeBytes;
	}

  private:
	FreshResult mount() override;
	FreshResult unmount() override;
	bool supportsFormat() const override {
		return true;
	}
	FreshResult formatBackend() override;
	FreshResult readInfoBackend(FreshStorageInfo &result) const override;

	FreshResult validateConfig() const;
	FreshResult initializeManagedSPIBus();
	FreshResult attachFlash();
	FreshResult registerPartition();
	FreshResult inspectPartitionBlank(bool &blank) const;
	FreshResult mountFilesystem();
	FreshResult releaseFilesystem();
	FreshResult releasePartition();
	FreshResult releaseFlash();
	FreshResult releaseManagedSPIBus();
	FreshResult cleanupMountFailure(const FreshResult &failure, int nativeError);

	FreshSPIFlashConfig _config;
	std::string _partitionLabel;
#if defined(ESP32)
	esp_flash_t *_flash = nullptr;
	const esp_partition_t *_partition = nullptr;
	wl_handle_t _wlHandle = WL_INVALID_HANDLE;
#endif
	bool _spiBusInitialized = false;
	bool _filesystemMounted = false;
	uint32_t _flashId = 0;
	uint32_t _flashSizeBytes = 0;
	mutable int _nativeError = 0;
};
