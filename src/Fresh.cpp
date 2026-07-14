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

Fresh::Fresh()
    : _mutex(std::make_unique<FreshMutex>()),
      _syncMutex(std::make_unique<FreshMutex>()),
      _backup(std::make_unique<FreshBackupRuntimeState>()) {
	_syncTaskExited = xSemaphoreCreateBinary();
}

Fresh::~Fresh() {
	deinit(FreshDeinitOptions{.sync = true, .timeoutMS = UINT32_MAX});
	if (_syncTaskExited != nullptr) {
		vSemaphoreDelete(_syncTaskExited);
		_syncTaskExited = nullptr;
	}
}

FreshResult Fresh::validateConfig(const FreshConfig &config) const {
	if (_syncTaskExited == nullptr) {
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate sync task exit signal");
	}
	if (config.syncIntervalMS == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "sync interval must be greater than zero");
	}
	if (config.syncTaskStackSize == 0) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "sync task stack size must be greater than zero");
	}
	if (config.syncTaskPriority >= configMAX_PRIORITIES) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "sync task priority is out of range");
	}
	if (config.syncTaskCore != tskNO_AFFINITY &&
	    (config.syncTaskCore < 0 || config.syncTaskCore >= static_cast<BaseType_t>(portNUM_PROCESSORS))) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "sync task core is out of range");
	}
	if (config.compressionType != FreshCompressionType::MessagePack) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "unsupported compression type");
	}
	if (config.backupBufferSize == 0 || config.backupBufferSize > FreshMaxBackupBufferBytes) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "backup buffer size is out of range");
	}
	if (config.maxDocumentBytes == 0 || config.maxDocumentBytes > FreshMaxPersistedPayloadBytes) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "document size limit is out of range");
	}
	if (config.maxJournalRecordBytes == 0 ||
	    config.maxJournalRecordBytes > FreshMaxPersistedPayloadBytes) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "journal record size limit is out of range");
	}
	if (config.maxSnapshotBytes == 0 || config.maxSnapshotBytes > FreshMaxPersistedPayloadBytes) {
		return FreshResult::failure(FreshStatus::InvalidArgument, "snapshot size limit is out of range");
	}
	if (config.maxJournalRecordBytes <= config.maxDocumentBytes) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "journal record limit must exceed document limit"
		);
	}
	if (config.maxSnapshotBytes < config.maxDocumentBytes) {
		return FreshResult::failure(
		    FreshStatus::InvalidArgument,
		    "snapshot limit must be at least the document limit"
		);
	}
	return FreshResult::success("configuration valid");
}

FreshResult Fresh::init(const char *dbPath, const FreshConfig &config) {
	if (dbPath == nullptr || *dbPath == '\0') {
		return FreshResult::failure(FreshStatus::InvalidArgument, "db path is required");
	}
	FreshResult configResult = validateConfig(config);
	if (!configResult) {
		return configResult;
	}

	FreshLock lock(*_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	if (_initialized || _syncTaskStarted) {
		return FreshResult::failure(FreshStatus::AlreadyInitialized, "database already initialized");
	}

	auto resetInitState = [this]() {
		_models.clear();
		_diagnostics = FreshDiagnostics();
		_rootPath.clear();
		_stopping = false;
		_stopTask = false;
		_manifestDirty = false;
		_forceSyncRequested = false;
		_manifestEpoch = 0;
		_nextPendingSequence = 1;
		_syncTaskHandle = nullptr;
		_syncTaskStarted = false;
		_backup->buffer.reset();
		_backup->head = 0;
		_backup->tail = 0;
		_backup->used = 0;
		_backup->requested = false;
		_backup->running = false;
		_backup->done = false;
		_backup->cancelled = false;
		_backup->state = FreshBackupState::NotRunning;
		_backup->result = FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");
	};

	_config = config;
	_diagnostics = FreshDiagnostics();
	_models.clear();
	_stopping = false;
	_stopTask = false;
	_manifestDirty = false;
	_forceSyncRequested = false;
	_manifestEpoch = 0;
	_nextPendingSequence = 1;
	xSemaphoreTake(_syncTaskExited, 0);

	const size_t backupBytes = std::max<size_t>(config.backupBufferSize, 512);
	if (!_backup->buffer.allocate(backupBytes)) {
		resetInitState();
		return FreshResult::failure(FreshStatus::OutOfMemory, "failed to allocate backup buffer");
	}
	_backup->head = 0;
	_backup->tail = 0;
	_backup->used = 0;
	_backup->progress = 0;
	_backup->total = 0;
	_backup->lastProgressEvent = 0;
	_backup->requested = false;
	_backup->running = false;
	_backup->done = false;
	_backup->cancelled = false;
	_backup->state = FreshBackupState::NotRunning;
	_backup->result = FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");

	_rootPath = dbPath;
	if (_rootPath.back() == '/' && _rootPath.size() > 1) {
		_rootPath.pop_back();
	}

	if (!LittleFS.begin(false)) {
		if (!config.eraseOnFileSystemFailure || !LittleFS.begin(true)) {
			resetInitState();
			return FreshResult::failure(FreshStatus::FileSystemError, "failed to mount LittleFS");
		}
	}

	FreshResult dirResult = ensureDir(_rootPath);
	if (!dirResult) {
		resetInitState();
		return dirResult;
	}
	dirResult = ensureDir(FreshJoinPath(_rootPath, "models"));
	if (!dirResult) {
		resetInitState();
		return dirResult;
	}

	FreshResult manifestResult = readManifest();
	if (!manifestResult) {
		resetInitState();
		return manifestResult;
	}

	_initialized = true;
	_syncTaskStarted = true;

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
		resetInitState();
		return FreshResult::failure(FreshStatus::InternalError, "failed to create sync task");
	}

	if (_diagnostics.degradedModelCount > 0) {
		return FreshResult::success("database initialized with recovered models", _diagnostics.degradedModelCount);
	}
	return FreshResult::success("database initialized");
}

FreshResult Fresh::deinit(const FreshDeinitOptions &options) {
	TaskHandle_t handle = nullptr;
	bool taskStarted = false;
	FreshResult finalSyncResult = FreshResult::success("database deinitialized");
	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized && !_syncTaskStarted) {
			return FreshResult::success("database not initialized");
		}
		_stopping = true;
		handle = _syncTaskHandle;
		taskStarted = _syncTaskStarted;
	}

	{
		FreshLock backupLock(_backup->mutex);
		_backup->requested = false;
		_backup->cancelled = true;
		if (!_backup->running) {
			_backup->state = FreshBackupState::Cancelled;
			_backup->result = FreshResult::failure(FreshStatus::Cancelled, "backup cancelled");
		}
	}

	if (options.sync && _initialized) {
		finalSyncResult = syncDirty(true);
	}

	{
		FreshLock lock(*_mutex);
		_stopTask = true;
		handle = _syncTaskHandle;
		taskStarted = _syncTaskStarted;
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}

	if (taskStarted) {
		const TickType_t timeoutTicks =
		    options.timeoutMS == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(options.timeoutMS);
		if (xSemaphoreTake(_syncTaskExited, timeoutTicks) != pdTRUE) {
			return FreshResult::failure(FreshStatus::Timeout, "database deinit timed out");
		}
		if (handle != nullptr) {
			vTaskDelete(handle);
		}
	}

	{
		FreshLock lock(*_mutex);
		_models.clear();
		_diagnostics = FreshDiagnostics();
		_onSync = nullptr;
		_onEvent = nullptr;
		_onTimeGet = nullptr;
		_onBackupStart = nullptr;
		_onBackupProgress = nullptr;
		_onBackupEnd = nullptr;
		_onBackupError = nullptr;
		_forceSyncRequested = false;
		_manifestDirty = false;
		_manifestEpoch = 0;
		_nextPendingSequence = 1;
		_initialized = false;
		_stopping = false;
		_stopTask = false;
		_syncTaskHandle = nullptr;
		_syncTaskStarted = false;
		_rootPath.clear();
	}

	{
		FreshLock backupLock(_backup->mutex);
		_backup->buffer.reset();
		_backup->head = 0;
		_backup->tail = 0;
		_backup->used = 0;
		_backup->progress = 0;
		_backup->total = 0;
		_backup->lastProgressEvent = 0;
		_backup->requested = false;
		_backup->running = false;
		_backup->done = false;
		_backup->cancelled = false;
		_backup->state = FreshBackupState::NotRunning;
		_backup->result = FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");
	}

	if (!finalSyncResult) {
		return finalSyncResult;
	}
	return FreshResult::success("database deinitialized");
}

void Fresh::syncTaskThunk(void *arg) {
	static_cast<Fresh *>(arg)->syncLoop();
}

void Fresh::syncLoop() {
	while (true) {
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(_config.syncIntervalMS));
		bool force = false;
		{
			FreshLock lock(*_mutex);
			if (_stopTask) {
				break;
			}
			force = _forceSyncRequested;
			_forceSyncRequested = false;
		}
		syncDirty(force);
		runBackupIfRequested();
		{
			FreshLock lock(*_mutex);
			if (_stopTask) {
				break;
			}
		}
	}

	xSemaphoreGive(_syncTaskExited);
	vTaskSuspend(nullptr);
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
	FreshLock lock(*_mutex);
	FreshStorageInfo info;
	info.totalBytes = LittleFS.totalBytes();
	info.usedBytes = LittleFS.usedBytes();
	info.freeBytes = info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
	return info;
}

FreshDiagnostics Fresh::diagnostics() const {
	FreshLock lock(*_mutex);
	return _diagnostics;
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

const char *Fresh::backupStateToString(FreshBackupState state) const {
	switch (state) {
	case FreshBackupState::NotRunning:
		return "not running";
	case FreshBackupState::Queued:
		return "queued";
	case FreshBackupState::Running:
		return "running";
	case FreshBackupState::Finished:
		return "finished";
	case FreshBackupState::Cancelled:
		return "cancelled";
	case FreshBackupState::Error:
		return "error";
	}
	return "unknown";
}

const char *Fresh::loadStatusToString(FreshLoadStatus status) const {
	switch (status) {
	case FreshLoadStatus::LoadedOk:
		return "loaded ok";
	case FreshLoadStatus::LoadedWithRecoveredJournal:
		return "loaded with recovered journal";
	case FreshLoadStatus::LoadedWithCorruptSnapshot:
		return "loaded with corrupt snapshot";
	case FreshLoadStatus::LoadedWithCorruptJournal:
		return "loaded with corrupt journal";
	case FreshLoadStatus::FailedToLoad:
		return "failed to load";
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
	case FreshStatus::DocumentNotFound:
		return "document not found";
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
	case FreshStatus::StorageFull:
		return "storage full";
	case FreshStatus::SizeLimitExceeded:
		return "size limit exceeded";
	case FreshStatus::Busy:
		return "busy";
	case FreshStatus::BackupNotRunning:
		return "backup not running";
	case FreshStatus::Cancelled:
		return "cancelled";
	case FreshStatus::Timeout:
		return "timeout";
	case FreshStatus::InternalError:
		return "internal error";
	}
	return "unknown";
}
