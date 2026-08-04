#pragma once

#include "../FreshStorage.h"

#if defined(ESP32)
#include <sdmmc_cmd.h>
#endif

class FreshSDStorage final : public FreshStorage {
  public:
	explicit FreshSDStorage(const FreshSDConfig &config);

	const char *name() const override;
	FreshStorageInfo info() const override;
	int nativeError() const override {
		return _nativeError;
	}

  private:
	FreshResult mount() override;
	FreshResult unmount() override;
	bool supportsFormat() const override {
		return true;
	}
	FreshResult formatBackend() override;
	FreshResult readInfoBackend(FreshStorageInfo &result) const override;
	FreshResult mountSPI();
	FreshResult mountSDMMC();
	FreshResult releaseManagedSPIBus();

	FreshSDConfig _config;
#if defined(ESP32)
	sdmmc_card_t *_card = nullptr;
#endif
	bool _spiBusInitialized = false;
	mutable int _nativeError = 0;
};
