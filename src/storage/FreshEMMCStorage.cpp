#include "FreshEMMCStorage.h"

#include "../Fresh.h"

#if defined(ESP32)
#include <driver/sdmmc_host.h>
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <soc/soc_caps.h>
#endif

#include <climits>
#include <limits>

FreshEMMCStorage::FreshEMMCStorage(const FreshEMMCConfig &config)
    : FreshStorage(FreshStorageType::EMMC, config.mountPath, config.maxOpenFiles),
      _config(config) {
}

const char *FreshEMMCStorage::name() const {
	return "eMMC";
}

FreshResult FreshEMMCStorage::mount() {
	if (isMounted()) return FreshResult::success("eMMC storage already mounted");
	if (*mountPath() == '\0' || mountPath()[0] != '/') {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "eMMC mount path is invalid");
	}
	if (_config.maxOpenFiles == 0 || _config.maxOpenFiles > static_cast<size_t>(INT_MAX)) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "eMMC max open files is out of range");
	}
	if (_config.allocationUnitSize == 0 ||
	    _config.allocationUnitSize > static_cast<size_t>(128 * 1024)) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "eMMC allocation unit size is out of range");
	}
	if (_config.slot < 0) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "eMMC slot is invalid");
	}
	if (_config.busWidth != 1 && _config.busWidth != 4 && _config.busWidth != 8) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "eMMC bus width must be 1, 4, or 8");
	}
	if (_config.frequencyHz == 0) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "eMMC frequency must be greater than zero");
	}

#if !defined(ESP32)
	setState(FreshStorageState::Error);
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "eMMC storage requires ESP32");
#elif !defined(SOC_SDMMC_HOST_SUPPORTED) || !SOC_SDMMC_HOST_SUPPORTED
	setState(FreshStorageState::Error);
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "target does not support an SDMMC host");
#else
	setState(FreshStorageState::Mounting);

	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	host.slot = _config.slot;
	host.max_freq_khz = static_cast<int>(_config.frequencyHz / 1000);
	if (host.max_freq_khz <= 0) host.max_freq_khz = 1;

	sdmmc_slot_config_t slotConfig = SDMMC_SLOT_CONFIG_DEFAULT();
	slotConfig.width = _config.busWidth;
	slotConfig.flags |= _config.slotFlags;

	const bool customPins = _config.clockPin != GPIO_NUM_NC ||
	                        _config.commandPin != GPIO_NUM_NC ||
	                        _config.data0Pin != GPIO_NUM_NC ||
	                        _config.data1Pin != GPIO_NUM_NC ||
	                        _config.data2Pin != GPIO_NUM_NC ||
	                        _config.data3Pin != GPIO_NUM_NC ||
	                        _config.data4Pin != GPIO_NUM_NC ||
	                        _config.data5Pin != GPIO_NUM_NC ||
	                        _config.data6Pin != GPIO_NUM_NC ||
	                        _config.data7Pin != GPIO_NUM_NC;
	if (customPins) {
#if defined(SOC_SDMMC_USE_GPIO_MATRIX) && SOC_SDMMC_USE_GPIO_MATRIX
		if (_config.clockPin == GPIO_NUM_NC || _config.commandPin == GPIO_NUM_NC ||
		    _config.data0Pin == GPIO_NUM_NC) {
			setState(FreshStorageState::Error);
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "custom eMMC pins require clock, command, and data0"
			);
		}
		if (_config.busWidth >= 4 &&
		    (_config.data1Pin == GPIO_NUM_NC || _config.data2Pin == GPIO_NUM_NC ||
		     _config.data3Pin == GPIO_NUM_NC)) {
			setState(FreshStorageState::Error);
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "four-bit eMMC requires data1, data2, and data3"
			);
		}
		if (_config.busWidth == 8 &&
		    (_config.data4Pin == GPIO_NUM_NC || _config.data5Pin == GPIO_NUM_NC ||
		     _config.data6Pin == GPIO_NUM_NC || _config.data7Pin == GPIO_NUM_NC)) {
			setState(FreshStorageState::Error);
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "eight-bit eMMC requires data4 through data7"
			);
		}

		slotConfig.clk = _config.clockPin;
		slotConfig.cmd = _config.commandPin;
		slotConfig.d0 = _config.data0Pin;
		if (_config.busWidth >= 4) {
			slotConfig.d1 = _config.data1Pin;
			slotConfig.d2 = _config.data2Pin;
			slotConfig.d3 = _config.data3Pin;
		}
		if (_config.busWidth == 8) {
			slotConfig.d4 = _config.data4Pin;
			slotConfig.d5 = _config.data5Pin;
			slotConfig.d6 = _config.data6Pin;
			slotConfig.d7 = _config.data7Pin;
		}
#else
		setState(FreshStorageState::Error);
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "target SDMMC host does not support custom eMMC GPIO routing"
		);
#endif
	}

	esp_vfs_fat_sdmmc_mount_config_t mountConfig = {};
	mountConfig.format_if_mount_failed = _config.formatOnMountFailure;
	mountConfig.max_files = static_cast<int>(_config.maxOpenFiles);
	mountConfig.allocation_unit_size = _config.allocationUnitSize;

	const esp_err_t mounted = esp_vfs_fat_sdmmc_mount(
	    mountPath(),
	    &host,
	    &slotConfig,
	    &mountConfig,
	    &_card
	);
	_nativeError = static_cast<int>(mounted);
	if (mounted != ESP_OK) {
		_card = nullptr;
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::StorageUnavailable, "failed to mount eMMC storage");
	}

	setState(FreshStorageState::Mounted);
	_nativeError = 0;
	return FreshResult::success("eMMC storage mounted");
#endif
}

FreshResult FreshEMMCStorage::unmount() {
	FreshResult canUnmount = validateCanUnmount();
	if (!canUnmount) return canUnmount;

#if !defined(ESP32)
	setState(FreshStorageState::Error);
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "eMMC storage requires ESP32");
#else
	if (_card == nullptr) {
		setState(FreshStorageState::Uninitialized);
		return FreshResult::success("eMMC storage not mounted");
	}

	setState(FreshStorageState::Unmounting);
	const esp_err_t unmounted = esp_vfs_fat_sdcard_unmount(mountPath(), _card);
	_nativeError = static_cast<int>(unmounted);
	if (unmounted != ESP_OK) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to unmount eMMC storage");
	}
	_card = nullptr;
	setState(FreshStorageState::Uninitialized);
	_nativeError = 0;
	return FreshResult::success("eMMC storage unmounted");
#endif
}

FreshStorageInfo FreshEMMCStorage::info() const {
	FreshStorageInfo result;
	(void)readInfoBackend(result);
	return result;
}

FreshResult FreshEMMCStorage::readInfoBackend(FreshStorageInfo &result) const {
	result = FreshStorageInfo();
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "eMMC storage requires ESP32");
#else
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::StorageUnavailable, "eMMC storage is not mounted");
	}
	uint64_t totalBytes = 0;
	uint64_t freeBytes = 0;
	const esp_err_t queried = esp_vfs_fat_info(mountPath(), &totalBytes, &freeBytes);
	_nativeError = static_cast<int>(queried);
	if (queried != ESP_OK) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to query eMMC storage");
	}
	const uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
	result.totalBytes = static_cast<size_t>(totalBytes > maxSize ? maxSize : totalBytes);
	result.freeBytes = static_cast<size_t>(freeBytes > maxSize ? maxSize : freeBytes);
	result.usedBytes = result.totalBytes > result.freeBytes
	                       ? result.totalBytes - result.freeBytes
	                       : 0;
	return FreshResult::success("eMMC storage information read");
#endif
}
