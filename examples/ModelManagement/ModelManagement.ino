#include <Arduino.h>
#include <Fresh.h>

Fresh db;

void printResult(const char *label, const FreshResult &result) {
	Serial.printf("%s: %s affected=%u\n", label, result.message.c_str(), static_cast<unsigned>(result.affectedCount));
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
	FreshModelResult gammaResult = db.createModel("GammaModel");

	Serial.printf(
	    "Alpha opened=%u Beta opened=%u Gamma opened=%u\n",
	    static_cast<unsigned>(static_cast<bool>(alphaResult)),
	    static_cast<unsigned>(static_cast<bool>(betaResult)),
	    static_cast<unsigned>(static_cast<bool>(gammaResult))
	);

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

	printResult("drop all", db.dropAllModels());
}

void loop() {
	delay(1000);
}
