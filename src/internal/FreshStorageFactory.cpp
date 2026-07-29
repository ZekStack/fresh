#include "FreshStorageFactory.h"

#include "../Fresh.h"
#include "../storage/FreshLittleFSStorage.h"
#include "../storage/FreshSDStorage.h"

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
	return FreshResult::success("LittleFS configuration valid");
}

FreshResult FreshValidateSDConfig(const FreshSDConfig &config) {
	if (!FreshValidMountPath(config.mountPath)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD mount path is invalid");
	}
	if (config.maxOpenFiles == 0 || config.maxOpenFiles > static_cast<size_t>(INT_MAX)) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD max open files is out of range");
	}
	if (config.allocationUnitSize == 0 || config.allocationUnitSize > 128 * 1024) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "SD allocation unit size is out of range");
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

	case FreshSDInterface::SDMMC:
		return FreshResult::success("SDMMC configuration accepted for later mount validation");
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
	}
	if (!storage) {
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate storage backend");
	}
	return FreshResult::success("storage backend created");
}
