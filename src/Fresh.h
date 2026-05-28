#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <Print.h>
#include <Stream.h>
#include <functional>
#include <memory>
#include <string>
#include <map>

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

enum class FreshStatus : uint8_t {
	Ok,
	NotInitialized,
	AlreadyInitialized,
	InvalidArgument,
	FileSystemError,
	ModelExists,
	ModelNotFound,
	InvalidModel,
	ValidationFailed,
	OutOfMemory,
	UnsupportedOperation,
	CorruptData,
	Busy,
	BackupNotRunning,
	Cancelled,
	InternalError,
};

enum class FreshCompressionType : uint8_t {
	MessagePack,
};

enum class FreshModelType : uint8_t {
	General,
	Stream,
};

enum class FreshEventType : uint8_t {
	ModelCreated,
	ModelDropped,
	ModelRenamed,
	DocumentCreated,
	DocumentUpdated,
	DocumentDeleted,
	StreamAppended,
	SyncStarted,
	SyncFinished,
	BackupStarted,
	BackupFinished,
	BackupCancelled,
	BackupError,
};

struct FreshConfig {
	uint32_t syncIntervalMS = 5000;
	UBaseType_t syncTaskPriority = 1;
	BaseType_t syncTaskCore = tskNO_AFFINITY;
	uint32_t syncTaskStackSize = 8192;
	bool eraseOnFileSystemFailure = false;
	FreshCompressionType compressionType = FreshCompressionType::MessagePack;
	FreshModelType defaultModelType = FreshModelType::General;
	uint32_t snapshotRecordThreshold = 128;
	size_t snapshotBytesThreshold = 32 * 1024;
	size_t backupBufferSize = 8 * 1024;
};

struct FreshResult {
	bool result = false;
	FreshStatus status = FreshStatus::InternalError;
	std::string message;
	JsonDocument doc;
	size_t affectedCount = 0;

	explicit operator bool() const {
		return result;
	}

	static FreshResult success(const char *message = "ok", size_t affectedCount = 0);
	static FreshResult failure(FreshStatus status, const char *message, size_t affectedCount = 0);
};

struct FreshValidationResult {
	bool result = false;
	std::string message;

	explicit operator bool() const {
		return result;
	}
};

struct FreshEvent {
	FreshEventType type = FreshEventType::SyncFinished;
	std::string modelName;
	std::string previousModelName;
	std::string documentId;
	size_t affectedCount = 0;
	FreshResult result;
};

enum class FreshBackupError : uint8_t {
	None,
	AlreadyRunning,
	NotRunning,
	Cancelled,
	SerializationFailed,
	FileSystemError,
	OutOfMemory,
};

struct FreshBackupInfo {
	size_t progress = 0;
	size_t total = 0;
	size_t size = 0;
	size_t estimatedSize = 0;
	FreshBackupError error = FreshBackupError::None;
	FreshResult result;
};

struct FreshStorageInfo {
	size_t totalBytes = 0;
	size_t usedBytes = 0;
	size_t freeBytes = 0;
};

struct FreshStreamRetrieveOptions {
	size_t offset = 0;
	size_t limit = 0;
	bool reverse = false;
};

class Fresh;
class FreshModel;
class FreshBackupPrint;

using FreshPredicate = std::function<bool(const JsonDocument &)>;
using FreshBoolValidator = std::function<bool(const JsonDocument &)>;
using FreshResultValidator = std::function<FreshValidationResult(const JsonDocument &)>;
using FreshEventCallback = std::function<void(FreshEvent)>;
using FreshSyncCallback = std::function<void(FreshResult)>;
using FreshBackupCallback = std::function<void(FreshBackupInfo)>;
using FreshTimeCallback = std::function<uint64_t()>;

class FreshModel {
  public:
	FreshModel() = default;

	explicit operator bool() const;

	const std::string &name() const;
	FreshModelType type() const;

	FreshResult setValidator(FreshBoolValidator validator);
	FreshResult setValidator(FreshResultValidator validator);

	FreshResult create(JsonDocument &doc);
	FreshResult append(JsonDocument &doc);

	FreshResult findById(const char *id) const;
	FreshResult findById(const std::string &id) const;
	FreshResult find(FreshPredicate predicate, bool stopAtFirst = false) const;

	template <typename T> FreshResult findOne(const char *field, const T &value) const {
		return find([field, value](const JsonDocument &doc) { return doc[field] == value; }, true);
	}

	FreshResult updateById(const char *id, const JsonDocument &patch);
	FreshResult updateById(const std::string &id, const JsonDocument &patch);
	FreshResult updateOne(FreshPredicate predicate, const JsonDocument &patch);
	FreshResult update(FreshPredicate predicate, const JsonDocument &patch);

	FreshResult deleteById(const char *id);
	FreshResult deleteById(const std::string &id);
	FreshResult deleteOne(FreshPredicate predicate);
	FreshResult deleteMany(FreshPredicate predicate);

	FreshResult retrieve() const;
	FreshResult retrieve(const FreshStreamRetrieveOptions &options) const;
	FreshResult retrieve(
	    FreshPredicate predicate,
	    const FreshStreamRetrieveOptions &options = FreshStreamRetrieveOptions()
	) const;
	FreshResult streamTo(Print &out) const;

  private:
	friend class Fresh;
	struct State;

	FreshModel(Fresh *owner, std::shared_ptr<State> state);

	Fresh *_owner = nullptr;
	std::shared_ptr<State> _state;
};

#include "internal/FreshInternal.h"

class Fresh {
  public:
	Fresh();
	~Fresh();

	Fresh(const Fresh &) = delete;
	Fresh &operator=(const Fresh &) = delete;

	FreshResult init(const char *dbPath, const FreshConfig &config = FreshConfig());

	FreshModel model(const char *modelName);
	FreshModel createModel(const char *modelName);
	FreshModel createModel(const char *modelName, FreshModelType type);
	FreshResult dropModel(const char *modelName);
	FreshResult dropModels(std::initializer_list<const char *> modelNames);
	FreshResult dropAllModels();
	FreshResult renameModel(const char *oldName, const char *newName);

	FreshResult forceSyncAsync();
	FreshResult forceSync();

	FreshStorageInfo storageInfo() const;

	FreshResult startBackup();
	size_t readBackup(uint8_t *buffer, size_t length, uint32_t timeoutMS = 0);
	FreshResult backupStatus() const;
	FreshResult cancelBackup();
	FreshResult backupImport(Stream &input);
	FreshResult backupImport(const uint8_t *data, size_t length);

	void onSync(FreshSyncCallback callback);
	void onEvent(FreshEventCallback callback);
	void onTimeGet(FreshTimeCallback callback);
	void onBackupStart(FreshBackupCallback callback);
	void onBackupProgress(FreshBackupCallback callback);
	void onBackupEnd(FreshBackupCallback callback);
	void onBackupError(FreshBackupCallback callback);

	const char *eventToString(FreshEventType type) const;
	const char *backupErrorToString(FreshBackupError error) const;
	const char *statusToString(FreshStatus status) const;

  private:
	friend class FreshModel;
	friend class FreshBackupPrint;

	static void syncTaskThunk(void *arg);

	void syncLoop();
	uint64_t now();
	void emitEvent(FreshEvent event);
	void emitSync(FreshResult result);

	std::string modelPath(const std::string &name) const;
	std::string modelFile(const std::string &name, const char *fileName) const;
	FreshResult ensureDir(const std::string &path);
	FreshResult readManifest();
	FreshResult writeManifest();
	FreshResult applyRecord(const std::shared_ptr<FreshModel::State> &state, const FreshPendingRecord &record);
	FreshResult loadSnapshot(const std::shared_ptr<FreshModel::State> &state);
	FreshResult loadJournal(const std::shared_ptr<FreshModel::State> &state);
	FreshResult loadModel(const std::shared_ptr<FreshModel::State> &state);
	JsonDocument recordToJson(const FreshPendingRecord &record);
	FreshResult appendJournalRecord(
	    const std::shared_ptr<FreshModel::State> &state,
	    const FreshPendingRecord &record
	);
	FreshResult writeSnapshot(const std::shared_ptr<FreshModel::State> &state);
	FreshResult syncModel(const std::shared_ptr<FreshModel::State> &state);
	FreshResult syncDirty(bool force);

	bool backupWriteByte(uint8_t byte);
	void runBackupIfRequested();
	FreshResult importBackupArchive(const JsonDocument &archive);
	bool isBackupCancelled();
	size_t estimateBackupSize();
	void callBackupStart(FreshBackupInfo info);
	void callBackupProgress(FreshBackupInfo info);
	void callBackupEnd(FreshBackupInfo info);
	void callBackupError(FreshBackupInfo info);

	FreshConfig _config;
	std::string _rootPath;
	bool _initialized = false;
	bool _stopTask = false;
	bool _manifestDirty = false;
	TaskHandle_t _syncTaskHandle = nullptr;
	std::map<std::string, std::shared_ptr<FreshModel::State>> _models;
	FreshMutex _mutex;

	FreshSyncCallback _onSync;
	FreshEventCallback _onEvent;
	FreshTimeCallback _onTimeGet;
	FreshBackupCallback _onBackupStart;
	FreshBackupCallback _onBackupProgress;
	FreshBackupCallback _onBackupEnd;
	FreshBackupCallback _onBackupError;
	mutable FreshBackupState _backup;
};
