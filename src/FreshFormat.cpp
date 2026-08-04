#include "Fresh.h"

#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"
#include "internal/FreshStorageAccessState.h"
#include "internal/FreshStorageContext.h"
#include "storage/FreshEMMCStorage.h"
#include "storage/FreshLittleFSStorage.h"
#include "storage/FreshSDStorage.h"

#if defined(ESP32)
extern "C" {
#include <esp_littlefs.h>
}
#include <esp_err.h>
#include <esp_vfs_fat.h>
#endif

#include <climits>
#include <limits>

namespace {

constexpr uint32_t FreshFormatStopTimeoutMS = 5000;

FreshResult FreshResetBackupAfterFormat(FreshBackupRuntimeState &backup) {
	FreshLock lock(backup.mutex);
	if (!lock) {
		return FreshResult::failure(
		    FreshStatus::InternalError,
		    "failed to reset backup state after format"
		);
	}
	backup.options = FreshBackupOptions();
	backup.head = 0;
	backup.tail = 0;
	backup.used = 0;
	backup.progress = 0;
	backup.total = 0;
	backup.lastProgressEvent = 0;
	backup.requested = false;
	backup.running = false;
	backup.done = false;
	backup.cancelled = false;
	backup.state = FreshBackupState::NotRunning;
	backup.result = FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");
	return FreshResult::success("backup state reset");
}

#if defined(ESP32)
FreshResult FreshFormatFatStorage(
    const char *successMessage,
    const char *failureMessage,
    const char *mountPath,
    sdmmc_card_t *card,
    size_t maxOpenFiles,
    size_t allocationUnitSize,
    int &nativeError
) {
	if (card == nullptr) {
		return FreshResult::failure(FreshStatus::StorageUnavailable, "storage card is unavailable");
	}

	esp_vfs_fat_mount_config_t config = {};
	config.format_if_mount_failed = false;
	config.max_files = static_cast<int>(maxOpenFiles);
	config.allocation_unit_size = allocationUnitSize;

	const esp_err_t formatted = esp_vfs_fat_sdcard_format_cfg(mountPath, card, &config);
	nativeError = static_cast<int>(formatted);
	if (formatted != ESP_OK) {
		return FreshResult::failure(
		    formatted == ESP_ERR_NO_MEM ? FreshStatus::OutOfMemory : FreshStatus::FileSystemError,
		    failureMessage
		);
	}

	uint64_t totalBytes = 0;
	uint64_t freeBytes = 0;
	const esp_err_t inspected = esp_vfs_fat_info(mountPath, &totalBytes, &freeBytes);
	nativeError = static_cast<int>(inspected);
	if (inspected != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "formatted FAT storage could not be verified"
		);
	}

	nativeError = 0;
	return FreshResult::success(successMessage);
}
#endif

} // namespace

FreshResult FreshStorage::formatBackend() {
	return FreshResult::failure(
	    FreshStatus::UnsupportedOperation,
	    "storage backend does not support formatting"
	);
}

FreshResult FreshStorage::format() {
	std::shared_ptr<FreshStorageAccessState> accessState =
	    _accessOwner ? _accessOwner->state() : nullptr;
	if (!accessState) {
		return FreshResult::failure(
		    FreshStatus::OutOfMemory,
		    "storage access state is unavailable"
		);
	}
	FreshLock operationLock(accessState->mutex);
	if (!operationLock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock storage operation");
	}
	if (!supportsFormat()) {
		return FreshResult::failure(
		    FreshStatus::UnsupportedOperation,
		    "storage backend does not support formatting"
		);
	}
	if (!isMounted()) {
		return FreshResult::failure(FreshStatus::StorageUnavailable, "storage is not mounted");
	}
	if (openFileCount() != 0) {
		return FreshResult::failure(
		    FreshStatus::Busy,
		    "storage still has open files",
		    openFileCount()
		);
	}

	setState(FreshStorageState::Formatting);
	FreshResult result = formatBackend();
	if (result) {
		setState(FreshStorageState::Mounted);
	} else if (state() == FreshStorageState::Formatting) {
		setState(FreshStorageState::Error);
	}
	return result;
}

FreshResult FreshLittleFSStorage::formatBackend() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "LittleFS backend requires ESP32");
#else
	const esp_err_t formatted = esp_littlefs_format(_partitionLabel.c_str());
	_nativeError = static_cast<int>(formatted);
	if (formatted != ESP_OK) {
		setState(
		    esp_littlefs_mounted(_partitionLabel.c_str())
		        ? FreshStorageState::Mounted
		        : FreshStorageState::Error
		);
		return FreshResult::failure(
		    formatted == ESP_ERR_NO_MEM ? FreshStatus::OutOfMemory : FreshStatus::FileSystemError,
		    "failed to format LittleFS storage"
		);
	}
	if (!esp_littlefs_mounted(_partitionLabel.c_str())) {
		_nativeError = static_cast<int>(ESP_ERR_INVALID_STATE);
		return FreshResult::failure(
		    FreshStatus::StorageUnavailable,
		    "formatted LittleFS storage was not remounted"
		);
	}

	size_t totalBytes = 0;
	size_t usedBytes = 0;
	const esp_err_t inspected = esp_littlefs_info(
	    _partitionLabel.c_str(),
	    &totalBytes,
	    &usedBytes
	);
	_nativeError = static_cast<int>(inspected);
	if (inspected != ESP_OK) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "formatted LittleFS storage could not be verified"
		);
	}

	_nativeError = 0;
	return FreshResult::success("LittleFS storage formatted");
#endif
}

FreshResult FreshSDStorage::formatBackend() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "SD storage requires ESP32");
#else
	return FreshFormatFatStorage(
	    _config.interface == FreshSDInterface::SPI
	        ? "SDSPI storage formatted"
	        : "SDMMC storage formatted",
	    _config.interface == FreshSDInterface::SPI
	        ? "failed to format SDSPI storage"
	        : "failed to format SDMMC storage",
	    mountPath(),
	    _card,
	    _config.maxOpenFiles,
	    _config.allocationUnitSize,
	    _nativeError
	);
#endif
}

FreshResult FreshEMMCStorage::formatBackend() {
#if !defined(ESP32)
	return FreshResult::failure(FreshStatus::UnsupportedOperation, "eMMC storage requires ESP32");
#else
	return FreshFormatFatStorage(
	    "eMMC storage formatted",
	    "failed to format eMMC storage",
	    mountPath(),
	    _card,
	    _config.maxOpenFiles,
	    _config.allocationUnitSize,
	    _nativeError
	);
#endif
}

FreshResult Fresh::format() {
	TaskHandle_t taskHandle = nullptr;
	bool taskStarted = false;
	size_t modelCount = 0;

	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database lifecycle operation is in progress");
		}
		if (!_storage || !_storage->isMounted()) {
			return FreshResult::failure(FreshStatus::StorageUnavailable, "storage is unavailable");
		}
		if (!_storage->supportsFormat()) {
			return FreshResult::failure(
			    FreshStatus::UnsupportedOperation,
			    "storage backend does not support formatting"
			);
		}

		modelCount = _models.size();
		_stopping = true;
		_lifecycle = Lifecycle::Formatting;
		_stopTask = true;
		_forceSyncRequested = false;
		taskHandle = _syncTaskHandle;
		taskStarted = _syncTaskStarted;
	}

	_storage->setApplicationFileAcceptance(false);

	{
		FreshLock backupLock(_backup->mutex);
		if (!backupLock) {
			FreshLock lock(*_mutex);
			if (lock) {
				_storage->setApplicationFileAcceptance(true);
				_stopping = false;
				_lifecycle = Lifecycle::Running;
				_stopTask = false;
			}
			return FreshResult::failure(FreshStatus::InternalError, "failed to cancel backup for format");
		}
		_backup->requested = false;
		_backup->cancelled = true;
		if (!_backup->running) {
			_backup->state = FreshBackupState::Cancelled;
			_backup->result = FreshResult::failure(FreshStatus::Cancelled, "backup cancelled by format");
		}
	}

	if (taskHandle != nullptr) xTaskNotifyGive(taskHandle);
	if (taskStarted) {
		if (xSemaphoreTake(
		        _syncTaskExited,
		        pdMS_TO_TICKS(FreshFormatStopTimeoutMS)
		    ) != pdTRUE) {
			FreshLock lock(*_mutex);
			_lifecycle = Lifecycle::WaitingForTaskExit;
			return FreshResult::failure(
			    FreshStatus::Timeout,
			    "database format timed out waiting for the sync task"
			);
		}
		if (taskHandle != nullptr) vTaskDelete(taskHandle);
	}

	{
		FreshLock lock(*_mutex);
		_syncTaskHandle = nullptr;
		_syncTaskStarted = false;
	}

	auto resetModelsLocked = [this](bool initialized) {
		for (auto &entry : _models) {
			const std::shared_ptr<FreshModel::State> &state = entry.second;
			state->dropped = true;
			state->docs.clear();
			state->streamEntries.clear();
			state->pending.clear();
			state->validator = nullptr;
			state->dirty = false;
			state->degraded = false;
			state->snapshotRequired = false;
			state->recordsSinceSnapshot = 0;
			state->bytesSinceSnapshot = 0;
			state->checkpointSequence = 0;
			state->lastSequence = 0;
			if (state->revision != std::numeric_limits<uint64_t>::max()) state->revision++;
			if (state->storageEpoch != std::numeric_limits<uint32_t>::max()) state->storageEpoch++;
		}
		_models.clear();
		_diagnostics = FreshDiagnostics();
		_manifestDirty = false;
		_manifestEpoch = 0;
		_nextPendingSequence = 1;
		_databaseRevision = 1;
		_forceSyncRequested = false;
		_initialized = initialized;
	};

	auto markStopped = [this]() {
		FreshLock lock(*_mutex);
		if (!lock) return;
		_initialized = false;
		_stopping = true;
		_stopTask = false;
		_syncTaskHandle = nullptr;
		_syncTaskStarted = false;
		_lifecycle = Lifecycle::Stopped;
	};

	auto startSyncTask = [this]() -> FreshResult {
		FreshLock lock(*_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		xSemaphoreTake(_syncTaskExited, 0);
		_stopTask = false;
		TaskHandle_t handle = nullptr;
		BaseType_t taskResult = pdFAIL;
		if (_config.syncTaskCore == tskNO_AFFINITY) {
			taskResult = xTaskCreate(
			    Fresh::syncTaskThunk,
			    "fresh-sync",
			    _config.syncTaskStackSize,
			    this,
			    _config.syncTaskPriority,
			    &handle
			);
		} else {
			taskResult = xTaskCreatePinnedToCore(
			    Fresh::syncTaskThunk,
			    "fresh-sync",
			    _config.syncTaskStackSize,
			    this,
			    _config.syncTaskPriority,
			    &handle,
			    _config.syncTaskCore
			);
		}
		if (taskResult != pdPASS) {
			_syncTaskHandle = nullptr;
			_syncTaskStarted = false;
			_initialized = false;
			_stopping = true;
			_lifecycle = Lifecycle::Stopped;
			return FreshResult::failure(
			    FreshStatus::InternalError,
			    "failed to restart sync task after format"
			);
		}
		_syncTaskHandle = handle;
		_syncTaskStarted = true;
		_initialized = true;
		_storage->setApplicationFileAcceptance(true);
		_stopping = false;
		_lifecycle = Lifecycle::Running;
		return FreshResult::success("sync task restarted");
	};

	auto recoverBeforeFormat = [this, &startSyncTask](const FreshResult &failure) -> FreshResult {
		FreshResult backupReset = FreshResetBackupAfterFormat(*_backup);
		FreshResult restarted = startSyncTask();
		if (!restarted) return restarted;
		return backupReset ? failure : backupReset;
	};

	FreshResult operation = FreshResult::success("storage ready to format");
	FreshResult preFormatFailure = FreshResult::success("storage format can begin");
	bool nativeFormatStarted = false;
	{
		FreshLock syncLock(*_syncMutex);
		if (!syncLock) {
			preFormatFailure = FreshResult::failure(
			    FreshStatus::InternalError,
			    "failed to lock sync for format"
			);
		} else {
			FreshLock databaseLock(*_mutex);
			if (!databaseLock) {
				preFormatFailure = FreshResult::failure(
				    FreshStatus::InternalError,
				    "failed to lock database for format"
				);
			} else {
				std::shared_ptr<FreshStorageAccessState> accessState =
				    _storage->_accessOwner ? _storage->_accessOwner->state() : nullptr;
				if (!accessState) {
					preFormatFailure = FreshResult::failure(
					    FreshStatus::OutOfMemory,
					    "storage access state is unavailable"
					);
				} else {
					FreshLock accessLock(accessState->mutex);
					if (!accessLock) {
						preFormatFailure = FreshResult::failure(
						    FreshStatus::InternalError,
						    "failed to lock storage for format"
						);
					} else {
						operation = _storage->closeAllFiles(false);
						if (!operation) {
							preFormatFailure = operation;
						} else if (_storage->openFileCount() != 0) {
							preFormatFailure = FreshResult::failure(
							    FreshStatus::Busy,
							    "storage still has open files",
							    _storage->openFileCount()
							);
						} else {
							nativeFormatStarted = true;
							operation = _storage->format();
							resetModelsLocked(static_cast<bool>(operation));
							if (operation) {
								FreshStorageScope storageScope(_storage.get());
								operation = ensureDir(_rootPath);
								if (operation) {
									operation = ensureDir(FreshJoinPath(_rootPath, "models"));
								}
								if (operation) {
									JsonDocument manifest(&FreshJsonAllocator());
									operation = FreshJsonSet(
									    manifest["version"],
									    FreshManifestVersion,
									    manifest,
									    "manifest version"
									);
									if (operation) {
										operation = FreshJsonSet(
										    manifest["modelCount"],
										    uint64_t{0},
										    manifest,
										    "manifest count"
										);
									}
									JsonArray models;
									if (operation) {
										operation = FreshJsonCreateArray(
										    manifest["models"],
										    manifest,
										    models,
										    "manifest models"
										);
									}
									if (operation) operation = writeManifest(manifest);
								}
								if (!operation) resetModelsLocked(false);
							}
						}
					}
				}
			}
		}
	}

	if (!preFormatFailure) return recoverBeforeFormat(preFormatFailure);
	if (!operation) {
		(void)FreshResetBackupAfterFormat(*_backup);
		markStopped();
		return operation;
	}
	if (!nativeFormatStarted) {
		return recoverBeforeFormat(
		    FreshResult::failure(FreshStatus::InternalError, "storage format did not start")
		);
	}

	FreshResult backupReset = FreshResetBackupAfterFormat(*_backup);
	if (!backupReset) {
		markStopped();
		return backupReset;
	}
	FreshResult restarted = startSyncTask();
	if (!restarted) return restarted;
	return FreshResult::success("storage formatted", modelCount);
}
