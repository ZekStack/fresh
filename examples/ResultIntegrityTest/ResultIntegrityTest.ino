#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

namespace {

constexpr const char *TestPath = "/fresh_result_integrity";
constexpr size_t RecordCount = 48;

bool checkResultArray(const FreshResult &result, size_t expected, const char *label) {
  if (!result) {
    Serial.printf("FAIL %s: %s\n", label, result.message.c_str());
    return false;
  }
  JsonArrayConst records = result.doc.as<JsonArrayConst>();
  if (records.isNull() || result.doc.overflowed() ||
      result.affectedCount != expected || records.size() != expected) {
    Serial.printf(
        "FAIL %s: affected=%u array=%u overflow=%d\n",
        label,
        static_cast<unsigned>(result.affectedCount),
        static_cast<unsigned>(records.size()),
        result.doc.overflowed()
    );
    return false;
  }
  return true;
}

bool runTest() {
  LittleFS.remove("/fresh_result_integrity/manifest.a.msgpack");
  LittleFS.remove("/fresh_result_integrity/manifest.b.msgpack");

  Fresh db;
  FreshConfig config;
  config.syncIntervalMS = 60000;
  if (!db.init(TestPath, config)) return false;

  FreshModelResult generalResult = db.createModel("Records", FreshModelType::General);
  FreshModelResult streamResult = db.createModel("Stream", FreshModelType::Stream);
  if (!generalResult || !streamResult) return false;

  String payload;
  payload.reserve(256);
  while (payload.length() < 255) payload += 'x';

  for (size_t index = 0; index < RecordCount; ++index) {
    JsonDocument record;
    record["index"] = index;
    record["payload"] = payload;
    if (!generalResult.model.create(record)) return false;

    JsonDocument entry;
    entry["index"] = index;
    entry["payload"] = payload;
    if (!streamResult.model.append(entry)) return false;
  }

  FreshResult found = generalResult.model.find([](const JsonDocument &) { return true; });
  if (!checkResultArray(found, RecordCount, "find")) return false;

  JsonDocument patch;
  patch["updated"] = true;
  FreshResult changed = generalResult.model.update(
      [](const JsonDocument &) { return true; },
      patch,
      FreshReturn::ChangedDocs
  );
  if (!checkResultArray(changed, RecordCount, "update changed")) return false;

  JsonDocument allPatch;
  allPatch["verified"] = true;
  FreshResult all = generalResult.model.update(
      [](const JsonDocument &doc) { return (doc["index"] | SIZE_MAX) == 0; },
      allPatch,
      FreshReturn::AllDocs
  );
  if (!checkResultArray(all, RecordCount, "update all")) return false;

  FreshRecordRetrieveOptions page;
  page.offset = 7;
  page.limit = 13;
  page.reverse = true;
  if (!checkResultArray(generalResult.model.listRecords(page), 13, "listRecords")) return false;
  if (!checkResultArray(streamResult.model.retrieve(page), 13, "retrieve")) return false;

  db.deinit({.sync = false});
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  if (!LittleFS.begin(true)) {
    Serial.println("FAIL LittleFS");
    return;
  }
  Serial.println(runTest() ? "PASS result integrity" : "FAIL result integrity");
}

void loop() {
  delay(1000);
}
