#include <Arduino.h>
#include <Fresh.h>
#include <FreshFile.h>
#include <FreshStorage.h>

namespace {

Fresh database;
size_t passed = 0;
size_t failed = 0;

void check(bool condition, const char* label) {
    if (condition) {
        passed++;
        Serial.printf("[PASS] %s\n", label);
    } else {
        failed++;
        Serial.printf("[FAIL] %s\n", label);
    }
}

void checkResult(const FreshResult& result, const char* label) {
    if (!result) {
        Serial.printf("       %s\n", result.message.c_str());
    }
    check(static_cast<bool>(result), label);
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("Storage lifecycle regression starting");

    checkResult(
        database.init("/fresh_storage_lifecycle"),
        "initialize database"
    );

    FreshStorage* storage = database.storage();
    check(storage != nullptr, "resolve active storage");

    if (storage != nullptr) {
        FreshFile forbidden;
        FreshResult forbiddenOpen = storage->open(
            "/fresh_storage_lifecycle/forbidden.bin",
            FreshOpenMode::Write,
            forbidden
        );
        check(
            !forbiddenOpen &&
                forbiddenOpen.status == FreshStatus::UnsupportedOperation,
            "reject application access to database root"
        );

        checkResult(
            storage->createDirectory("/backups"),
            "create application backup directory"
        );

        FreshFile archive;
        checkResult(
            storage->open(
                "/backups/lifecycle.bin",
                FreshOpenMode::Write,
                archive
            ),
            "open application file"
        );

        const uint8_t payload[] = {0x46, 0x52, 0x45, 0x53, 0x48};
        check(
            archive.write(payload, sizeof(payload)) == sizeof(payload),
            "write application file"
        );

        FreshResult busy = database.deinit();
        check(
            !busy && busy.status == FreshStatus::Busy,
            "deinit rejects open application file"
        );
        check(database.storage() != nullptr, "database remains running after busy deinit");

        checkResult(archive.syncAndClose(), "sync and close application file");
    }

    checkResult(database.deinit(), "deinitialize after closing files");
    checkResult(
        database.init("/fresh_storage_lifecycle"),
        "reinitialize same storage"
    );
    checkResult(database.deinit(), "repeat deinitialize");

    Serial.printf(
        "Storage lifecycle regression complete: %u passed, %u failed\n",
        static_cast<unsigned>(passed),
        static_cast<unsigned>(failed)
    );
}

void loop() {
    delay(1000);
}
