#include <Arduino.h>
#include <Fresh.h>
#include <FreshFile.h>

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
    if (!result) Serial.printf("       %s\n", result.message.c_str());
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
    check(database.storage().available(), "expose initialized storage facade");

    FreshFile forbidden;
    FreshResult forbiddenOpen = database.storage().open(
        "/fresh_storage_lifecycle/forbidden.bin",
        FreshOpenMode::Write,
        forbidden
    );
    check(
        !forbiddenOpen && forbiddenOpen.status == FreshStatus::UnsupportedOperation,
        "reject application access to database root"
    );

    for (const char* alias : {
             "//fresh_storage_lifecycle/forbidden.bin",
             "/fresh_storage_lifecycle//forbidden.bin",
             "/fresh_storage_lifecycle/./forbidden.bin",
             "/fresh_storage_lifecycle/../forbidden.bin"
         }) {
        FreshFile aliasFile;
        FreshResult aliasOpen = database.storage().open(
            alias,
            FreshOpenMode::Write,
            aliasFile
        );
        check(
            !aliasOpen && aliasOpen.status == FreshStatus::InvalidArgument,
            "reject non-canonical database path alias"
        );
    }

    checkResult(
        database.storage().ensureDirectory("/backups/lifecycle"),
        "create nested application directory"
    );

    FreshFile archive;
    checkResult(
        database.storage().open(
            "/backups/lifecycle/lifecycle.bin",
            FreshOpenMode::Write,
            archive
        ),
        "open application file through db.storage()"
    );

    FreshStorageInfo openInfo;
    checkResult(database.storage().info(openInfo), "read open file diagnostics");
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
    check(database.storage().available(), "storage remains available after busy deinit");
    checkResult(archive.syncAndClose(), "sync and close application file");
    checkResult(database.deinit(), "deinitialize after closing files");
    check(!database.storage().available(), "detach storage facade after deinit");

    FreshStorageAccess retainedStorage;
    FreshFile leaked;
    {
        Fresh scoped;
        checkResult(
            scoped.init("/fresh_storage_destructor"),
            "initialize destructor lifecycle database"
        );
        retainedStorage = scoped.storage();
        check(retainedStorage.available(), "retained facade is initially available");
        checkResult(
            scoped.storage().ensureDirectory("/backups"),
            "create destructor test directory"
        );
        checkResult(
            scoped.storage().open(
                "/backups/destructor.bin",
                FreshOpenMode::Write,
                leaked
            ),
            "open file that outlives Fresh"
        );
        check(
            leaked.write(payload, sizeof(payload)) == sizeof(payload),
            "write file before destructor cleanup"
        );
    }

    check(!retainedStorage.available(), "retained facade detaches after Fresh destruction");
    bool retainedExists = true;
    FreshResult retainedExistsResult = retainedStorage.exists(
        "/backups/destructor.bin",
        retainedExists
    );
    check(
        !retainedExistsResult && retainedExistsResult.status == FreshStatus::NotInitialized &&
            !retainedExists,
        "retained facade fails safely after Fresh destruction"
    );

    check(!leaked, "destructor invalidates surviving FreshFile");
    check(
        leaked.write(payload, sizeof(payload)) == 0 &&
            leaked.getWriteError() != 0,
        "surviving FreshFile fails closed after destruction"
    );
    checkResult(leaked.close(), "close already invalidated FreshFile");

    checkResult(
        database.init("/fresh_storage_lifecycle"),
        "reinitialize same LittleFS backend"
    );
    check(
        database.storage().exists("/backups/lifecycle/lifecycle.bin"),
        "application file persists after reinitialization"
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
