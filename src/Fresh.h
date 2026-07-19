#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <Print.h>
#include <Stream.h>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
	DocumentNotFound,
	InvalidModel,
	ValidationFailed,
	OutOfMemory,
	UnsupportedOperation,
	CorruptData,
	StorageFull,
	SizeLimitExceeded,
	Busy,
	BackupNotRunning,
	Cancelled,
	Timeout,
	InternalError,
};

enum class FreshCompressionType : uint8_t {
	MessagePack,
};

enum class FreshModelType : uint8_t {
	General,
	Stream,
};

enum class FreshReturn : uint8_t {
	None,
	ChangedDocs,
	AllDocs,
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

enum class FreshLoadStatus : uint8_t {
	LoadedOk,
	LoadedWithRecoveredJournal,
	LoadedWithCorruptSnapshot,
	LoadedWithCorruptJournal,
	FailedToLoad,
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
	size_t minFreeBytes = 4096;
	size_t maxDocumentBytes = 16 * 1024;
	size_t maxJournalRecordBytes = 32 * 1024;
	size_t maxSnapshotBytes = 256 * 1024;
};

struct FreshDeinitOptions {
	bool sync = true;
	// Total deadline for the complete explicit shutdown operation. UINT32_MAX
	// keeps destructor-style unbounded lifetime-barrier behavior.
	uint32_t timeoutMS = 2000;
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

enum class FreshBackupState : uint8_t {
	NotRunning,
	Queued,
	Running,
	Finished,
	Cancelled,
	Error,
};

struct FreshBackupStatus {
	FreshBackupState state = FreshBackupState::NotRunning;
	FreshResult result = FreshResult::failure(FreshStatus::BackupNotRunning, "backup not running");

	explicit operator bool() const {
		return static_cast<bool>(result);
	}
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

struct FreshModelLoadInfo {
	std::string modelName;
	FreshModelType modelType = FreshModelType::General;
	FreshLoadStatus status = FreshLoadStatus::LoadedOk;
	bool degraded = false;
	std::string message;
};

struct FreshDiagnostics {
	std::vector<FreshModelLoadInfo> modelLoads;
	size_t degradedModelCount = 0;
};

struct FreshModelInfo {
	std::string name;
	FreshModelType type = FreshModelType::General;
	size_t recordCount = 0;
};

struct FreshModelListResult {
	bool result = false;
	FreshStatus status = FreshStatus::InternalError;
	std::string message;
	std::vector<FreshModelInfo> models;
	size_t affectedCount = 0;

	explicit operator bool() const {
		return result;
	}
};

struct FreshRecordRetrieveOptions {
	size_t offset = 0;
	size_t limit = 0;
	bool reverse = false;
};

using FreshStreamRetrieveOptions = FreshRecordRetrieveOptions;

struct FreshStreamAppendOptions {
	// Zero keeps the stream unbounded. A positive value retains only the newest
	// maxEntries entries as part of the same append operation.
	size_t maxEntries = 0;
};

class Fresh;
class FreshModel;
class FreshBackupPrint;
struct FreshBackupRuntimeState;
struct FreshMutex;
struct FreshPendingRecord;

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

	std::string name() const;
	FreshModelType type() const;

	FreshResult setValidator(FreshBoolValidator validator);
	FreshResult setValidator(FreshResultValidator validator);

	FreshResult create(JsonDocument &doc);
	FreshResult append(JsonDocument &doc);
	FreshResult append(JsonDocument &doc, const FreshStreamAppendOptions &options);

	FreshResult findById(const char *id) const;
	FreshResult findById(const std::string &id) const;
	FreshResult find(FreshPredicate predicate, bool stopAtFirst = false) const;
	FreshResult listRecords(
	    const FreshRecordRetrieveOptions &options = FreshRecordRetrieveOptions()
	) const;

	template <typename T> FreshResult findOne(const char *field, const T &value) const {
		return find([field, value](const JsonDocument &doc) { return doc[field] == value; }, true);
	}

	FreshResult replaceById(const char *id, const JsonDocument &replacement);
	FreshResult replaceById(const std::string &id, const JsonDocument &replacement);
	FreshResult updateById(
	    const char *id,
	    const JsonDocument &patch,
	    FreshReturn returnMode = FreshReturn::None
	);
	FreshResult updateById(
	    const std::string &id,
	    const JsonDocument &patch,
	    FreshReturn returnMode = FreshReturn::None
	);
	FreshResult updateOne(
	    FreshPredicate predicate,
	    const JsonDocument &patch,
	    FreshReturn returnMode = FreshReturn::None
	);
	FreshResult update(
	    FreshPredicate predicate,
	    const JsonDocument &patch,
	    FreshReturn returnMode = FreshReturn::None
	);

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
	FreshResult validateLocked(
	    bool requireType = false,
	    FreshModelType requiredType = FreshModelType::General,
	    const char *unsupportedMessage = "unsupported operation"
	) const;

	Fresh *_owner = nullptr;
	std::shared_ptr<State> _state;
};

struct FreshModelResult {
	bool result = false;
	FreshStatus status = FreshStatus::InternalError;
	std::string message;
	FreshModel model;
	size_t affectedCount = 0;

	explicit operator bool() const {
		return result;
	}
};

class Fresh {
  public:
	Fresh();
	~Fresh();

	Fresh(const Fresh &) = delete;
	Fresh &operator=(const Fresh &) = delete;

	FreshResult init(const char *dbPath, const FreshConfig &config = FreshConfig());
	FreshResult deinit(const FreshDeinitOptions &options = FreshDeinitOptions());

	FreshModel model(const char *modelName);
	FreshModelListResult listModels() const;
	FreshModelResult createModel(const char *modelName);
	FreshModelResult createModel(const char *modelName, FreshModelType type);
	FreshResult dropModel(const char *modelName);
	FreshResult dropModels(std::initializer_list<const char *> modelNames);
	FreshResult dropAllModels();
	FreshResult renameModel(const char *oldName, const char *newName);

	FreshResult flush();
	FreshResult forceSyncAsync();
	FreshResult forceSync();

	FreshStorageInfo storageInfo() const;
	FreshDiagnostics diagnostics() const;

	FreshResult startBackup();
	size_t readBackup(uint8_t *buffer, size_t length, uint32_t timeoutMS = 0);
	FreshBackupStatus backupStatus() const;
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
	const char *backupStateToString(FreshBackupState state) const;
	const char *backupErrorToString(FreshBackupError error) const;
	const char *loadStatusToString(FreshLoadStatus status) const;
	const char *statusToString(FreshStatus status) const;

	// Internal checked staging boundary shared by the model implementation.
	// These are not application-level APIs and may change before v0.1.0.
	FreshResult checkPayloadSize(size_t payloadBytes, size_t limit, const char *label) const;
	FreshResult recordToJson(const FreshPendingRecord &record, JsonDocument &out);

  private:
	friend class FreshModel;
	friend class FreshBackupPrint;

	enum class Lifecycle : uint8_t {
		Uninitialized,
		Running,
		FinalSync,
		StopRequested,
		WaitingForTaskExit,
		Stopped,
	};

	static void syncTaskThunk(void *arg);

	void syncLoop();
	uint64_t now();
	void emitEvent(FreshEvent event);
	void emitSync(FreshResult result);

	FreshResult validateConfig(const FreshConfig &config) const;
	std::string modelPath(const std::string &storageId) const;
	std::string modelFile(const std::string &storageId, const char *fileName) const;
	FreshResult ensureDir(const std::string &path);
	FreshResult checkFreeSpace(size_t requiredBytes) const;
	FreshResult readManifest();
	FreshResult writeManifest(const JsonDocument &manifest);
	FreshResult applyRecord(const std::shared_ptr<FreshModel::State> &state, const FreshPendingRecord &record);
	FreshResult loadSnapshot(const std::shared_ptr<FreshModel::State> &state);
	FreshResult loadJournal(const std::shared_ptr<FreshModel::State> &state);
	FreshResult loadModel(const std::shared_ptr<FreshModel::State> &state);
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
	FreshDiagnostics _diagnostics;
	std::string _rootPath;
	Lifecycle _lifecycle = Lifecycle::Uninitialized;
	bool _initialized = false;
	bool _stopping = false;
	bool _stopTask = false;
	bool _manifestDirty = false;
	bool _forceSyncRequested = false;
	bool _syncTaskStarted = false;
	uint32_t _manifestEpoch = 0;
	uint64_t _nextPendingSequence = 1;
	uint64_t _databaseRevision = 1;
	TaskHandle_t _syncTaskHandle = nullptr;
	SemaphoreHandle_t _syncTaskExited = nullptr;
	std::map<std::string, std::shared_ptr<FreshModel::State>> _models;
	std::unique_ptr<FreshMutex> _mutex;
	std::unique_ptr<FreshMutex> _syncMutex;

	FreshSyncCallback _onSync;
	FreshEventCallback _onEvent;
	FreshTimeCallback _onTimeGet;
	FreshBackupCallback _onBackupStart;
	FreshBackupCallback _onBackupProgress;
	FreshBackupCallback _onBackupEnd;
	FreshBackupCallback _onBackupError;
	mutable std::unique_ptr<FreshBackupRuntimeState> _backup;
};
