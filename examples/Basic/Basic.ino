#include <Arduino.h>
#include <Fresh.h>

Fresh db;
FreshModel users;

void setup() {
	Serial.begin(115200);

	FreshConfig config;
	config.syncIntervalMS = 5000;

	FreshResult initResult = db.init("/fresh_basic", config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	FreshModelResult usersResult = db.createModel("User");
	if (!usersResult) {
		Serial.println(usersResult.message.c_str());
		return;
	}
	users = usersResult.model;

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
	FreshResult updateResult = users.updateById(user["_id"].as<const char *>(), patch);
	if (!updateResult) {
		Serial.println(updateResult.message.c_str());
	}
}

void loop() {
	delay(1000);
}
