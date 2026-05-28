#include <Arduino.h>
#include <Fresh.h>

Fresh db;
FreshModel users;
FreshModel logs;

void setup() {
	Serial.begin(115200);

	FreshConfig config;
	config.syncIntervalMS = 5000;
	config.snapshotRecordThreshold = 32;
	config.backupBufferSize = 4096;

	FreshResult initResult = db.init("/fresh_basic", config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	db.onEvent([](FreshEvent event) {
		Serial.printf(
		    "Fresh event: %u on %s\n",
		    static_cast<unsigned>(event.type),
		    event.modelName.c_str()
		);
	});

	users = db.createModel("User");
	if (!users) {
		Serial.println("User model already exists or could not be created");
		return;
	}

	users.setValidator([](const JsonDocument &doc) { return !doc["name"].isNull(); });

	JsonDocument user;
	user["name"] = "Panna";
	user["age"] = 19;
	FreshResult createResult = users.create(user);
	if (!createResult) {
		Serial.println(createResult.message.c_str());
		return;
	}

	FreshResult found = users.findById(user["_id"].as<const char *>());
	if (found) {
		Serial.println("Found user");
		serializeJson(found.doc, Serial);
		Serial.println();
	}

	JsonDocument patch;
	patch["age"] = 20;
	users.updateById(user["_id"].as<const char *>(), patch);

	logs = db.createModel("Log", FreshModelType::Stream);
	JsonDocument logEntry;
	logEntry["message"] = "booted";
	logs.append(logEntry);
}

void loop() {
	delay(1000);
}
