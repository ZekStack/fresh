#include "FreshSPIFlashStorage.h"

#include "../Fresh.h"

#if defined(ESP32)
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_flash_spi_init.h>
#include <esp_vfs_fat.h>
#include <spi_flash_mmap.h>
#endif

#include <algorithm>
#include <array>
#include <climits>

namespace {

constexpr uint32_t FreshHzPerMHz = 1'000'000;
constexpr size_t FreshPartitionLabelCapacity = 16;
constexpr size_t FreshBlankProbeBytes = 4096;

#if defined(ESP32)
bool isValidInputPin(gpio_num_t pin) {
	const int value = static_cast<int>(pin);
	return value >= 0 && value < GPIO_PIN_COUNT && GPIO_IS_VALID_GPIO(value);
}

bool isValidOutputPin(gpio_num_t pin) {
	const int value = static_cast<int>(pin);
	return value >= 0 && value < GPIO_PIN_COUNT && GPIO_IS_VALID_OUTPUT_GPIO(value);
}
#endif

} // namespace

FreshSPIFlashStorage::FreshSPIFlashStorage(const FreshSPIFlashConfig &config)
    : FreshStorage(FreshStorageType::SPIFlash, config.mountPath, config.maxOpenFiles),
      _config(config),
      _partitionLabel(config.partitionLabel != nullptr ? config.partitionLabel : "") {
}

const char *FreshSPIFlashStorage::name() const {
	return "SPI Flash";
}

FreshResult FreshSPIFlashStorage::validateConfig() const {
	if (*mountPath() == '\0' || mountPath()[0] != '/') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SPI flash mount path is invalid");
	}
	if (_partitionLabel.empty() || _partitionLabel.size() >= FreshPartitionLabelCapacity) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash partition label must contain 1-15 characters"
		);
	}
	if (_config.maxOpenFiles == 0 || _config.maxOpenFiles > static_cast<size_t>(INT_MAX)) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash max open files is out of range"
		);
	}
	if (_config.allocationUnitSize == 0 ||
	    _config.allocationUnitSize > static_cast<size_t>(128 * 1024)) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash allocation unit size is out of range"
		);
	}
	if (_config.frequencyHz == 0 || _config.frequencyHz % FreshHzPerMHz != 0) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash frequency must be a positive whole number of MHz"
		);
	}

#if !defined(ESP32)
	return FreshResult::failure(
	    FreshStatus::UnsupportedOperation,
	    "SPI flash storage requires ESP32"
	);
#else
	if (!isValidOutputPin(_config.chipSelectPin)) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash chip-select pin is invalid"
		);
	}
	if (_config.busOwnership == FreshSPIBusOwnership::Managed) {
		if (!isValidOutputPin(_config.clockPin) ||
		    !isValidOutputPin(_config.mosiPin) ||
		    !isValidInputPin(_config.misoPin)) {
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "managed SPI flash requires valid clock, MOSI, and MISO pins"
			);
		}
		const int cs = static_cast<int>(_config.chipSelectPin);
		const int clock = static_cast<int>(_config.clockPin);
		const int mosi = static_cast<int>(_config.mosiPin);
		const int miso = static_cast<int>(_config.misoPin);
		if (cs == clock || cs == mosi || cs == miso ||
		    clock == mosi || clock == miso || mosi == miso) {
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "managed SPI flash signal pins must be distinct"
			);
		}
	}
	if (_config.partitionOffset % SPI_FLASH_SEC_SIZE != 0) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash partition offset must be sector aligned"
		);
	}
	if (_config.partitionSize != 0 && _config.partitionSize % SPI_FLASH_SEC_SIZE != 0) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash partition size must be sector aligned"
		);
	}
	return FreshResult::success("SPI flash configuration valid");
#endif
}

FreshResult FreshSPIFlashStorage::initializeManagedSPIBus() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SPI flash storage requires ESP32");
#else
	if (_config.busOwnership != FreshSPIBusOwnership::Managed) {
		return FreshResult::success("SPI flash bus is externally managed");
	}

	spi_bus_config_t busConfig = {};
	busConfig.mosi_io_num = _config.mosiPin;
	busConfig.miso_io_num = _config.misoPin;
	busConfig.sclk_io_num = _config.clockPin;
	busConfig.quadwp_io_num = -1;
	busConfig.quadhd_io_num = -1;
	busConfig.max_transfer_sz = 0;

	const esp_err_t initialized =
	    spi_bus_initialize(_config.host, &busConfig, SPI_DMA_CH_AUTO);
	_nativeError = static_cast<int>(initialized);
	if (initialized != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to initialize SPI flash bus"
		);
	}
	_spiBusInitialized = true;
	return FreshResult::success("SPI flash bus initialized");
#endif
}

FreshResult FreshSPIFlashStorage::attachFlash() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SPI flash storage requires ESP32");
#else
	esp_flash_spi_device_config_t deviceConfig = {};
	deviceConfig.host_id = _config.host;
	deviceConfig.cs_io_num = _config.chipSelectPin;
	deviceConfig.io_mode = SPI_FLASH_FASTRD;
	deviceConfig.input_delay_ns = 0;
	deviceConfig.cs_id = 0;
	deviceConfig.freq_mhz = static_cast<int>(_config.frequencyHz / FreshHzPerMHz);

	esp_err_t error = spi_bus_add_flash_device(&_flash, &deviceConfig);
	_nativeError = static_cast<int>(error);
	if (error != ESP_OK) {
		_flash = nullptr;
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "failed to attach SPI flash device"
		);
	}

	error = esp_flash_init(_flash);
	_nativeError = static_cast<int>(error);
	if (error != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "failed to initialize SPI flash device"
		);
	}
	error = esp_flash_read_id(_flash, &_flashId);
	_nativeError = static_cast<int>(error);
	if (error != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "failed to read SPI flash identifier"
		);
	}
	error = esp_flash_get_size(_flash, &_flashSizeBytes);
	_nativeError = static_cast<int>(error);
	if (error != ESP_OK || _flashSizeBytes == 0) {
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "failed to read SPI flash capacity"
		);
	}
	return FreshResult::success("SPI flash device initialized");
#endif
}

FreshResult FreshSPIFlashStorage::registerPartition() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SPI flash storage requires ESP32");
#else
	if (_flash == nullptr || _flashSizeBytes == 0) {
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "SPI flash device is unavailable"
		);
	}
	if (esp_partition_find_first(
	        ESP_PARTITION_TYPE_DATA,
	        ESP_PARTITION_SUBTYPE_ANY,
	        _partitionLabel.c_str()
	    ) != nullptr) {
		return FreshResult::failure(
		    FreshStatus::Busy,
		    "SPI flash partition label is already registered"
		);
	}
	if (_config.partitionOffset >= static_cast<size_t>(_flashSizeBytes)) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash partition offset is outside the device"
		);
	}

	const size_t available =
	    static_cast<size_t>(_flashSizeBytes) - _config.partitionOffset;
	const size_t partitionSize =
	    _config.partitionSize == 0 ? available : _config.partitionSize;
	if (partitionSize == 0 || partitionSize > available ||
	    partitionSize % SPI_FLASH_SEC_SIZE != 0) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SPI flash partition size is invalid"
		);
	}

	const esp_err_t registered = esp_partition_register_external(
	    _flash,
	    _config.partitionOffset,
	    partitionSize,
	    _partitionLabel.c_str(),
	    ESP_PARTITION_TYPE_DATA,
	    ESP_PARTITION_SUBTYPE_DATA_FAT,
	    &_partition
	);
	_nativeError = static_cast<int>(registered);
	if (registered != ESP_OK || _partition == nullptr) {
		_partition = nullptr;
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to register external SPI flash partition"
		);
	}
	return FreshResult::success("SPI flash partition registered");
#endif
}

FreshResult FreshSPIFlashStorage::inspectPartitionBlank(bool &blank) const {
	blank = false;
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SPI flash storage requires ESP32");
#else
	if (_partition == nullptr) {
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "SPI flash partition is unavailable"
		);
	}
	std::array<uint8_t, FreshBlankProbeBytes> buffer{};
	size_t offset = 0;
	while (offset < _partition->size) {
		const size_t length = std::min(buffer.size(), static_cast<size_t>(_partition->size) - offset);
		const esp_err_t read =
		    esp_partition_read(_partition, offset, buffer.data(), length);
		_nativeError = static_cast<int>(read);
		if (read != ESP_OK) {
			return FreshResult::failure(
			    FreshStatus::FileSystemError,
			    "failed to inspect SPI flash partition"
			);
		}
		for (size_t index = 0; index < length; ++index) {
			if (buffer[index] != 0xFF) {
				return FreshResult::success("SPI flash partition contains data");
			}
		}
		offset += length;
	}
	blank = true;
	return FreshResult::success("SPI flash partition is erased");
#endif
}

FreshResult FreshSPIFlashStorage::mountFilesystem() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SPI flash storage requires ESP32");
#else
	bool blank = false;
	if (_config.formatIfBlank) {
		FreshResult inspected = inspectPartitionBlank(blank);
		if (!inspected) return inspected;
	}

	esp_vfs_fat_mount_config_t mountConfig = {};
	mountConfig.format_if_mount_failed =
	    _config.formatOnMountFailure || (_config.formatIfBlank && blank);
	mountConfig.max_files = static_cast<int>(_config.maxOpenFiles);
	mountConfig.allocation_unit_size = _config.allocationUnitSize;

	_wlHandle = WL_INVALID_HANDLE;
	const esp_err_t mounted = esp_vfs_fat_spiflash_mount_rw_wl(
	    mountPath(),
	    _partitionLabel.c_str(),
	    &mountConfig,
	    &_wlHandle
	);
	_nativeError = static_cast<int>(mounted);
	if (mounted != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "failed to mount SPI flash FAT filesystem"
		);
	}
	_filesystemMounted = true;
	return FreshResult::success("SPI flash filesystem mounted");
#endif
}

FreshResult FreshSPIFlashStorage::releaseFilesystem() {
#if !defined(ESP32)
	return FreshResult::success("SPI flash filesystem unavailable");
#else
	if (_filesystemMounted) {
		const esp_err_t unmounted =
		    esp_vfs_fat_spiflash_unmount_rw_wl(mountPath(), _wlHandle);
		_nativeError = static_cast<int>(unmounted);
		if (unmounted != ESP_OK) {
			return FreshResult::failure(
			    FreshStatus::FileSystemError,
			    "failed to unmount SPI flash filesystem"
			);
		}
		_filesystemMounted = false;
		_wlHandle = WL_INVALID_HANDLE;
		return FreshResult::success("SPI flash filesystem unmounted");
	}
	if (_wlHandle != WL_INVALID_HANDLE) {
		const esp_err_t unmounted = wl_unmount(_wlHandle);
		_nativeError = static_cast<int>(unmounted);
		if (unmounted != ESP_OK) {
			return FreshResult::failure(
			    FreshStatus::FileSystemError,
			    "failed to release SPI flash wear levelling"
			);
		}
		_wlHandle = WL_INVALID_HANDLE;
	}
	return FreshResult::success("SPI flash filesystem not mounted");
#endif
}

FreshResult FreshSPIFlashStorage::releasePartition() {
#if !defined(ESP32)
	return FreshResult::success("SPI flash partition unavailable");
#else
	if (_partition == nullptr) {
		return FreshResult::success("SPI flash partition not registered");
	}
	const esp_err_t deregistered = esp_partition_deregister_external(_partition);
	_nativeError = static_cast<int>(deregistered);
	if (deregistered != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to deregister SPI flash partition"
		);
	}
	_partition = nullptr;
	return FreshResult::success("SPI flash partition deregistered");
#endif
}

FreshResult FreshSPIFlashStorage::releaseFlash() {
#if !defined(ESP32)
	return FreshResult::success("SPI flash device unavailable");
#else
	if (_flash == nullptr) {
		return FreshResult::success("SPI flash device not attached");
	}
	const esp_err_t removed = spi_bus_remove_flash_device(_flash);
	_nativeError = static_cast<int>(removed);
	if (removed != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to remove SPI flash device"
		);
	}
	_flash = nullptr;
	_flashId = 0;
	_flashSizeBytes = 0;
	return FreshResult::success("SPI flash device removed");
#endif
}

FreshResult FreshSPIFlashStorage::releaseManagedSPIBus() {
#if !defined(ESP32)
	return FreshResult::success("SPI flash bus unavailable");
#else
	if (_config.busOwnership != FreshSPIBusOwnership::Managed || !_spiBusInitialized) {
		return FreshResult::success("SPI flash bus not owned");
	}
	const esp_err_t released = spi_bus_free(_config.host);
	_nativeError = static_cast<int>(released);
	if (released != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to release SPI flash bus"
		);
	}
	_spiBusInitialized = false;
	return FreshResult::success("SPI flash bus released");
#endif
}

FreshResult FreshSPIFlashStorage::cleanupMountFailure(
    const FreshResult &failure,
    int nativeError
) {
	FreshResult cleanup = releaseFilesystem();
	if (cleanup) cleanup = releasePartition();
	if (cleanup) cleanup = releaseFlash();
	if (cleanup) cleanup = releaseManagedSPIBus();
	_nativeError = nativeError;
	if (cleanup) return failure;
	std::string message = failure.message;
	message += "; cleanup failed: ";
	message += cleanup.message;
	return FreshResult::failure(failure.status, message.c_str(), failure.affectedCount);
}

FreshResult FreshSPIFlashStorage::mount() {
	if (isMounted()) return FreshResult::success("SPI flash storage already mounted");

	FreshResult validated = validateConfig();
	if (!validated) {
		setState(FreshStorageState::Error);
		return validated;
	}
	setState(FreshStorageState::Mounting);

	FreshResult result = initializeManagedSPIBus();
	if (!result) {
		const int nativeError = _nativeError;
		setState(FreshStorageState::Error);
		return cleanupMountFailure(result, nativeError);
	}
	result = attachFlash();
	if (!result) {
		const int nativeError = _nativeError;
		setState(FreshStorageState::Error);
		return cleanupMountFailure(result, nativeError);
	}
	result = registerPartition();
	if (!result) {
		const int nativeError = _nativeError;
		setState(FreshStorageState::Error);
		return cleanupMountFailure(result, nativeError);
	}
	result = mountFilesystem();
	if (!result) {
		const int nativeError = _nativeError;
		setState(FreshStorageState::Error);
		return cleanupMountFailure(result, nativeError);
	}

	setState(FreshStorageState::Mounted);
	_nativeError = 0;
	return FreshResult::success("SPI flash storage mounted");
}

FreshResult FreshSPIFlashStorage::unmount() {
	FreshResult canUnmount = validateCanUnmount();
	if (!canUnmount) return canUnmount;

	setState(FreshStorageState::Unmounting);
	FreshResult result = releaseFilesystem();
	if (!result) {
		setState(FreshStorageState::Error);
		return result;
	}
	result = releasePartition();
	if (!result) {
		setState(FreshStorageState::Error);
		return result;
	}
	result = releaseFlash();
	if (!result) {
		setState(FreshStorageState::Error);
		return result;
	}
	result = releaseManagedSPIBus();
	if (!result) {
		setState(FreshStorageState::Error);
		return result;
	}

	setState(FreshStorageState::Uninitialized);
	_nativeError = 0;
	return FreshResult::success("SPI flash storage unmounted");
}

FreshStorageInfo FreshSPIFlashStorage::info() const {
	FreshStorageInfo result;
	(void)readInfoBackend(result);
	return result;
}

FreshResult FreshSPIFlashStorage::readInfoBackend(FreshStorageInfo &result) const {
	result = FreshStorageInfo();
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SPI flash storage requires ESP32");
#else
	if (!isMounted() || !_filesystemMounted) {
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "SPI flash storage is not mounted"
		);
	}
	uint64_t totalBytes = 0;
	uint64_t freeBytes = 0;
	const esp_err_t queried = esp_vfs_fat_info(mountPath(), &totalBytes, &freeBytes);
	_nativeError = static_cast<int>(queried);
	if (queried != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to query SPI flash storage"
		);
	}
	result.totalBytes = totalBytes;
	result.freeBytes = freeBytes;
	result.usedBytes =
	    result.totalBytes > result.freeBytes ? result.totalBytes - result.freeBytes : 0;
	return FreshResult::success("SPI flash storage information read");
#endif
}
