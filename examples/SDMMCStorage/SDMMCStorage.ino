#include <Arduino.h>
#include <Fresh.h>

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

    // Waveshare ESP32-P4-Module-DEV-KIT onboard TF card.
    storageConfig.sdmmc.slot = 1;
    storageConfig.sdmmc.oneBitMode = false;
    storageConfig.sdmmc.clockPin = GPIO_NUM_43;
    storageConfig.sdmmc.commandPin = GPIO_NUM_44;
    storageConfig.sdmmc.data0Pin = GPIO_NUM_39;
    storageConfig.sdmmc.data1Pin = GPIO_NUM_40;
    storageConfig.sdmmc.data2Pin = GPIO_NUM_41;
    storageConfig.sdmmc.data3Pin = GPIO_NUM_42;

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
        "%s total=%u used=%u free=%u\n",
        storage.name.c_str(),
        static_cast<unsigned>(storage.totalBytes),
        static_cast<unsigned>(storage.usedBytes),
        static_cast<unsigned>(storage.freeBytes)
    );
}

void loop() {
    delay(1000);
}
