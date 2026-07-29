#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>

#include <cstring>

namespace {

constexpr const char *TestPath = "/fresh_result_integrity";
constexpr size_t RecordCount = 48;

void formatValue(char *buffer, size_t size, const char *prefix, size_t index) {
  snprintf(buffer, size, "%s-%03u", prefix, static_cast<unsigned>(index));
}

void churnJsonAllocations() {
  for (size_t iteration = 0; iteration < 64; ++iteration) {
    JsonDocument churn;
    JsonArray values = churn.to<JsonArray>();
    for (size_t index = 0; index < 8; ++index) {
      char value[48];
      formatValue(value, sizeof(value), "allocation-churn-value", iteration * 8 + index);
      values.add(value);
    }
  }
}

bool checkString(
    JsonVariantConst value,
    const char *expected,
    const char *field,
    const char *label,
    size_t position
) {
  const char *actual = value.as<const char *>();
  if (actual != nullptr && strcmp(actual, expected) == 0) return true;
  Serial.printf(
      "FAIL %s[%u].%s: expected=%s actual=%s\n",
      label,
      static_cast<unsigned>(position),
      field,
      expected,
      actual == nullptr ? "<null>" : actual
  );
  return false;
}

bool checkResultArray(
    const FreshResult &result,
    size_t expectedCount,
    size_t firstIndex,
    int step,
    bool general,
    bool expectUpdated,
    bool checkVerified,
    const char *label
) {
  if (!result) {
    Serial.printf("FAIL %s: %s\n", label, result.message.c_str());
    return false;
  }
  JsonArrayConst records = result.doc.as<JsonArrayConst>();
  if (records.isNull() || result.doc.overflowed() ||
      result.affectedCount != expectedCount || records.size() != expectedCount) {
    Serial.printf(
        "FAIL %s: affected=%u array=%u overflow=%d\n",
        label,
        static_cast<unsigned>(result.affectedCount),
        static_cast<unsigned>(records.size()),
        result.doc.overflowed()
    );
    return false;
  }

  size_t position = 0;
  for (JsonObjectConst record : records) {
    const size_t expectedIndex =
        static_cast<size_t>(static_cast<int>(firstIndex) + step * static_cast<int>(position));
    if (!record["index"].is<size_t>() || record["index"].as<size_t>() != expectedIndex) {
      Serial.printf(
          "FAIL %s[%u].index: expected=%u actual=%u\n",
          label,
          static_cast<unsigned>(position),
          static_cast<unsigned>(expectedIndex),
          record["index"] | 0U
      );
      return false;
    }

    char expected[48];
    formatValue(
        expected,
        sizeof(expected),
        general ? "integrity-user" : "integrity-stream-user",
        expectedIndex
    );
    if (!checkString(record["username"], expected, "username", label, position)) return false;

    formatValue(
        expected,
        sizeof(expected),
        general ? "IntegrityGeneralAvatarIcon" : "IntegrityStreamAvatarIcon",
        expectedIndex
    );
    if (!checkString(record["avatar"]["icon"], expected, "avatar.icon", label, position)) {
      return false;
    }

    formatValue(
        expected,
        sizeof(expected),
        general ? "general-color" : "stream-color",
        expectedIndex
    );
    if (!checkString(record["avatar"]["color"], expected, "avatar.color", label, position)) {
      return false;
    }

    if (general) {
      formatValue(expected, sizeof(expected), "record", expectedIndex);
      if (!checkString(record["_id"], expected, "_id", label, position)) return false;

      if (record["updated"].is<bool>() != expectUpdated ||
          (expectUpdated && !record["updated"].as<bool>())) {
        Serial.printf("FAIL %s[%u].updated\n", label, static_cast<unsigned>(position));
        return false;
      }
      if (checkVerified) {
        const bool expectedVerified = expectedIndex == 0;
        if (record["verified"].is<bool>() != expectedVerified ||
            (expectedVerified && !record["verified"].as<bool>())) {
          Serial.printf("FAIL %s[%u].verified\n", label, static_cast<unsigned>(position));
          return false;
        }
      }
    } else {
      formatValue(expected, sizeof(expected), "stream-entry", expectedIndex);
      if (!checkString(record["entryId"], expected, "entryId", label, position)) return false;
    }
    position++;
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
    char value[48];
    JsonDocument record;
    formatValue(value, sizeof(value), "record", index);
    record["_id"] = value;
    record["index"] = index;
    formatValue(value, sizeof(value), "integrity-user", index);
    record["username"] = value;
    formatValue(value, sizeof(value), "IntegrityGeneralAvatarIcon", index);
    record["avatar"]["icon"] = value;
    formatValue(value, sizeof(value), "general-color", index);
    record["avatar"]["color"] = value;
    record["payload"] = payload;
    if (!generalResult.model.create(record)) return false;

    JsonDocument entry;
    entry["index"] = index;
    formatValue(value, sizeof(value), "stream-entry", index);
    entry["entryId"] = value;
    formatValue(value, sizeof(value), "integrity-stream-user", index);
    entry["username"] = value;
    formatValue(value, sizeof(value), "IntegrityStreamAvatarIcon", index);
    entry["avatar"]["icon"] = value;
    formatValue(value, sizeof(value), "stream-color", index);
    entry["avatar"]["color"] = value;
    entry["payload"] = payload;
    if (!streamResult.model.append(entry)) return false;
  }

  FreshResult found = generalResult.model.find([](const JsonDocument &) { return true; });
  churnJsonAllocations();
  if (!checkResultArray(found, RecordCount, 0, 1, true, false, false, "find")) return false;

  FreshResult foundOne = generalResult.model.findOne("username", "integrity-user-023");
  churnJsonAllocations();
  if (!checkResultArray(foundOne, 1, 23, 1, true, false, false, "findOne")) return false;

  JsonDocument patch;
  patch["updated"] = true;
  FreshResult changed = generalResult.model.update(
      [](const JsonDocument &) { return true; },
      patch,
      FreshReturn::ChangedDocs
  );
  churnJsonAllocations();
  if (!checkResultArray(changed, RecordCount, 0, 1, true, true, false, "update changed")) {
    return false;
  }

  JsonDocument allPatch;
  allPatch["verified"] = true;
  FreshResult all = generalResult.model.update(
      [](const JsonDocument &doc) {
        return doc["index"].is<size_t>() && doc["index"].as<size_t>() == 0;
      },
      allPatch,
      FreshReturn::AllDocs
  );
  churnJsonAllocations();
  if (!checkResultArray(all, RecordCount, 0, 1, true, true, true, "update all")) return false;

  // Earlier results must remain independent from later model mutations.
  if (!checkResultArray(found, RecordCount, 0, 1, true, false, false, "find retained")) {
    return false;
  }

  FreshRecordRetrieveOptions page;
  page.offset = 7;
  page.limit = 13;
  page.reverse = true;
  FreshResult listed = generalResult.model.listRecords(page);
  churnJsonAllocations();
  if (!checkResultArray(listed, 13, 40, -1, true, true, true, "listRecords")) return false;

  FreshResult retrieved = streamResult.model.retrieve(page);
  churnJsonAllocations();
  if (!checkResultArray(retrieved, 13, 40, -1, false, false, false, "retrieve")) return false;

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
