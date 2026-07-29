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
	int nativeError() const {
		return _nativeError;
	}

  private:
	FreshResult mount() override;
	FreshResult unmount() override;
	FreshResult mountSPI();
	FreshResult mountSDMMC();
	void releaseManagedSPIBus();

	FreshSDConfig _config;
#if defined(ESP32)
	sdmmc_card_t *_card = nullptr;
#endif
	bool _spiBusInitialized = false;
	int _nativeError = 0;
};
