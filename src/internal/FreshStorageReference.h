#pragma once

#include "../FreshStorage.h"

class FreshStorageReference final : public FreshStorage {
  public:
	explicit FreshStorageReference(FreshStorage &target);

	const char *name() const override;
	FreshStorageInfo info() const override;
	int nativeError() const override;

  private:
	FreshResult mount() override;
	FreshResult unmount() override;
	FreshResult openBackend(
	    const char *logicalPath,
	    FreshOpenMode mode,
	    std::unique_ptr<FreshFileBackend> &backend
	) override;
	FreshResult existsBackend(const char *logicalPath, bool &result) const override;
	FreshResult createDirectoryBackend(const char *logicalPath) override;
	FreshResult removeFileBackend(const char *logicalPath) override;
	FreshResult removeDirectoryBackend(const char *logicalPath) override;
	FreshResult listDirectoryBackend(
	    const char *logicalPath,
	    std::vector<FreshDirectoryEntry> &entries
	) const override;

	FreshStorage &_target;
};
