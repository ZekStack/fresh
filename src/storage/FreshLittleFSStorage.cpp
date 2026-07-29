#include "FreshLittleFSStorage.h"

#include "../Fresh.h"

#if defined(ESP32)
extern "C" {
#include <esp_littlefs.h>
}
#include <esp_err.h>
#endif

#include <limits>

FreshLittleFSStorage::FreshLittleFSStorage(const FreshLittleFSConfig &config)
    : FreshStorage(FreshStorageType::LittleFS, config.mountPath),
      _config(config),
      _partitionLabel(config.partitionLabel != nullptr ? config.partitionLabel : "") {
}

const char *FreshLittleFSStorage::name() const {
	return "LittleFS";
}

FreshResult FreshLittleFSStorage::mount() {
	if (isMounted()) return FreshResult::success("LittleFS already mounted");
	if (*mountPath() == '\0' || mountPath()[0] != '/') {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS mount path is invalid");
	}
	if (_partitionLabel.empty()) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS partition label is required");
	}
	if (_config.maxOpenFiles == 0 ||
	    _config.maxOpenFiles > std::numeric_limits<uint8_t>::max()) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::InvalidArgument, "LittleFS max open files is out of range");
	}

#if !defined(ESP32)
	setState(FreshStorageState::Error);
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "LittleFS backend requires ESP32");
#else
	if (esp_littlefs_mounted(_partitionLabel.c_str())) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(
		    FreshStatus::Busy,
		    "LittleFS partition is already mounted; use caller-owned custom storage for shared mounts"
		);
	}

	setState(FreshStorageState::Mounting);
	esp_vfs_littlefs_conf_t configuration = {};
	configuration.base_path = mountPath();
	configuration.partition_label = _partitionLabel.c_str();
	configuration.partition = nullptr;
	configuration.format_if_mount_failed = _config.formatOnMountFailure;
	configuration.read_only = false;
	configuration.dont_mount = false;
	configuration.grow_on_mount = _config.growOnMount;

	const esp_err_t mounted = esp_vfs_littlefs_register(&configuration);
	_nativeError = static_cast<int>(mounted);
	if (mounted != ESP_OK) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::StorageUnavailable, "failed to mount LittleFS storage");
	}

	setState(FreshStorageState::Mounted);
	_nativeError = 0;
	return FreshResult::success("LittleFS storage mounted");
#endif
}

FreshResult FreshLittleFSStorage::unmount() {
	FreshResult canUnmount = validateCanUnmount();
	if (!canUnmount) return canUnmount;

	if (!isMounted()) {
		setState(FreshStorageState::Uninitialized);
		return FreshResult::success("LittleFS storage not mounted");
	}

#if !defined(ESP32)
	setState(FreshStorageState::Error);
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "LittleFS backend requires ESP32");
#else
	setState(FreshStorageState::Unmounting);
	const esp_err_t unmounted = esp_vfs_littlefs_unregister(_partitionLabel.c_str());
	_nativeError = static_cast<int>(unmounted);
	if (unmounted != ESP_OK) {
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to unmount LittleFS storage");
	}
	setState(FreshStorageState::Uninitialized);
	_nativeError = 0;
	return FreshResult::success("LittleFS storage unmounted");
#endif
}

FreshStorageInfo FreshLittleFSStorage::info() const {
	FreshStorageInfo info;
#if defined(ESP32)
	if (!isMounted()) return info;
	size_t totalBytes = 0;
	size_t usedBytes = 0;
	const esp_err_t result = esp_littlefs_info(
	    _partitionLabel.c_str(),
	    &totalBytes,
	    &usedBytes
	);
	if (result != ESP_OK) return info;
	info.totalBytes = totalBytes;
	info.usedBytes = usedBytes;
	info.freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
#endif
	return info;
}
