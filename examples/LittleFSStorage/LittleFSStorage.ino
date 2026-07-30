#include <Arduino.h>
#include <Fresh.h>

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;
    config.storageType = FreshStorageType::LittleFS;
    config.littleFS.partitionLabel = "spiffs";
    config.littleFS.mountPath = "/littlefs";
    config.littleFS.maxOpenFiles = 10;
    config.littleFS.formatOnMountFailure = false;
    config.littleFS.growOnMount = true;

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
