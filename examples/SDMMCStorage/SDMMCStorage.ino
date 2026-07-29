#include <Arduino.h>
#include <Fresh.h>

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;
    config.storageType = FreshStorageType::SD;
    config.sd.interface = FreshSDInterface::SDMMC;
    config.sd.mountPath = "/fresh-sd";
    config.sd.maxOpenFiles = 8;
    config.sd.allocationUnitSize = 16 * 1024;
    config.sd.formatOnMountFailure = false;

    config.sd.sdmmc.slot = 0;
    config.sd.sdmmc.oneBitMode = false;

    // Leave the pins unset to use the target's default SDMMC routing.
    // Targets with GPIO-matrix SDMMC support may configure clock, command,
    // and data pins explicitly. See docs/storage.md.

    FreshResult initialized = database.init("/fresh", config);
    if (!initialized) {
        Serial.printf("Fresh init failed: %s\n", initialized.message.c_str());
        return;
    }

    FreshStorageInfo storage;
    FreshResult queried = database.storageInfo(storage);
    if (!queried) {
        Serial.printf("Storage query failed: %s\n", queried.message.c_str());
        return;
    }

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
