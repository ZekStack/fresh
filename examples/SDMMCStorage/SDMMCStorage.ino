#include <Arduino.h>
#include <Fresh.h>

#if defined(ESP32)
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#endif

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;

    FreshSDConfig storageConfig;
    storageConfig.interface = FreshSDInterface::SDMMC;
    storageConfig.mountPath = "/fresh-sd";
    storageConfig.maxOpenFiles = 8;
    storageConfig.allocationUnitSize = 16 * 1024;
    storageConfig.formatOnMountFailure = false;
    storageConfig.sdmmc.oneBitMode = false;
    storageConfig.sdmmc.frequencyHz = 20'000'000;

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    // Waveshare ESP32-P4-Module-DEV-KIT onboard TF card power and signals.
    storageConfig.sdmmc.slot = SDMMC_HOST_SLOT_0;
    if (gpio_set_level(GPIO_NUM_45, 0) != ESP_OK ||
        gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_set_level(GPIO_NUM_45, 0) != ESP_OK) {
        Serial.println("Failed to enable the onboard TF card power gate");
        return;
    }
    storageConfig.sdmmc.powerMode = FreshSDMMCPowerMode::OnChipLDO;
    storageConfig.sdmmc.ldoChannel = 4;
    storageConfig.sdmmc.clockPin = GPIO_NUM_43;
    storageConfig.sdmmc.commandPin = GPIO_NUM_44;
    storageConfig.sdmmc.data0Pin = GPIO_NUM_39;
    storageConfig.sdmmc.data1Pin = GPIO_NUM_40;
    storageConfig.sdmmc.data2Pin = GPIO_NUM_41;
    storageConfig.sdmmc.data3Pin = GPIO_NUM_42;
    storageConfig.sdmmc.slotFlags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#else
    // Configure the SDMMC slot and pins required by the target board. Leaving
    // them unset uses the ESP-IDF target defaults when that target supports SDMMC.
#endif

    FreshInitResult initialized = database.init(
        "/fresh",
        config,
        FreshSDStorage(storageConfig)
    );
    if (!initialized) {
        Serial.printf("Fresh init failed: %s\n", initialized.message.c_str());
        return;
    }

    FreshStorageInfo storage = database.storage().info();
    Serial.printf(
        "%s total=%llu used=%llu free=%llu\n",
        storage.name.c_str(),
        static_cast<unsigned long long>(storage.totalBytes),
        static_cast<unsigned long long>(storage.usedBytes),
        static_cast<unsigned long long>(storage.freeBytes)
    );
}

void loop() {
    delay(1000);
}
