#include <Arduino.h>
#include <Fresh.h>

Fresh db;

void printResult(const char *label, const FreshResult &result) {
	Serial.printf("%s: %s affected=%u\n", label, result.message.c_str(), static_cast<unsigned>(result.affectedCount));
}

void printModels() {
	FreshModelListResult result = db.listModels();
	if (!result) {
		Serial.printf("list models failed: %s\n", result.message.c_str());
		return;
	}

	Serial.printf("models=%u\n", static_cast<unsigned>(result.affectedCount));
	for (const FreshModelInfo &model : result.models) {
		Serial.printf(
		    "- %s type=%s records=%u\n",
		    model.name.c_str(),
		    model.type == FreshModelType::Stream ? "stream" : "general",
		    static_cast<unsigned>(model.recordCount)
		);
	}
}

void printRecords(const char *label, const FreshModel &model) {
	FreshRecordRetrieveOptions options;
	options.limit = 10;
	FreshResult result = model.listRecords(options);
	Serial.printf("%s: %s affected=%u ", label, result.message.c_str(), static_cast<unsigned>(result.affectedCount));
	serializeJson(result.doc, Serial);
	Serial.println();
}

void setup() {
	Serial.begin(115200);

	FreshResult initResult = db.init("/fresh_models");
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	FreshModelResult alphaResult = db.createModel("AlphaModel");
	FreshModelResult betaResult = db.createModel("BetaModel");
	FreshModelResult gammaResult = db.createModel("GammaModel", FreshModelType::Stream);

	Serial.printf(
	    "Alpha opened=%u Beta opened=%u Gamma opened=%u\n",
	    static_cast<unsigned>(static_cast<bool>(alphaResult)),
	    static_cast<unsigned>(static_cast<bool>(betaResult)),
	    static_cast<unsigned>(static_cast<bool>(gammaResult))
	);

	JsonDocument alphaDoc;
	alphaDoc["name"] = "before";
	printResult("create Alpha record", alphaResult.model.create(alphaDoc));

	JsonDocument streamEntry;
	streamEntry["message"] = "stream entry";
	printResult("append Gamma record", gammaResult.model.append(streamEntry));

	printModels();
	printRecords("Alpha records", alphaResult.model);
	printRecords("Gamma records", gammaResult.model);

	JsonDocument replacement;
	replacement["name"] = "after";
	replacement["enabled"] = true;
	printResult(
	    "replace Alpha record",
	    alphaResult.model.replaceById(alphaDoc["_id"].as<const char *>(), replacement)
	);
	printRecords("Alpha records after replace", alphaResult.model);

	printResult("rename AlphaModel", db.renameModel("AlphaModel", "AlphaRenamed"));
	printResult("drop BetaModel", db.dropModel("BetaModel"));
	printResult("drop selected", db.dropModels({"GammaModel", "MissingModel"}));

	FreshModelResult tempAResult = db.createModel("TempA");
	FreshModelResult tempBResult = db.createModel("TempB");
	Serial.printf(
	    "Temp models opened=%u/%u\n",
	    static_cast<unsigned>(static_cast<bool>(tempAResult)),
	    static_cast<unsigned>(static_cast<bool>(tempBResult))
	);
	printModels();

	printResult("drop all", db.dropAllModels());
	printModels();
}

void loop() {
	delay(1000);
}
