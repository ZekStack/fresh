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

    FreshFile archive;
    FreshResult opened = database.withStorage(
        [&](FreshStorage& storage) -> FreshResult {
            FreshFile forbidden;
            FreshResult forbiddenOpen = storage.open(
                "/fresh_storage_lifecycle/forbidden.bin",
                FreshOpenMode::Write,
                forbidden
            );
            check(
                !forbiddenOpen &&
                    forbiddenOpen.status == FreshStatus::UnsupportedOperation,
                "reject application access to database root"
            );

            for (const char* alias : {
                     "//fresh_storage_lifecycle/forbidden.bin",
                     "/fresh_storage_lifecycle//forbidden.bin",
                     "/fresh_storage_lifecycle/./forbidden.bin",
                     "/fresh_storage_lifecycle/../forbidden.bin"
                 }) {
                FreshFile aliasFile;
                FreshResult aliasOpen = storage.open(
                    alias,
                    FreshOpenMode::Write,
                    aliasFile
                );
                check(
                    !aliasOpen && aliasOpen.status == FreshStatus::InvalidArgument,
                    "reject non-canonical database path alias"
                );
            }

            FreshResult directory = storage.createDirectory("/backups");
            if (!directory) return directory;

            return storage.open(
                "/backups/lifecycle.bin",
                FreshOpenMode::Write,
                archive
            );
        }
    );
    checkResult(opened, "open application file through guarded access");

    FreshStorageInfo openInfo;
    checkResult(database.storageInfo(openInfo), "read open file diagnostics");
    check(
        openInfo.openFileCount == 1 &&
            openInfo.applicationOpenFileCount == 1 &&
            openInfo.internalOpenFileCount == 0 &&
            openInfo.maxOpenFiles >= 1,
        "report application and internal file counts"
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

    FreshStorageInfo runningInfo;
    checkResult(
        database.storageInfo(runningInfo),
        "database remains running after busy deinit"
    );
    checkResult(archive.syncAndClose(), "sync and close application file");

    checkResult(database.deinit(), "deinitialize after closing files");

    FreshFile leaked;
    {
        Fresh scoped;
        checkResult(
            scoped.init("/fresh_storage_destructor"),
            "initialize destructor lifecycle database"
        );
        checkResult(
            scoped.withStorage(
                [&](FreshStorage& storage) -> FreshResult {
                    FreshResult directory = storage.createDirectory("/backups");
                    if (!directory) return directory;
                    return storage.open(
                        "/backups/destructor.bin",
                        FreshOpenMode::Write,
                        leaked
                    );
                }
            ),
            "open file that outlives Fresh"
        );
        check(
            leaked.write(payload, sizeof(payload)) == sizeof(payload),
            "write file before destructor cleanup"
        );
    }

    check(!leaked, "destructor invalidates surviving FreshFile");
    check(
        leaked.write(payload, sizeof(payload)) == 0 &&
            leaked.getWriteError() != 0,
        "surviving FreshFile fails closed after destruction"
    );
    checkResult(leaked.close(), "close already invalidated FreshFile");

    checkResult(
        database.init("/fresh_storage_lifecycle"),
        "reinitialize same storage after destructor cleanup"
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
