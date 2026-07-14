#include <Arduino.h>
#include <Fresh.h>
#include <cstring>

Fresh db;
FreshModel logs;

void printResult(const char *label, const FreshResult &result) {
	Serial.printf("%s: %s affected=%u\n", label, result.message.c_str(), static_cast<unsigned>(result.affectedCount));
	if (result) {
		serializeJson(result.doc, Serial);
		Serial.println();
	}
}

void appendLog(const char *level, const char *message) {
	JsonDocument entry;
	entry["level"] = level;
	entry["message"] = message;
	entry["ms"] = millis();
	printResult("append", logs.append(entry, {.maxEntries = 50}));
}

void setup() {
	Serial.begin(115200);

	FreshResult initResult = db.init("/fresh_stream");
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	FreshModelResult logsResult = db.createModel("DeviceLog", FreshModelType::Stream);
	if (!logsResult) {
		Serial.println(logsResult.message.c_str());
		return;
	}
	logs = logsResult.model;

	appendLog("info", "boot");
	appendLog("info", "wifi skipped");
	appendLog("warn", "battery low");

	FreshStreamRetrieveOptions latestOptions;
	latestOptions.reverse = true;
	latestOptions.limit = 50;
	printResult("retrieve latest", logs.retrieve(latestOptions));

	printResult(
	    "retrieve warn",
	    logs.retrieve(
	        [](const JsonDocument &entry) {
		        const char *level = entry["level"] | "";
		        return strcmp(level, "warn") == 0;
	        },
	        latestOptions
	    )
	);

	latestOptions.limit = 2;
	printResult("retrieve latest 2", logs.retrieve(latestOptions));

	Serial.println("streamTo Serial:");
	printResult("streamTo", logs.streamTo(Serial));
	Serial.println();
}

void loop() {
	delay(1000);
}
