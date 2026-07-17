#include <Arduino.h>
#include <Fresh.h>
#include <LittleFS.h>
#include <esp_system.h>

#include <cstring>

namespace {

constexpr const char *ModelName = "Records";
constexpr const char *DocumentId = "ownership-record";
constexpr const char *ExpectedHostname = "HitecSmartHome";
constexpr const char *ExpectedSsid = "Hitec Industrial Technology";
constexpr const char *ExpectedMessage = "caller-owned text must remain stable";

struct CallerOwnedData {
	char hostname[32] = {};
	char ssid[64] = {};
	char message[96] = {};
};

bool checkDocument(const FreshResult &result, const char *stage) {
	if (!result) {
		Serial.printf("FAIL %s lookup: %s\n", stage, result.message.c_str());
		return false;
	}

	JsonObjectConst config = result.doc["config"].as<JsonObjectConst>();
	if (config.isNull() ||
	    std::strcmp(config["hostname"] | "", ExpectedHostname) != 0 ||
	    std::strcmp(config["ssid"] | "", ExpectedSsid) != 0 ||
	    std::strcmp(result.doc["message"] | "", ExpectedMessage) != 0) {
		Serial.printf("FAIL %s ownership mismatch\n", stage);
		serializeJson(result.doc, Serial);
		Serial.println();
		return false;
	}

	return true;
}

bool runTest() {
	char databasePath[64] = {};
	snprintf(
	    databasePath,
	    sizeof(databasePath),
	    "/fresh_ownership_%08lx",
	    static_cast<unsigned long>(esp_random())
	);

	FreshConfig config;
	config.syncIntervalMS = 60000;

	Fresh database;
	FreshResult initialized = database.init(databasePath, config);
	if (!initialized) {
		Serial.printf("FAIL init: %s\n", initialized.message.c_str());
		return false;
	}

	FreshModelResult createdModel = database.createModel(ModelName);
	if (!createdModel) {
		Serial.printf("FAIL model: %s\n", createdModel.message.c_str());
		database.deinit({.sync = false});
		return false;
	}

	CallerOwnedData callerData;
	strlcpy(callerData.hostname, ExpectedHostname, sizeof(callerData.hostname));
	strlcpy(callerData.ssid, ExpectedSsid, sizeof(callerData.ssid));
	strlcpy(callerData.message, ExpectedMessage, sizeof(callerData.message));

	const CallerOwnedData &constView = callerData;
	JsonDocument input;
	input["_id"] = DocumentId;
	JsonObject nestedConfig = input["config"].to<JsonObject>();
	nestedConfig["hostname"] = constView.hostname;
	nestedConfig["ssid"] = constView.ssid;
	input["message"] = constView.message;

	FreshResult created = createdModel.model.create(input);
	if (!created) {
		Serial.printf("FAIL create: %s\n", created.message.c_str());
		database.deinit({.sync = false});
		return false;
	}

	std::memset(callerData.hostname, 'x', sizeof(callerData.hostname));
	std::memset(callerData.ssid, 'y', sizeof(callerData.ssid));
	std::memset(callerData.message, 'z', sizeof(callerData.message));
	callerData.hostname[sizeof(callerData.hostname) - 1] = '\0';
	callerData.ssid[sizeof(callerData.ssid) - 1] = '\0';
	callerData.message[sizeof(callerData.message) - 1] = '\0';
	input.clear();

	if (!checkDocument(createdModel.model.findById(DocumentId), "in-memory")) {
		database.deinit({.sync = false});
		return false;
	}

	FreshResult synced = database.forceSync();
	if (!synced) {
		Serial.printf("FAIL sync: %s\n", synced.message.c_str());
		database.deinit({.sync = false});
		return false;
	}

	if (!database.deinit({.sync = false})) {
		Serial.println("FAIL deinit");
		return false;
	}

	Fresh reopened;
	FreshResult reopenedResult = reopened.init(databasePath, config);
	if (!reopenedResult) {
		Serial.printf("FAIL reopen: %s\n", reopenedResult.message.c_str());
		return false;
	}

	FreshModel model = reopened.model(ModelName);
	const bool valid = model && checkDocument(model.findById(DocumentId), "reopened");
	reopened.deinit({.sync = false});
	return valid;
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(500);

	if (!LittleFS.begin(true)) {
		Serial.println("FAIL LittleFS");
		return;
	}

	Serial.println(runTest() ? "PASS persisted ownership" : "FAIL persisted ownership");
}

void loop() {
	delay(1000);
}
