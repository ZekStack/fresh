#include <Arduino.h>
#include <Fresh.h>

Fresh db;
FreshModel readings;

void printStorage(const char *label) {
	FreshStorageInfo info = db.storageInfo();
	Serial.printf(
	    "%s total=%u used=%u free=%u\n",
	    label,
	    static_cast<unsigned>(info.totalBytes),
	    static_cast<unsigned>(info.usedBytes),
	    static_cast<unsigned>(info.freeBytes)
	);
}

void printResult(const char *label, const FreshResult &result) {
	Serial.printf("%s: %s\n", label, result.message.c_str());
}

void setup() {
	Serial.begin(115200);

	FreshConfig config;
	config.syncIntervalMS = 1000;
	FreshResult initResult = db.init("/fresh_sync", config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	printStorage("after init");

	readings = db.createModel("Reading");
	if (!readings) {
		Serial.println("Failed to open Reading model");
		return;
	}

	JsonDocument reading;
	reading["sensor"] = "battery";
	reading["value"] = 4.12;
	printResult("RAM-first create", readings.create(reading));

	printStorage("after RAM write");

	JsonDocument patch;
	patch["value"] = 4.10;
	printResult("RAM-first update", readings.updateById(reading["_id"].as<const char *>(), patch));
	printStorage("before background sync");

	delay(config.syncIntervalMS + 500);
	printStorage("after background sync");

	FreshModel loaded = db.model("Reading");
	if (loaded) {
		printResult("loaded model find", loaded.findById(reading["_id"].as<const char *>()));
	}
}

void loop() {
	delay(1000);
}
