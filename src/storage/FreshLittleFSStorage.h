#pragma once

#include "../FreshStorage.h"

#include <string>

class FreshLittleFSStorage final : public FreshStorage {
  public:
	explicit FreshLittleFSStorage(
	    const FreshLittleFSConfig &config = FreshLittleFSConfig()
	);

	const char *name() const override;
	FreshStorageInfo info() const override;
	int nativeError() const override {
		return _nativeError;
	}

  private:
	FreshResult mount() override;
	FreshResult unmount() override;
	FreshResult readInfoBackend(FreshStorageInfo &result) const override;

	FreshLittleFSConfig _config;
	std::string _partitionLabel;
	mutable int _nativeError = 0;
};
