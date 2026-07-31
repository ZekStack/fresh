#include <Arduino.h>
#include <Fresh.h>

Fresh database;

void setup() {
    Serial.begin(115200);

    FreshConfig config;

    FreshLittleFSConfig storageConfig;
    storageConfig.partitionLabel = "spiffs";
    storageConfig.mountPath = "/littlefs";
    storageConfig.maxOpenFiles = 12;
    storageConfig.formatOnMountFailure = false;
    storageConfig.growOnMount = true;

    FreshInitResult initialized = database.init(
        "/fresh",
        config,
        FreshLittleFSStorage(storageConfig)
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

    const uint8_t marker[] = {0x46, 0x52, 0x45, 0x53, 0x48};
    FreshResult written = database.storage().writeFile(
        "/application-marker.bin",
        marker,
        sizeof(marker)
    );
    if (!written) {
        Serial.printf("Application file write failed: %s\n", written.message.c_str());
    }
}

void loop() {
    delay(1000);
}
