#include "FreshLittleFSStorage.h"

#include "../Fresh.h"

#include <LittleFS.h>
#include <limits>

FreshLittleFSStorage::FreshLittleFSStorage(const FreshLittleFSConfig &config)
    : FreshStorage(FreshStorageType::LittleFS), _config(config) {
}

const char *FreshLittleFSStorage::name() const {
	return "LittleFS";
}

FreshResult FreshLittleFSStorage::mount() {
	if (isMounted()) {
		return FreshResult::success("LittleFS already mounted");
	}
	if (_config.mountPath == nullptr || *_config.mountPath == '\0') {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS mount path is required");
	}
	if (_config.partitionLabel == nullptr || *_config.partitionLabel == '\0') {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS partition label is required");
	}
	if (_config.maxOpenFiles == 0 ||
	    _config.maxOpenFiles > std::numeric_limits<uint8_t>::max()) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS max open files is out of range");
	}

	setState(FreshStorageState::Mounting);
	if (!LittleFS.begin(
	        _config.formatOnMountFailure,
	        _config.mountPath,
	        static_cast<uint8_t>(_config.maxOpenFiles),
	        _config.partitionLabel
	    )) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to mount LittleFS storage");
	}

	setState(FreshStorageState::Mounted);
	return FreshResult::success("LittleFS storage mounted");
}

FreshResult FreshLittleFSStorage::unmount() {
	if (!isMounted()) {
		setState(FreshStorageState::Uninitialized);
		return FreshResult::success("LittleFS storage not mounted");
	}

	setState(FreshStorageState::Unmounting);
	LittleFS.end();
	setState(FreshStorageState::Uninitialized);
	return FreshResult::success("LittleFS storage unmounted");
}

FreshStorageInfo FreshLittleFSStorage::info() const {
	FreshStorageInfo info;
	if (!isMounted()) {
		return info;
	}

	info.totalBytes = LittleFS.totalBytes();
	info.usedBytes = LittleFS.usedBytes();
	info.freeBytes = info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
	return info;
}
