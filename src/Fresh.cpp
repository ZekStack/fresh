#include "Fresh.h"
#include "internal/FreshInternal.h"

#include <LittleFS.h>
#include <algorithm>
#include <ctime>

FreshResult FreshResult::success(const char *message, size_t affectedCount) {
	FreshResult result;
	result.result = true;
	result.status = FreshStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	result.affectedCount = affectedCount;
	return result;
}

FreshResult FreshResult::failure(FreshStatus status, const char *message, size_t affectedCount) {
	FreshResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	result.affectedCount = affectedCount;
	return result;
}

Fresh::Fresh() : _mutex(std::make_unique<FreshMutex>()), _backup(std::make_unique<FreshBackupState>()) {
}

Fresh::~Fresh() {
	_stopTask = true;
	if (_syncTaskHandle != nullptr) {
		xTaskNotifyGive(_syncTaskHandle);
	}
}

FreshResult Fresh::init(const char *dbPath, const FreshConfig &config) {
	if (dbPath == nullptr || *dbPath == '\0') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "db path is required");
	}

	FreshLock lock(*_mutex);
	if (_initialized) {
		return FreshResult::failure(FreshStatus::AlreadyInitialized, "database already initialized");
	}

	_config = config;
	_rootPath = dbPath;
	if (_rootPath.back() == '/' && _rootPath.size() > 1) {
		_rootPath.pop_back();
	}

	if (!LittleFS.begin(false)) {
		if (!config.eraseOnFileSystemFailure || !LittleFS.begin(true)) {
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to mount LittleFS");
		}
	}

	FreshResult dirResult = ensureDir(_rootPath);
	if (!dirResult) {
		return dirResult;
	}

	FreshResult manifestResult = readManifest();
	if (!manifestResult) {
		return manifestResult;
	}

	_backup->buffer.resize(std::max<size_t>(config.backupBufferSize, 512));
	_initialized = true;

	BaseType_t taskResult = pdFAIL;
	if (config.syncTaskCore == tskNO_AFFINITY) {
		taskResult = xTaskCreate(
		    Fresh::syncTaskThunk,
		    "fresh-sync",
		    config.syncTaskStackSize,
		    this,
		    config.syncTaskPriority,
		    &_syncTaskHandle
		);
	} else {
		taskResult = xTaskCreatePinnedToCore(
		    Fresh::syncTaskThunk,
		    "fresh-sync",
		    config.syncTaskStackSize,
		    this,
		    config.syncTaskPriority,
		    &_syncTaskHandle,
		    config.syncTaskCore
		);
	}

	if (taskResult != pdPASS) {
		_initialized = false;
		return FreshResult::failure(FreshStatus::InternalError, "failed to create sync task");
	}

	return FreshResult::success("database initialized");
}

void Fresh::syncTaskThunk(void *arg) {
	static_cast<Fresh *>(arg)->syncLoop();
}

void Fresh::syncLoop() {
	while (!_stopTask) {
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(_config.syncIntervalMS));
		if (_stopTask) {
			break;
		}
		syncDirty(false);
		runBackupIfRequested();
	}
	vTaskDelete(nullptr);
}

uint64_t Fresh::now() {
	FreshTimeCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onTimeGet;
	}
	if (callback) {
		return callback();
	}
	time_t current = time(nullptr);
	if (current > 0) {
		return static_cast<uint64_t>(current);
	}
	return static_cast<uint64_t>(millis());
}

void Fresh::emitEvent(FreshEvent event) {
	FreshEventCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onEvent;
	}
	if (callback) {
		callback(event);
	}
}

void Fresh::emitSync(FreshResult result) {
	FreshSyncCallback callback;
	{
		FreshLock lock(*_mutex);
		callback = _onSync;
	}
	if (callback) {
		callback(result);
	}
}

FreshStorageInfo Fresh::storageInfo() const {
	FreshStorageInfo info;
	info.totalBytes = LittleFS.totalBytes();
	info.usedBytes = LittleFS.usedBytes();
	info.freeBytes = info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
	return info;
}

void Fresh::onSync(FreshSyncCallback callback) {
	FreshLock lock(*_mutex);
	_onSync = callback;
}

void Fresh::onEvent(FreshEventCallback callback) {
	FreshLock lock(*_mutex);
	_onEvent = callback;
}

void Fresh::onTimeGet(FreshTimeCallback callback) {
	FreshLock lock(*_mutex);
	_onTimeGet = callback;
}

void Fresh::onBackupStart(FreshBackupCallback callback) {
	FreshLock lock(*_mutex);
	_onBackupStart = callback;
}

void Fresh::onBackupProgress(FreshBackupCallback callback) {
	FreshLock lock(*_mutex);
	_onBackupProgress = callback;
}

void Fresh::onBackupEnd(FreshBackupCallback callback) {
	FreshLock lock(*_mutex);
	_onBackupEnd = callback;
}

void Fresh::onBackupError(FreshBackupCallback callback) {
	FreshLock lock(*_mutex);
	_onBackupError = callback;
}

const char *Fresh::eventToString(FreshEventType type) const {
	switch (type) {
	case FreshEventType::ModelCreated:
		return "modelCreated";
	case FreshEventType::ModelDropped:
		return "modelDropped";
	case FreshEventType::ModelRenamed:
		return "modelRenamed";
	case FreshEventType::DocumentCreated:
		return "documentCreated";
	case FreshEventType::DocumentUpdated:
		return "documentUpdated";
	case FreshEventType::DocumentDeleted:
		return "documentDeleted";
	case FreshEventType::StreamAppended:
		return "streamAppended";
	case FreshEventType::SyncStarted:
		return "syncStarted";
	case FreshEventType::SyncFinished:
		return "syncFinished";
	case FreshEventType::BackupStarted:
		return "backupStarted";
	case FreshEventType::BackupFinished:
		return "backupFinished";
	case FreshEventType::BackupCancelled:
		return "backupCancelled";
	case FreshEventType::BackupError:
		return "backupError";
	}
	return "unknown";
}

const char *Fresh::backupErrorToString(FreshBackupError error) const {
	switch (error) {
	case FreshBackupError::None:
		return "none";
	case FreshBackupError::AlreadyRunning:
		return "already running";
	case FreshBackupError::NotRunning:
		return "not running";
	case FreshBackupError::Cancelled:
		return "cancelled";
	case FreshBackupError::SerializationFailed:
		return "serialization failed";
	case FreshBackupError::FileSystemError:
		return "file system error";
	case FreshBackupError::OutOfMemory:
		return "out of memory";
	}
	return "unknown";
}

const char *Fresh::statusToString(FreshStatus status) const {
	switch (status) {
	case FreshStatus::Ok:
		return "ok";
	case FreshStatus::NotInitialized:
		return "not initialized";
	case FreshStatus::AlreadyInitialized:
		return "already initialized";
	case FreshStatus::InvalidArgument:
		return "invalid argument";
	case FreshStatus::FileSystemError:
		return "file system error";
	case FreshStatus::ModelExists:
		return "model exists";
	case FreshStatus::ModelNotFound:
		return "model not found";
	case FreshStatus::InvalidModel:
		return "invalid model";
	case FreshStatus::ValidationFailed:
		return "validation failed";
	case FreshStatus::OutOfMemory:
		return "out of memory";
	case FreshStatus::UnsupportedOperation:
		return "unsupported operation";
	case FreshStatus::CorruptData:
		return "corrupt data";
	case FreshStatus::Busy:
		return "busy";
	case FreshStatus::BackupNotRunning:
		return "backup not running";
	case FreshStatus::Cancelled:
		return "cancelled";
	case FreshStatus::InternalError:
		return "internal error";
	}
	return "unknown";
}
