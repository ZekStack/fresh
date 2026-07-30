#include "FreshSDStorage.h"

#include "../Fresh.h"

#if defined(ESP32)
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <soc/soc_caps.h>
#endif

#include <climits>
#include <limits>

FreshSDStorage::FreshSDStorage(const FreshSDConfig &config)
    : FreshStorage(FreshStorageType::SD, config.mountPath, config.maxOpenFiles), _config(config) {
}

const char *FreshSDStorage::name() const {
	return _config.interface == FreshSDInterface::SPI ? "SDSPI" : "SDMMC";
}

FreshResult FreshSDStorage::mount() {
	if (isMounted()) return FreshResult::success("SD storage already mounted");
	if (*mountPath() == '\0' || mountPath()[0] != '/') {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD mount path is invalid");
	}
	if (_config.maxOpenFiles == 0 || _config.maxOpenFiles > static_cast<size_t>(INT_MAX)) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD max open files is out of range");
	}
	if (_config.allocationUnitSize == 0 ||
	    _config.allocationUnitSize > static_cast<size_t>(128 * 1024)) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD allocation unit size is out of range");
	}

	setState(FreshStorageState::Mounting);
	FreshResult result = _config.interface == FreshSDInterface::SPI ? mountSPI() : mountSDMMC();
	setState(result ? FreshStorageState::Mounted : FreshStorageState::Error);
	return result;
}

FreshResult FreshSDStorage::mountSPI() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SDSPI requires ESP32");
#else
	if (_config.spi.chipSelectPin == GPIO_NUM_NC) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SDSPI chip select pin is required");
	}
	if (_config.spi.frequencyHz == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SDSPI frequency must be greater than zero");
	}

	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.slot = static_cast<int>(_config.spi.host);
	host.max_freq_khz = static_cast<int>(_config.spi.frequencyHz / 1000);
	if (host.max_freq_khz <= 0) host.max_freq_khz = 1;

	if (_config.spi.busOwnership == FreshSPIBusOwnership::Managed) {
		if (_config.spi.clockPin == GPIO_NUM_NC || _config.spi.mosiPin == GPIO_NUM_NC ||
		    _config.spi.misoPin == GPIO_NUM_NC) {
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "managed SDSPI requires clock, MOSI, and MISO pins"
			);
		}
		spi_bus_config_t busConfig = {};
		busConfig.mosi_io_num = _config.spi.mosiPin;
		busConfig.miso_io_num = _config.spi.misoPin;
		busConfig.sclk_io_num = _config.spi.clockPin;
		busConfig.quadwp_io_num = -1;
		busConfig.quadhd_io_num = -1;
		busConfig.max_transfer_sz = static_cast<int>(_config.allocationUnitSize);

		esp_err_t initialized =
		    spi_bus_initialize(_config.spi.host, &busConfig, SPI_DMA_CH_AUTO);
		_nativeError = static_cast<int>(initialized);
		if (initialized != ESP_OK) {
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to initialize SDSPI bus");
		}
		_spiBusInitialized = true;
	}

	sdspi_device_config_t deviceConfig = SDSPI_DEVICE_CONFIG_DEFAULT();
	deviceConfig.host_id = _config.spi.host;
	deviceConfig.gpio_cs = _config.spi.chipSelectPin;

	esp_vfs_fat_sdmmc_mount_config_t mountConfig = {};
	mountConfig.format_if_mount_failed = _config.formatOnMountFailure;
	mountConfig.max_files = static_cast<int>(_config.maxOpenFiles);
	mountConfig.allocation_unit_size = _config.allocationUnitSize;

	esp_err_t mounted = esp_vfs_fat_sdspi_mount(
	    mountPath(),
	    &host,
	    &deviceConfig,
	    &mountConfig,
	    &_card
	);
	_nativeError = static_cast<int>(mounted);
	if (mounted != ESP_OK) {
		_card = nullptr;
		FreshResult released = releaseManagedSPIBus();
		if (!released) return released;
		return FreshResult::failure(FreshStatus::StorageUnavailable, "failed to mount SDSPI storage");
	}
	return FreshResult::success("SDSPI storage mounted");
#endif
}

FreshResult FreshSDStorage::mountSDMMC() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SDMMC requires ESP32");
#elif !defined(SOC_SDMMC_HOST_SUPPORTED) || !SOC_SDMMC_HOST_SUPPORTED
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "target does not support SDMMC host");
#else
	if (_config.sdmmc.slot < 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SDMMC slot is invalid");
	}

	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	host.slot = _config.sdmmc.slot;

	sdmmc_slot_config_t slotConfig = SDMMC_SLOT_CONFIG_DEFAULT();
	slotConfig.width = _config.sdmmc.oneBitMode ? 1 : 4;

	const bool customPins = _config.sdmmc.clockPin != GPIO_NUM_NC ||
	                        _config.sdmmc.commandPin != GPIO_NUM_NC ||
	                        _config.sdmmc.data0Pin != GPIO_NUM_NC ||
	                        _config.sdmmc.data1Pin != GPIO_NUM_NC ||
	                        _config.sdmmc.data2Pin != GPIO_NUM_NC ||
	                        _config.sdmmc.data3Pin != GPIO_NUM_NC;
	if (customPins) {
#if defined(SOC_SDMMC_USE_GPIO_MATRIX) && SOC_SDMMC_USE_GPIO_MATRIX
		if (_config.sdmmc.clockPin == GPIO_NUM_NC || _config.sdmmc.commandPin == GPIO_NUM_NC ||
		    _config.sdmmc.data0Pin == GPIO_NUM_NC) {
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "custom SDMMC pins require clock, command, and data0"
			);
		}
		if (!_config.sdmmc.oneBitMode &&
		    (_config.sdmmc.data1Pin == GPIO_NUM_NC || _config.sdmmc.data2Pin == GPIO_NUM_NC ||
		     _config.sdmmc.data3Pin == GPIO_NUM_NC)) {
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "four-bit SDMMC requires data1, data2, and data3"
			);
		}
		slotConfig.clk = _config.sdmmc.clockPin;
		slotConfig.cmd = _config.sdmmc.commandPin;
		slotConfig.d0 = _config.sdmmc.data0Pin;
		if (!_config.sdmmc.oneBitMode) {
			slotConfig.d1 = _config.sdmmc.data1Pin;
			slotConfig.d2 = _config.sdmmc.data2Pin;
			slotConfig.d3 = _config.sdmmc.data3Pin;
		}
#else
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "target SDMMC host does not support custom GPIO routing"
		);
#endif
	}

	esp_vfs_fat_sdmmc_mount_config_t mountConfig = {};
	mountConfig.format_if_mount_failed = _config.formatOnMountFailure;
	mountConfig.max_files = static_cast<int>(_config.maxOpenFiles);
	mountConfig.allocation_unit_size = _config.allocationUnitSize;

	esp_err_t mounted = esp_vfs_fat_sdmmc_mount(
	    mountPath(),
	    &host,
	    &slotConfig,
	    &mountConfig,
	    &_card
	);
	_nativeError = static_cast<int>(mounted);
	if (mounted != ESP_OK) {
		_card = nullptr;
		return FreshResult::failure(FreshStatus::StorageUnavailable, "failed to mount SDMMC storage");
	}
	return FreshResult::success("SDMMC storage mounted");
#endif
}

FreshResult FreshSDStorage::unmount() {
	FreshResult canUnmount = validateCanUnmount();
	if (!canUnmount) return canUnmount;

#if !defined(ESP32)
	setState(FreshStorageState::Error);
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SD storage requires ESP32");
#else
	if (_card != nullptr) {
		setState(FreshStorageState::Unmounting);
		esp_err_t unmounted = esp_vfs_fat_sdcard_unmount(mountPath(), _card);
		_nativeError = static_cast<int>(unmounted);
		if (unmounted != ESP_OK) {
			setState(FreshStorageState::Error);
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to unmount SD storage");
		}
		_card = nullptr;
	}

	FreshResult released = releaseManagedSPIBus();
	if (!released) {
		setState(FreshStorageState::Error);
		return released;
	}

	setState(FreshStorageState::Uninitialized);
	_nativeError = 0;
	return FreshResult::success("SD storage unmounted");
#endif
}

FreshResult FreshSDStorage::releaseManagedSPIBus() {
#if !defined(ESP32)
	return FreshResult::success("SDSPI bus not configured");
#else
	if (_config.interface != FreshSDInterface::SPI ||
	    _config.spi.busOwnership != FreshSPIBusOwnership::Managed || !_spiBusInitialized) {
		return FreshResult::success("SDSPI bus not owned");
	}
	esp_err_t released = spi_bus_free(_config.spi.host);
	_nativeError = static_cast<int>(released);
	if (released != ESP_OK) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to release SDSPI bus");
	}
	_spiBusInitialized = false;
	return FreshResult::success("SDSPI bus released");
#endif
}

FreshStorageInfo FreshSDStorage::info() const {
	FreshStorageInfo result;
	readInfoBackend(result);
	return result;
}

FreshResult FreshSDStorage::readInfoBackend(FreshStorageInfo &result) const {
	result = FreshStorageInfo();
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SD storage requires ESP32");
#else
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::StorageUnavailable, "SD storage is not mounted");
	}
	uint64_t totalBytes = 0;
	uint64_t freeBytes = 0;
	const esp_err_t queried = esp_vfs_fat_info(mountPath(), &totalBytes, &freeBytes);
	_nativeError = static_cast<int>(queried);
	if (queried != ESP_OK) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to query SD storage");
	}
	const uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
	result.totalBytes = static_cast<size_t>(totalBytes > maxSize ? maxSize : totalBytes);
	result.freeBytes = static_cast<size_t>(freeBytes > maxSize ? maxSize : freeBytes);
	result.usedBytes = result.totalBytes > result.freeBytes ? result.totalBytes - result.freeBytes : 0;
	return FreshResult::success("SD storage information read");
#endif
}
