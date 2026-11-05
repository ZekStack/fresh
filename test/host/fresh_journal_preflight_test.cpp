#include <Fresh.h>
#include <internal/FreshInternal.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class HostStorage final : public FreshStorage {
  public:
    explicit HostStorage(const std::string &path)
        : FreshStorage(FreshStorageType::Custom, path.c_str(), 32) {
        std::filesystem::create_directories(path);
    }
    const char *name() const override { return "host-posix"; }
    FreshStorageInfo info() const override {
        FreshStorageInfo out;
        out.type = FreshStorageType::Custom;
        out.state = state();
        out.freeBytes = 1024ULL * 1024 * 1024;
        out.totalBytes = out.freeBytes;
        return out;
    }
  protected:
    FreshResult mount() override {
        setState(FreshStorageState::Mounted);
        return FreshResult::success();
    }
    FreshResult unmount() override {
        setState(FreshStorageState::Uninitialized);
        return FreshResult::success();
    }
    FreshResult readInfoBackend(FreshStorageInfo &out) const override {
        out = info();
        return FreshResult::success();
    }
};

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
void ok(const FreshResult &result, const char *what) {
    check(static_cast<bool>(result), std::string(what) + ": " + result.message);
}
void ok(const FreshModelResult &result, const char *what) {
    check(static_cast<bool>(result), std::string(what) + ": " + result.message);
}
FreshConfig testConfig() {
    FreshConfig cfg;
    cfg.syncIntervalMS = 60000;
    cfg.snapshotRecordThreshold = 1000;
    return cfg;
}

void testMutations(const std::string &path) {
    Fresh db;
    FreshConfig cfg = testConfig();
    ok(db.init("/journal", cfg, std::make_unique<HostStorage>(path)), "init");
    auto items = db.createModel("Items");
    auto audit = db.createModel("Audit", FreshModelType::Stream);
    ok(items, "create Items");
    ok(audit, "create Audit");
    for (const char *id : {"one", "two", "three"}) {
        JsonDocument doc;
        doc["_id"] = id;
        doc["value"] = 1;
        ok(items.model.create(doc), "create");
    }
    size_t auditCount = 0;
    auto appendAudit = [&] {
        JsonDocument entry;
        entry["ordinal"] = ++auditCount;
        ok(audit.model.append(entry), "interleaved append");
    };
    ok(items.model.setValidator(FreshResultValidator(
        [&](const JsonDocument &) -> FreshValidationResult {
            appendAudit();
            return {.result = true, .message = "ok"};
        }
    )), "set validator");

    JsonDocument patch;
    patch["value"] = 2;
    ok(items.model.updateById("one", patch), "updateById");
    JsonDocument replacement;
    replacement["value"] = 3;
    ok(items.model.replaceById("two", replacement), "replaceById");
    JsonDocument batchPatch;
    batchPatch["tag"] = "checked";
    auto batch = items.model.update(
        [&](const JsonDocument &) { appendAudit(); return true; },
        batchPatch, FreshReturn::ChangedDocs
    );
    ok(batch, "update batch");
    check(batch.affectedCount == 3, "batch count differs");
    auto deleted = items.model.deleteMany([&](const JsonDocument &doc) {
        appendAudit();
        return std::strcmp(doc["_id"] | "", "two") == 0;
    });
    ok(deleted, "deleteMany");
    check(deleted.affectedCount == 1, "batch delete count differs");
    ok(items.model.deleteById("three"), "deleteById");
    auto surviving = items.model.findById("one");
    ok(surviving, "read surviving document");
    check((surviving.doc["value"] | 0) == 2, "update missing");
    check(std::strcmp(surviving.doc["tag"] | "", "checked") == 0, "batch patch missing");
    check(auditCount >= 5, "no cross-model interleaving");
    ok(db.forceSync(), "test durability boundary");
    ok(db.deinit(FreshDeinitOptions{.sync = false}), "deinit");

    Fresh restored;
    ok(restored.init("/journal", cfg, std::make_unique<HostStorage>(path)), "reopen");
    auto recovered = restored.model("Items").findById("one");
    ok(recovered, "read recovered document");
    check((recovered.doc["value"] | 0) == 2, "recovered value differs");
    check(std::strcmp(recovered.doc["tag"] | "", "checked") == 0, "recovered tag differs");
    for (const char *id : {"two", "three"}) {
        auto missing = restored.model("Items").findById(id);
        check(!missing && missing.status == FreshStatus::DocumentNotFound,
              "deleted record reappeared");
    }
    auto log = restored.model("Audit").listRecords();
    ok(log, "recovered audit");
    check(log.affectedCount == auditCount, "interleaved audit records lost");
    ok(restored.deinit(FreshDeinitOptions{.sync = false}), "deinit restored");
}

void testOversizeAtomicity(const std::string &path) {
    Fresh db;
    FreshConfig cfg = testConfig();
    cfg.maxJournalRecordBytes = 256;
    ok(db.init("/limits", cfg, std::make_unique<HostStorage>(path)), "init limited");
    auto items = db.createModel("Items");
    ok(items, "create limited model");
    JsonDocument small;
    small["_id"] = "safe";
    small["value"] = 1;
    ok(items.model.create(small), "create safe document");

    JsonDocument huge;
    huge["_id"] = "too-large";
    huge["payload"] = std::string(1024, 'x');
    auto reject = items.model.create(huge);
    check(!reject && reject.status == FreshStatus::SizeLimitExceeded,
          "oversize create succeeded");
    JsonDocument hugePatch;
    hugePatch["payload"] = std::string(1024, 'x');
    reject = items.model.updateById("safe", hugePatch);
    check(!reject && reject.status == FreshStatus::SizeLimitExceeded,
          "oversize update succeeded");
    reject = items.model.update([](const JsonDocument &) { return true; }, hugePatch);
    check(!reject && reject.status == FreshStatus::SizeLimitExceeded,
          "oversize batch succeeded");
    auto surviving = items.model.findById("safe");
    ok(surviving, "read unaffected document");
    check((surviving.doc["value"] | 0) == 1 && surviving.doc["payload"].isNull(),
          "failed preflight modified RAM");
    ok(db.forceSync(), "sync valid record");
    ok(db.deinit(FreshDeinitOptions{.sync = false}), "close limits");

    Fresh restored;
    ok(restored.init("/limits", cfg, std::make_unique<HostStorage>(path)), "reload limits");
    auto persisted = restored.model("Items").findById("safe");
    ok(persisted, "find safe record");
    check((persisted.doc["value"] | 0) == 1 && persisted.doc["payload"].isNull(),
          "failed preflight persisted");
    auto missing = restored.model("Items").findById("too-large");
    check(!missing && missing.status == FreshStatus::DocumentNotFound,
          "rejected record persisted");
    ok(restored.deinit(FreshDeinitOptions{.sync = false}), "close restored limits");
}

void testSequenceChecks() {
    FreshPendingRecord record;
    record.sequence = 1;
    record.preparedJournalBytes = 100;
    struct Boundary { uint64_t sequence; size_t bytes; };
    const Boundary boundaries[] = {
        {127, 100}, {128, 101}, {255, 101}, {256, 102},
        {65535, 102}, {65536, 104}, {4294967295ULL, 104},
        {4294967296ULL, 108}
    };
    for (const auto &boundary : boundaries) {
        ok(FreshCheckPreparedJournalSize(record, boundary.sequence, boundary.bytes),
           "valid sequence size");
        if (boundary.bytes > 100) {
            auto failure = FreshCheckPreparedJournalSize(
                record, boundary.sequence, boundary.bytes - 1);
            check(!failure && failure.status == FreshStatus::SizeLimitExceeded,
                  "sequence-width boundary bypassed size limit");
        }
    }
    record.preparedJournalBytes = 0;
    auto failure = FreshCheckPreparedJournalSize(record, 1, 100);
    check(!failure && failure.status == FreshStatus::InternalError,
          "invalid prepared metadata accepted");
    ok(FreshCheckJournalSequenceRange(UINT64_MAX - 1, 1), "last legal sequence");
    failure = FreshCheckJournalSequenceRange(UINT64_MAX - 1, 2);
    check(!failure && failure.status == FreshStatus::InternalError,
          "sequence overflow accepted");
}

void testBackgroundSync(const std::string &path) {
    Fresh db;
    FreshConfig cfg = testConfig();
    cfg.syncIntervalMS = 50;
    ok(db.init("/background", cfg, std::make_unique<HostStorage>(path)), "init background");
    auto items = db.createModel("Items");
    ok(items, "create background model");
    JsonDocument doc;
    doc["_id"] = "async";
    doc["value"] = 42;
    const auto before = db.diagnostics().syncSuccesses;
    ok(items.model.create(doc), "async create");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline &&
           db.diagnostics().syncSuccesses <= before) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(db.diagnostics().syncSuccesses > before, "no background sync");
    ok(db.deinit(FreshDeinitOptions{.sync = false}), "background deinit");

    Fresh restored;
    ok(restored.init("/background", cfg, std::make_unique<HostStorage>(path)), "background reopen");
    auto persisted = restored.model("Items").findById("async");
    ok(persisted, "read background persisted record");
    check((persisted.doc["value"] | 0) == 42, "async mutation was not persisted");
    ok(restored.deinit(FreshDeinitOptions{.sync = false}), "background close");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: fresh-journal-host <temporary-directory>\n";
        return 2;
    }
    try {
        const std::filesystem::path root(argv[1]);
        testSequenceChecks();
        testMutations((root / "mutations").string());
        testOversizeAtomicity((root / "limits").string());
        testBackgroundSync((root / "background").string());
        std::cout << "Fresh journal preflight host regression: PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Fresh journal preflight host regression: FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
