#include <Arduino.h>
#include <Fresh.h>
#include <functional>

class FreshLogger {
  public:
	explicit FreshLogger(Fresh &db) : _db(db) {
	}

	void attach() {
		_db.onEvent(std::bind(&FreshLogger::handleEvent, this, std::placeholders::_1));
		_db.onSync(std::bind(&FreshLogger::handleSync, this, std::placeholders::_1));
	}

  private:
	void handleEvent(FreshEvent event) {
		Serial.printf("event=%s model=%s affected=%u\n", _db.eventToString(event.type), event.modelName.c_str(), static_cast<unsigned>(event.affectedCount));
	}

	void handleSync(FreshResult result) {
		Serial.printf("sync=%s\n", result.message.c_str());
	}

	Fresh &_db;
};

Fresh db;
FreshLogger logger(db);
FreshModel sensors;

void printResult(const char *label, const FreshResult &result) {
	Serial.printf("%s: %s\n", label, result.message.c_str());
}

void setup() {
	Serial.begin(115200);

	FreshResult initResult = db.init("/fresh_callbacks");
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	logger.attach();
	db.onTimeGet([]() {
		return static_cast<uint64_t>(millis() / 1000);
	});

	FreshModelResult sensorsResult = db.createModel("Sensor");
	if (!sensorsResult) {
		Serial.println(sensorsResult.message.c_str());
		return;
	}
	sensors = sensorsResult.model;

	sensors.setValidator([](const JsonDocument &doc) {
		return !doc["type"].isNull();
	});

	JsonDocument invalidSensor;
	invalidSensor["pin"] = 4;
	printResult("bool validator create", sensors.create(invalidSensor));

	sensors.setValidator([](const JsonDocument &doc) {
		bool valid = !doc["type"].isNull() && !doc["pin"].isNull();
		return FreshValidationResult{
		    .result = valid,
		    .message = valid ? "ok" : "Sensor requires type and pin"
		};
	});

	JsonDocument validSensor;
	validSensor["type"] = "temperature";
	validSensor["pin"] = 34;
	printResult("result validator create", sensors.create(validSensor));
}

void loop() {
	delay(1000);
}
