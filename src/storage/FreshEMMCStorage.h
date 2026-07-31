#pragma once

#include "../FreshStorage.h"

#if defined(ESP32)
#include <driver/gpio.h>
#include <sdmmc_cmd.h>
#endif

struct FreshEMMCConfig {
	const char *mountPath = "/fresh-emmc";
	size_t maxOpenFiles = 8;
	size_t allocationUnitSize = 16 * 1024;
	bool formatOnMountFailure = false;
	int slot = 1;
	uint8_t busWidth = 8;
	uint32_t frequencyHz = 20'000'000;
	uint32_t slotFlags = 0;

#if defined(ESP32)
	gpio_num_t clockPin = GPIO_NUM_NC;
	gpio_num_t commandPin = GPIO_NUM_NC;
	gpio_num_t data0Pin = GPIO_NUM_NC;
	gpio_num_t data1Pin = GPIO_NUM_NC;
	gpio_num_t data2Pin = GPIO_NUM_NC;
	gpio_num_t data3Pin = GPIO_NUM_NC;
	gpio_num_t data4Pin = GPIO_NUM_NC;
	gpio_num_t data5Pin = GPIO_NUM_NC;
	gpio_num_t data6Pin = GPIO_NUM_NC;
	gpio_num_t data7Pin = GPIO_NUM_NC;
#else
	int clockPin = -1;
	int commandPin = -1;
	int data0Pin = -1;
	int data1Pin = -1;
	int data2Pin = -1;
	int data3Pin = -1;
	int data4Pin = -1;
	int data5Pin = -1;
	int data6Pin = -1;
	int data7Pin = -1;
#endif
};

class FreshEMMCStorage final : public FreshStorage {
  public:
	explicit FreshEMMCStorage(
	    const FreshEMMCConfig &config = FreshEMMCConfig()
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

	FreshEMMCConfig _config;
#if defined(ESP32)
	sdmmc_card_t *_card = nullptr;
#endif
	mutable int _nativeError = 0;
};
