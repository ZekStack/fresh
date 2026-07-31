#include <Arduino.h>
#include <Fresh.h>

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;

    FreshSDConfig storageConfig;
    storageConfig.interface = FreshSDInterface::SPI;
    storageConfig.mountPath = "/fresh-sd";
    storageConfig.maxOpenFiles = 8;
    storageConfig.allocationUnitSize = 16 * 1024;
    storageConfig.formatOnMountFailure = false;

    // Replace these values with the wiring for the target board.
    storageConfig.spi.host = SPI2_HOST;
    storageConfig.spi.chipSelectPin = GPIO_NUM_10;
    storageConfig.spi.clockPin = GPIO_NUM_12;
    storageConfig.spi.mosiPin = GPIO_NUM_11;
    storageConfig.spi.misoPin = GPIO_NUM_13;
    storageConfig.spi.frequencyHz = 10'000'000;
    storageConfig.spi.busOwnership = FreshSPIBusOwnership::Managed;

    FreshInitResult initialized = database.init(
        "/fresh",
        config,
        FreshSDStorage(storageConfig)
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
