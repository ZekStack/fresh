#include "FreshLittleFSStorage.h"

#include "../Fresh.h"
#include "../internal/FreshArduinoLittleFSBridge.h"

#if defined(ESP32)
extern "C" {
#include <esp_littlefs.h>
}
#include <esp_err.h>
#endif

#include <cstring>
#include <limits>

FreshLittleFSStorage::FreshLittleFSStorage(const FreshLittleFSConfig &config)
    : FreshStorage(FreshStorageType::LittleFS, config.mountPath, config.maxOpenFiles),
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

	// Fresh performs file operations through POSIX/VFS, but the isolated bridge
	// also binds Arduino's global LittleFS wrapper to the managed mountpoint.
	// Fresh 0.1.x provided that process-wide side effect, and existing applications
	// may use LittleFS outside the database after Fresh initialization.
	const bool mounted = FreshArduinoLittleFSMount(
	    _config.formatOnMountFailure,
	    mountPath(),
	    static_cast<uint8_t>(_config.maxOpenFiles),
	    _partitionLabel.c_str()
	);
	if (!mounted || !esp_littlefs_mounted(_partitionLabel.c_str())) {
		_nativeError = ESP_FAIL;
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::StorageUnavailable, "failed to mount LittleFS storage");
	}

	const char *arduinoMountPath = FreshArduinoLittleFSMountPath();
	if (arduinoMountPath == nullptr || strcmp(arduinoMountPath, mountPath()) != 0) {
		FreshArduinoLittleFSUnmount();
		_nativeError = ESP_ERR_INVALID_STATE;
		setState(FreshStorageState::Error);
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "Arduino LittleFS wrapper did not bind the configured mount path"
		);
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
	FreshArduinoLittleFSUnmount();
	if (esp_littlefs_mounted(_partitionLabel.c_str())) {
		_nativeError = ESP_FAIL;
		setState(FreshStorageState::Error);
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to unmount LittleFS storage");
	}
	setState(FreshStorageState::Uninitialized);
	_nativeError = 0;
	return FreshResult::success("LittleFS storage unmounted");
#endif
}

FreshStorageInfo FreshLittleFSStorage::info() const {
	FreshStorageInfo result;
	readInfoBackend(result);
	return result;
}

FreshResult FreshLittleFSStorage::readInfoBackend(FreshStorageInfo &result) const {
	result = FreshStorageInfo();
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "LittleFS backend requires ESP32");
#else
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::StorageUnavailable, "LittleFS storage is not mounted");
	}
	size_t totalBytes = 0;
	size_t usedBytes = 0;
	const esp_err_t queried = esp_littlefs_info(
	    _partitionLabel.c_str(),
	    &totalBytes,
	    &usedBytes
	);
	_nativeError = static_cast<int>(queried);
	if (queried != ESP_OK) {
		return FreshResult::failure(FreshStatus::FileSystemError, "failed to query LittleFS storage");
	}
	result.totalBytes = totalBytes;
	result.usedBytes = usedBytes;
	result.freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
	return FreshResult::success("LittleFS storage information read");
#endif
}
