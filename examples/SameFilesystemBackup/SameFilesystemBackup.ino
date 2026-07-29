#include <Arduino.h>
#include <Fresh.h>
#include <FreshFile.h>
#include <FreshStorage.h>

Fresh database;

FreshResult writeBackupArchive(const char* archivePath) {
    FreshFile archive;
    FreshResult opened = database.withStorage(
        [&](FreshStorage& storage) -> FreshResult {
            FreshResult directory = storage.createDirectory("/backups");
            if (!directory) return directory;
            return storage.open(
                archivePath,
                FreshOpenMode::Write,
                archive
            );
        }
    );
    if (!opened) return opened;

    FreshResult started = database.startBackup();
    if (!started) {
        archive.close();
        return started;
    }

    uint8_t buffer[1024];
    while (true) {
        const size_t read = database.readBackup(
            buffer,
            sizeof(buffer),
            100
        );
        if (read > 0 && archive.write(buffer, read) != read) {
            database.cancelBackup();
            archive.close();
            return FreshResult::failure(
                FreshStatus::FileSystemError,
                "failed to write backup archive"
            );
        }

        const FreshBackupStatus status = database.backupStatus();
        if (status.state == FreshBackupState::Finished) break;
        if (status.state == FreshBackupState::Error ||
            status.state == FreshBackupState::Cancelled) {
            archive.close();
            return status.result;
        }

        delay(1);
    }

    if (archive.getWriteError() != 0) {
        archive.close();
        return FreshResult::failure(
            FreshStatus::FileSystemError,
            "backup archive has a write error"
        );
    }

    return archive.syncAndClose();
}

void setup() {
    Serial.begin(115200);

    FreshResult initialized = database.init("/fresh");
    if (!initialized) {
        Serial.printf("Fresh init failed: %s\n", initialized.message.c_str());
        return;
    }

    FreshResult archived = writeBackupArchive(
        "/backups/configuration.fresh"
    );
    Serial.printf(
        "Backup archive: %s\n",
        archived.message.c_str()
    );
}

void loop() {
    delay(1000);
}
