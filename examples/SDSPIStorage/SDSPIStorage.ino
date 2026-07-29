#include <Arduino.h>
#include <Fresh.h>

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;
    config.storageType = FreshStorageType::SD;
    config.sd.interface = FreshSDInterface::SPI;
    config.sd.mountPath = "/fresh-sd";
    config.sd.maxOpenFiles = 8;
    config.sd.allocationUnitSize = 16 * 1024;
    config.sd.formatOnMountFailure = false;

    // Replace these values with the wiring for the target board.
    config.sd.spi.host = SPI2_HOST;
    config.sd.spi.chipSelectPin = GPIO_NUM_10;
    config.sd.spi.clockPin = GPIO_NUM_12;
    config.sd.spi.mosiPin = GPIO_NUM_11;
    config.sd.spi.misoPin = GPIO_NUM_13;
    config.sd.spi.frequencyHz = 20'000'000;
    config.sd.spi.busOwnership = FreshSPIBusOwnership::Managed;

    FreshResult initialized = database.init("/fresh", config);
    if (!initialized) {
        Serial.printf("Fresh init failed: %s\n", initialized.message.c_str());
        return;
    }

    FreshModelResult settings = database.createModel("Settings");
    if (!settings && settings.status != FreshStatus::ModelExists) {
        Serial.printf("Model creation failed: %s\n", settings.message.c_str());
        return;
    }

    FreshStorageInfo storage;
    FreshResult queried = database.storageInfo(storage);
    if (queried) {
        Serial.printf(
            "%s total=%u used=%u free=%u\n",
            storage.name.c_str(),
            static_cast<unsigned>(storage.totalBytes),
            static_cast<unsigned>(storage.usedBytes),
            static_cast<unsigned>(storage.freeBytes)
        );
    }
}

void loop() {
    delay(1000);
}
