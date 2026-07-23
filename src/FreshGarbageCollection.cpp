#include "Fresh.h"
#include "FreshGarbageCollectionTesting.h"
#include "internal/FreshInternal.h"
#include "internal/FreshMemory.h"

#include <LittleFS.h>

#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class FreshGarbageCollectionSlotStatus : uint8_t {
	Missing,
	Valid,
	Corrupt,
	Unavailable,
};

struct FreshGarbageCollectionManifestSlot {
	FreshGarbageCollectionSlotStatus status = FreshGarbageCollectionSlotStatus::Missing;
	uint64_t generation = 0;
	JsonDocument payload{&FreshJsonAllocator()};
	FreshResult result = FreshResult::success("manifest slot missing");
};

std::string FreshGarbageCollectionSlotPath(
    const std::string &rootPath,
    char slot
) {
	std::string fileName = FreshManifestFile;
	fileName += ".";
	fileName.push_back(slot);
	fileName += ".msgpack";
	return FreshJoinPath(rootPath, fileName);
}

bool FreshIsStorageId(const std::string &name) {
	if (name.size() != 16) return false;
	for (char c : name) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return true;
}

std::string FreshBaseName(const std::string &path) {
	const size_t separator = path.find_last_of('/');
	return separator == std::string::npos ? path : path.substr(separator + 1);
}

FreshResult FreshValidateGarbageCollectionManifest(
    const JsonDocument &manifest,
    std::set<std::string> &storageIds
) {
	storageIds.clear();
	if ((manifest["version"] | 0U) != FreshManifestVersion ||
	    !manifest["modelCount"].is<uint64_t>() ||
	    !manifest["models"].is<JsonArrayConst>()) {
		return FreshResult::failure(FreshStatus::CorruptData, "invalid garbage collection manifest");
	}

	const uint64_t declaredCount = manifest["modelCount"].as<uint64_t>();
	JsonArrayConst models = manifest["models"].as<JsonArrayConst>();
	if (declaredCount > SIZE_MAX || static_cast<size_t>(declaredCount) != models.size()) {
		return FreshResult::failure(FreshStatus::CorruptData, "garbage collection manifest count mismatch");
	}

	std::set<std::string> names;
	for (JsonObjectConst model : models) {
		const char *name = model["name"] | "";
		const char *storageId = model["storageId"] | "";
		const char *type = model["type"] | "";
		if (!FreshIsValidName(name) || !FreshIsValidName(storageId) ||
		    (strcmp(type, "general") != 0 && strcmp(type, "stream") != 0)) {
			return FreshResult::failure(
			    FreshStatus::CorruptData,
			    "garbage collection manifest contains invalid model metadata"
			);
		}
		if (!names.insert(name).second || !storageIds.insert(storageId).second) {
			return FreshResult::failure(
			    FreshStatus::CorruptData,
			    "garbage collection manifest contains duplicate model metadata"
			);
		}
	}
	return FreshResult::success("garbage collection manifest valid");
}

FreshGarbageCollectionManifestSlot FreshReadGarbageCollectionManifestSlot(
    const std::string &path
) {
	FreshGarbageCollectionManifestSlot slot;
	if (!LittleFS.exists(path.c_str())) return slot;

	File file = LittleFS.open(path.c_str(), "r");
	if (!file) {
		slot.status = FreshGarbageCollectionSlotStatus::Unavailable;
		slot.result = FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to open manifest slot for garbage collection"
		);
		return slot;
	}

	uint32_t magic = 0;
	uint16_t version = 0;
	uint64_t generation = 0;
	uint32_t payloadSize = 0;
	uint32_t expectedChecksum = 0;
	const bool headerOk = FreshReadU32(file, magic) && FreshReadU16(file, version) &&
	                      FreshReadU64(file, generation) && FreshReadU32(file, payloadSize) &&
	                      FreshReadU32(file, expectedChecksum);
	if (!headerOk || magic != FreshSlotMagic || version != FreshSlotVersion ||
	    payloadSize == 0 || payloadSize > FreshMaxPersistedPayloadBytes ||
	    file.available() != static_cast<int>(payloadSize)) {
		file.close();
		slot.status = FreshGarbageCollectionSlotStatus::Corrupt;
		slot.result = FreshResult::failure(
		    FreshStatus::CorruptData,
		    "invalid manifest slot for garbage collection"
		);
		return slot;
	}

	FreshBuffer bytes;
	if (!bytes.allocate(payloadSize, FreshAllocationCategory::DurableSlotPayload)) {
		file.close();
		slot.status = FreshGarbageCollectionSlotStatus::Unavailable;
		slot.result = FreshResult::failure(
		    FreshStatus::OutOfMemory,
		    "failed to allocate garbage collection manifest payload"
		);
		return slot;
	}

	const int read = file.read(bytes.data(), bytes.size());
	const bool trailingBytes = file.available() != 0;
	file.close();
	if (read != static_cast<int>(bytes.size()) || trailingBytes ||
	    FreshChecksum(bytes.data(), bytes.size()) != expectedChecksum) {
		slot.status = FreshGarbageCollectionSlotStatus::Corrupt;
		slot.result = FreshResult::failure(
		    FreshStatus::CorruptData,
		    "manifest slot checksum failed during garbage collection"
		);
		return slot;
	}

	JsonDocument decoded(&FreshJsonAllocator());
	DeserializationError decode = deserializeMsgPack(decoded, bytes.data(), bytes.size());
	if (decode || decoded.overflowed()) {
		if (decode == DeserializationError::NoMemory || decoded.overflowed()) {
			slot.status = FreshGarbageCollectionSlotStatus::Unavailable;
			slot.result = FreshResult::failure(
			    FreshStatus::OutOfMemory,
			    "failed to decode garbage collection manifest"
			);
		} else {
			slot.status = FreshGarbageCollectionSlotStatus::Corrupt;
			slot.result = FreshResult::failure(
			    FreshStatus::CorruptData,
			    "failed to decode garbage collection manifest"
			);
		}
		return slot;
	}

	std::set<std::string> ignoredStorageIds;
	FreshResult valid = FreshValidateGarbageCollectionManifest(decoded, ignoredStorageIds);
	if (!valid) {
		slot.status = FreshGarbageCollectionSlotStatus::Corrupt;
		slot.result = valid;
		return slot;
	}

	slot.status = FreshGarbageCollectionSlotStatus::Valid;
	slot.generation = generation;
	slot.payload = std::move(decoded);
	slot.result = FreshResult::success("manifest slot loaded for garbage collection");
	return slot;
}

bool FreshRemoveGarbageCollectionTree(const std::string &path) {
	File entry = LittleFS.open(path.c_str(), "r");
	if (!entry) return !LittleFS.exists(path.c_str());

	if (!entry.isDirectory()) {
		entry.close();
		return LittleFS.remove(path.c_str());
	}

	std::vector<std::string> children;
	File child = entry.openNextFile();
	while (child) {
		const std::string childName = FreshBaseName(child.name());
		children.push_back(FreshJoinPath(path, childName));
		child.close();
		child = entry.openNextFile();
	}
	entry.close();

	for (const std::string &childPath : children) {
		if (!FreshRemoveGarbageCollectionTree(childPath)) return false;
	}
	return !LittleFS.exists(path.c_str()) || LittleFS.rmdir(path.c_str());
}

#if defined(FRESH_TESTING)
FreshGarbageCollectionTestFailurePoint FreshGarbageCollectionFailurePoint =
    FreshGarbageCollectionTestFailurePoint::None;

bool FreshGarbageCollectionShouldFail(FreshGarbageCollectionTestFailurePoint point) {
	if (FreshGarbageCollectionFailurePoint != point) return false;
	FreshGarbageCollectionFailurePoint = FreshGarbageCollectionTestFailurePoint::None;
	return true;
}
#else
bool FreshGarbageCollectionShouldFail(FreshGarbageCollectionTestFailurePoint) {
	return false;
}
#endif

void FreshStoreGarbageCollectionDiagnostics(
    FreshDiagnostics &diagnostics,
    bool attempted,
    const FreshResult &collectionResult,
    const FreshGarbageCollectionResult &result
) {
	diagnostics.garbageCollection.attempted = attempted;
	diagnostics.garbageCollection.degraded = !collectionResult;
	diagnostics.garbageCollection.message = collectionResult.message;
	diagnostics.garbageCollection.result = result;
}

} // namespace

#if defined(FRESH_TESTING)
void FreshTestConfigureGarbageCollectionFailure(
    FreshGarbageCollectionTestFailurePoint point
) {
	FreshGarbageCollectionFailurePoint = point;
}

void FreshTestResetGarbageCollectionFailure() {
	FreshGarbageCollectionFailurePoint = FreshGarbageCollectionTestFailurePoint::None;
}
#endif

FreshResult FreshCollectGarbageStorage(
    const std::string &rootPath,
    FreshGarbageCollectionResult &result,
    bool &attempted
) {
	result = FreshGarbageCollectionResult();
	attempted = false;

	const std::string slotAPath = FreshGarbageCollectionSlotPath(rootPath, 'a');
	const std::string slotBPath = FreshGarbageCollectionSlotPath(rootPath, 'b');
	const bool slotAExists = LittleFS.exists(slotAPath.c_str());
	const bool slotBExists = LittleFS.exists(slotBPath.c_str());
	if (!slotAExists && !slotBExists) {
		return FreshResult::success("garbage collection skipped; manifest not found");
	}
	attempted = true;

	FreshGarbageCollectionManifestSlot slotA =
	    FreshReadGarbageCollectionManifestSlot(slotAPath);
	FreshGarbageCollectionManifestSlot slotB =
	    FreshReadGarbageCollectionManifestSlot(slotBPath);

	if (slotA.status == FreshGarbageCollectionSlotStatus::Unavailable) return slotA.result;
	if (slotB.status == FreshGarbageCollectionSlotStatus::Unavailable) return slotB.result;

	const FreshGarbageCollectionManifestSlot *activeSlot = nullptr;
	if (slotA.status == FreshGarbageCollectionSlotStatus::Valid) activeSlot = &slotA;
	if (slotB.status == FreshGarbageCollectionSlotStatus::Valid &&
	    (activeSlot == nullptr || slotB.generation > activeSlot->generation)) {
		activeSlot = &slotB;
	}
	if (activeSlot == nullptr) {
		return FreshResult::failure(
		    FreshStatus::CorruptData,
		    "garbage collection skipped; no valid manifest slot"
		);
	}

	std::set<std::string> referencedStorageIds;
	FreshResult manifestResult = FreshValidateGarbageCollectionManifest(
	    activeSlot->payload,
	    referencedStorageIds
	);
	if (!manifestResult) return manifestResult;

	const std::string modelsPath = FreshJoinPath(rootPath, "models");
	File models = LittleFS.open(modelsPath.c_str(), "r");
	if (!models || !models.isDirectory()) {
		if (models) models.close();
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "failed to open models directory for garbage collection"
		);
	}

	std::vector<std::string> candidates;
	File child = models.openNextFile();
	while (child) {
		const bool isDirectory = child.isDirectory();
		const std::string name = FreshBaseName(child.name());
		child.close();

		if (isDirectory) {
			result.scannedDirectories++;
			if (FreshIsStorageId(name) &&
			    referencedStorageIds.find(name) == referencedStorageIds.end()) {
				candidates.push_back(FreshJoinPath(modelsPath, name));
			}
		}
		child = models.openNextFile();
	}
	models.close();

	for (const std::string &candidate : candidates) {
		if (FreshGarbageCollectionShouldFail(
		        FreshGarbageCollectionTestFailurePoint::BeforeCandidateRemoval
		    )) {
			result.failedDirectories++;
			continue;
		}

		const size_t usedBefore = LittleFS.usedBytes();
		const bool removed = FreshRemoveGarbageCollectionTree(candidate);
		const size_t usedAfter = LittleFS.usedBytes();
		if (usedBefore > usedAfter) {
			const size_t reclaimed = usedBefore - usedAfter;
			if (reclaimed > std::numeric_limits<size_t>::max() - result.reclaimedBytes) {
				result.reclaimedBytes = std::numeric_limits<size_t>::max();
			} else {
				result.reclaimedBytes += reclaimed;
			}
		}

		if (removed && !LittleFS.exists(candidate.c_str())) {
			result.removedDirectories++;
		} else {
			result.failedDirectories++;
		}
	}

	if (result.failedDirectories > 0) {
		return FreshResult::failure(
		    FreshStatus::FileSystemError,
		    "garbage collection completed with cleanup failures",
		    result.removedDirectories
		);
	}
	return FreshResult::success(
	    result.removedDirectories > 0
	        ? "orphaned model storage collected"
	        : "no orphaned model storage found",
	    result.removedDirectories
	);
}

FreshResult Fresh::collectGarbage(FreshGarbageCollectionResult &result) {
	result = FreshGarbageCollectionResult();

	FreshLock backupLock(_backup->mutex);
	if (!backupLock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock backup state");
	}
	if (_backup->running || _backup->requested) {
		return FreshResult::failure(FreshStatus::Busy, "backup already running");
	}

	{
		FreshLock lock(*_mutex);
		if (!lock) {
			return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
		}
		if (!_initialized) {
			return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
		}
		if (_stopping || _lifecycle != Lifecycle::Running) {
			return FreshResult::failure(FreshStatus::Busy, "database is stopping");
		}
	}

	FreshResult syncResult = forceSync();
	if (!syncResult) return syncResult;

	FreshLock syncLock(*_syncMutex);
	if (!syncLock) {
		return FreshResult::failure(
		    FreshStatus::InternalError,
		    "failed to lock storage for garbage collection"
		);
	}
	FreshLock lock(*_mutex);
	if (!lock) {
		return FreshResult::failure(FreshStatus::InternalError, "failed to lock database");
	}
	if (!_initialized) {
		return FreshResult::failure(FreshStatus::NotInitialized, "database not initialized");
	}
	if (_stopping || _lifecycle != Lifecycle::Running) {
		return FreshResult::failure(FreshStatus::Busy, "database is stopping");
	}

	bool attempted = false;
	FreshResult collectionResult = FreshCollectGarbageStorage(
	    _rootPath,
	    result,
	    attempted
	);
	FreshStoreGarbageCollectionDiagnostics(
	    _diagnostics,
	    attempted,
	    collectionResult,
	    result
	);
	return collectionResult;
}
