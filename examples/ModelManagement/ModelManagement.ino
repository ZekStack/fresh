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

	FreshModel alpha = db.createModel("AlphaModel");
	FreshModel beta = db.createModel("BetaModel");
	FreshModel gamma = db.createModel("GammaModel");

	Serial.printf(
	    "Alpha opened=%u Beta opened=%u Gamma opened=%u\n",
	    static_cast<unsigned>(static_cast<bool>(alpha)),
	    static_cast<unsigned>(static_cast<bool>(beta)),
	    static_cast<unsigned>(static_cast<bool>(gamma))
	);

	printResult("rename AlphaModel", db.renameModel("AlphaModel", "AlphaRenamed"));
	printResult("drop BetaModel", db.dropModel("BetaModel"));
	printResult("drop selected", db.dropModels({"GammaModel", "MissingModel"}));

	FreshModel tempA = db.createModel("TempA");
	FreshModel tempB = db.createModel("TempB");
	Serial.printf(
	    "Temp models opened=%u/%u\n",
	    static_cast<unsigned>(static_cast<bool>(tempA)),
	    static_cast<unsigned>(static_cast<bool>(tempB))
	);

	printResult("drop all", db.dropAllModels());
	printResult("sync model changes", db.forceSync());
}

void loop() {
	delay(1000);
}
