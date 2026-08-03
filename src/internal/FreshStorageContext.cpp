#include "FreshStorageContext.h"

#include "../Fresh.h"

#include <cstring>

namespace {

thread_local FreshStorage *FreshActiveStorage = nullptr;
FreshStorageFileSystem FreshActiveFileSystem;

bool FreshModeToOpenMode(const char *mode, FreshOpenMode &openMode) {
	if (mode == nullptr || *mode == '\0') return false;
	if (strchr(mode, 'a') != nullptr) {
		openMode = FreshOpenMode::Append;
		return true;
	}
	if (strchr(mode, 'w') != nullptr) {
		openMode = FreshOpenMode::Write;
		return true;
	}
	if (strchr(mode, 'r') != nullptr) {
		openMode = FreshOpenMode::Read;
		return true;
	}
	return false;
}

} // namespace

FreshStorageScope::FreshStorageScope(FreshStorage *storage) : _previous(FreshActiveStorage) {
	FreshActiveStorage = storage;
}

FreshStorageScope::~FreshStorageScope() {
	FreshActiveStorage = _previous;
}

FreshStorageFileSystem &FreshCurrentFileSystem() {
	return FreshActiveFileSystem;
}

FreshStorage *FreshCurrentStorage() {
	return FreshActiveStorage;
}

bool FreshHasInternalStorageAccess(const FreshStorage *storage) {
	return storage != nullptr && FreshActiveStorage == storage;
}

FreshStorage *FreshStorageFileSystem::storage() const {
	return FreshActiveStorage;
}

FreshFile FreshStorageFileSystem::open(const char *path, const char *mode) const {
	FreshFile file;
	if (FreshActiveStorage == nullptr) return file;
	FreshOpenMode openMode = FreshOpenMode::Read;
	if (!FreshModeToOpenMode(mode, openMode)) return file;
	FreshActiveStorage->open(path, openMode, file);
	return file;
}

FreshResult FreshStorageFileSystem::exists(const char *path, bool &result) const {
	result = false;
	if (FreshActiveStorage == nullptr) {
		return FreshResult::failure(FreshStatus::StorageUnavailable, "storage is unavailable");
	}
	return FreshActiveStorage->exists(path, result);
}

bool FreshStorageFileSystem::mkdir(const char *path) const {
	return FreshActiveStorage != nullptr && FreshActiveStorage->createDirectory(path);
}

bool FreshStorageFileSystem::remove(const char *path) const {
	return FreshActiveStorage != nullptr && FreshActiveStorage->removeFile(path);
}

bool FreshStorageFileSystem::rmdir(const char *path) const {
	return FreshActiveStorage != nullptr && FreshActiveStorage->removeDirectory(path);
}

size_t FreshStorageFileSystem::totalBytes() const {
	return FreshActiveStorage != nullptr ? FreshActiveStorage->info().totalBytes : 0;
}

size_t FreshStorageFileSystem::usedBytes() const {
	return FreshActiveStorage != nullptr ? FreshActiveStorage->info().usedBytes : 0;
}
