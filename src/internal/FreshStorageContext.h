#pragma once

#include "../FreshFile.h"
#include "../FreshStorage.h"

#include <cstddef>

class FreshStorageFileSystem {
  public:
	FreshFile open(const char *path, const char *mode) const;
	FreshResult exists(const char *path, bool &result) const;
	bool exists(const char *path) const;
	bool mkdir(const char *path) const;
	bool remove(const char *path) const;
	bool rmdir(const char *path) const;
	size_t totalBytes() const;
	size_t usedBytes() const;
	FreshStorage *storage() const;
};

class FreshStorageScope {
  public:
	explicit FreshStorageScope(FreshStorage *storage);
	~FreshStorageScope();

	FreshStorageScope(const FreshStorageScope &) = delete;
	FreshStorageScope &operator=(const FreshStorageScope &) = delete;

  private:
	FreshStorage *_previous = nullptr;
};

FreshStorageFileSystem &FreshCurrentFileSystem();
FreshStorage *FreshCurrentStorage();
bool FreshHasInternalStorageAccess(const FreshStorage *storage);
