#include <Arduino.h>
#include <Fresh.h>
#include <FreshSPIFlashStorage.h>

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;

    FreshSPIFlashConfig storageConfig;
    storageConfig.mountPath = "/fresh-flash";
    storageConfig.partitionLabel = "fresh-flash";
    storageConfig.maxOpenFiles = 8;
    storageConfig.allocationUnitSize = 4 * 1024;
    storageConfig.formatIfBlank = true;
    storageConfig.formatOnMountFailure = false;

    // Replace these values with the wiring for the target board.
    storageConfig.host = SPI2_HOST;
    storageConfig.chipSelectPin = GPIO_NUM_10;
    storageConfig.clockPin = GPIO_NUM_12;
    storageConfig.mosiPin = GPIO_NUM_11;
    storageConfig.misoPin = GPIO_NUM_13;
    storageConfig.frequencyHz = 20'000'000;
    storageConfig.busOwnership = FreshSPIBusOwnership::Managed;

    FreshInitResult initialized = database.init(
        "/fresh",
        config,
        FreshSPIFlashStorage(storageConfig)
    );
    if (!initialized) {
        Serial.printf("Fresh init failed: %s\n", initialized.message.c_str());
        return;
    }

    FreshModelResult settings = database.createModel("Settings");
    if (!settings && settings.status != FreshStatus::ModelExists) {
        Serial.printf("Model creation failed: %s\n", settings.message.c_str());
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
