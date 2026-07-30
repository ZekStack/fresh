#include "FreshStorageFactory.h"

#include "../Fresh.h"
#include "../storage/FreshLittleFSStorage.h"
#include "../storage/FreshSDStorage.h"

#if defined(ESP32)
#include <soc/soc_caps.h>
#endif

#include <climits>
#include <limits>
#include <new>

namespace {

bool FreshValidMountPath(const char *path) {
	if (path == nullptr || path[0] != '/' || path[1] == '\0') return false;
	for (const char *cursor = path; *cursor != '\0'; ++cursor) {
		if (*cursor == '\\') return false;
	}
	return true;
}

bool FreshValidAllocationUnit(size_t size) {
	return size >= 512 && size <= 64 * 1024 && (size & (size - 1)) == 0;
}

FreshResult FreshValidateLittleFSConfig(const FreshLittleFSConfig &config) {
	if (!FreshValidMountPath(config.mountPath)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS mount path is invalid");
	}
	if (config.partitionLabel == nullptr || *config.partitionLabel == '\0') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS partition label is required");
	}
	if (config.maxOpenFiles == 0 ||
	    config.maxOpenFiles > std::numeric_limits<uint8_t>::max()) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS max open files is out of range");
	}
	if (!config.growOnMount) {
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "managed LittleFS requires growOnMount while Arduino LittleFS interoperability is enabled"
		);
	}
	return FreshResult::success("LittleFS configuration valid");
}

FreshResult FreshValidateSDConfig(const FreshSDConfig &config) {
	if (!FreshValidMountPath(config.mountPath)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD mount path is invalid");
	}
	if (config.maxOpenFiles == 0 || config.maxOpenFiles > static_cast<size_t>(INT_MAX)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD max open files is out of range");
	}
	if (!FreshValidAllocationUnit(config.allocationUnitSize)) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "SD allocation unit must be a power of two between 512 and 65536 bytes"
		);
	}

	switch (config.interface) {
	case FreshSDInterface::SPI:
		if (config.spi.chipSelectPin == GPIO_NUM_NC) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "SDSPI chip select pin is required");
		}
		if (config.spi.frequencyHz == 0) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "SDSPI frequency must be greater than zero");
		}
		if (config.spi.busOwnership == FreshSPIBusOwnership::Managed &&
		    (config.spi.clockPin == GPIO_NUM_NC || config.spi.mosiPin == GPIO_NUM_NC ||
		     config.spi.misoPin == GPIO_NUM_NC)) {
			return FreshResult::failure(
			    FreshStatus::InvalidArgument,
			    "managed SDSPI requires clock, MOSI, and MISO pins"
			);
		}
		return FreshResult::success("SDSPI configuration valid");

	case FreshSDInterface::SDMMC: {
#if defined(ESP32) && defined(SOC_SDMMC_HOST_SUPPORTED) && SOC_SDMMC_HOST_SUPPORTED
		if (config.sdmmc.slot < 0) {
			return FreshResult::failure(FreshStatus::InvalidArgument, "SDMMC slot is invalid");
		}
		const bool customPins = config.sdmmc.clockPin != GPIO_NUM_NC ||
		                        config.sdmmc.commandPin != GPIO_NUM_NC ||
		                        config.sdmmc.data0Pin != GPIO_NUM_NC ||
		                        config.sdmmc.data1Pin != GPIO_NUM_NC ||
		                        config.sdmmc.data2Pin != GPIO_NUM_NC ||
		                        config.sdmmc.data3Pin != GPIO_NUM_NC;
		if (customPins) {
#if defined(SOC_SDMMC_USE_GPIO_MATRIX) && SOC_SDMMC_USE_GPIO_MATRIX
			if (config.sdmmc.clockPin == GPIO_NUM_NC ||
			    config.sdmmc.commandPin == GPIO_NUM_NC ||
			    config.sdmmc.data0Pin == GPIO_NUM_NC) {
				return FreshResult::failure(
				    FreshStatus::InvalidArgument,
				    "custom SDMMC pins require clock, command, and data0"
				);
			}
			if (!config.sdmmc.oneBitMode &&
			    (config.sdmmc.data1Pin == GPIO_NUM_NC || config.sdmmc.data2Pin == GPIO_NUM_NC ||
			     config.sdmmc.data3Pin == GPIO_NUM_NC)) {
				return FreshResult::failure(
				    FreshStatus::InvalidArgument,
				    "four-bit SDMMC requires data1, data2, and data3"
				);
			}
#else
			return FreshResult::failure(
			    FreshStatus::UnsupportedOperation,
			    "target SDMMC host does not support custom GPIO routing"
			);
#endif
		}
		return FreshResult::success("SDMMC configuration valid");
#else
		return FreshResult::failure(FreshStatus::UnsupportedOperation, "target does not support SDMMC host");
#endif
	}
	}
	return FreshResult::failure(FreshStatus::InvalidArgument, "unknown SD interface");
}

} // namespace

FreshResult FreshValidateStorageConfig(
    FreshStorageType type,
    const FreshLittleFSConfig &littleFS,
    const FreshSDConfig &sd
) {
	switch (type) {
	case FreshStorageType::LittleFS: return FreshValidateLittleFSConfig(littleFS);
	case FreshStorageType::SD: return FreshValidateSDConfig(sd);
	case FreshStorageType::Custom:
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "custom storage must be supplied through a custom-storage init overload"
		);
	}
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "unknown storage type");
}

FreshResult FreshCreateStorage(
    FreshStorageType type,
    const FreshLittleFSConfig &littleFS,
    const FreshSDConfig &sd,
    std::unique_ptr<FreshStorage> &storage
) {
	storage.reset();
	FreshResult valid = FreshValidateStorageConfig(type, littleFS, sd);
	if (!valid) return valid;

	switch (type) {
	case FreshStorageType::LittleFS:
		storage.reset(new (std::nothrow) FreshLittleFSStorage(littleFS));
		break;
	case FreshStorageType::SD:
		storage.reset(new (std::nothrow) FreshSDStorage(sd));
		break;
	case FreshStorageType::Custom:
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "custom storage cannot be created by the built-in storage factory"
		);
	}
	if (!storage) {
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate storage backend");
	}
	return FreshResult::success("storage backend created");
}
