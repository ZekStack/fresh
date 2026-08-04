#include <Arduino.h>
#include <Fresh.h>
#include <FreshEMMCStorage.h>

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;

    FreshEMMCConfig storageConfig;
    storageConfig.mountPath = "/fresh-emmc";
    storageConfig.maxOpenFiles = 8;
    storageConfig.allocationUnitSize = 16 * 1024;
    storageConfig.formatOnMountFailure = false;
    storageConfig.slot = 1;
    storageConfig.busWidth = 8;
    storageConfig.frequencyHz = 20'000'000;

    // Configure clock, command, and data0-data7 when the board does not use
    // the target's default SDMMC routing. Power and reset sequencing remain
    // application/board-support responsibilities and must run before init().

    FreshInitResult initialized = database.init(
        "/fresh",
        config,
        FreshEMMCStorage(storageConfig)
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
