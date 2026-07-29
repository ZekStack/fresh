#pragma once

#include "../FreshStorage.h"

class FreshLittleFSStorage final : public FreshStorage {
  public:
	explicit FreshLittleFSStorage(const FreshLittleFSConfig &config);

	const char *name() const override;
	FreshStorageInfo info() const override;

  private:
	FreshResult mount() override;
	FreshResult unmount() override;

	FreshLittleFSConfig _config;
};
