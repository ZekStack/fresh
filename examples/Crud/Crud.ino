#include <Arduino.h>
#include <Fresh.h>

Fresh db;
FreshModel users;

void printJson(const JsonDocument &doc) {
	serializeJson(doc, Serial);
	Serial.println();
}

void printResult(const char *label, const FreshResult &result) {
	Serial.printf("%s: %s, affected=%u\n", label, result.message.c_str(), static_cast<unsigned>(result.affectedCount));
	if (result.doc.size() > 0) {
		printJson(result.doc);
	}
}

void setup() {
	Serial.begin(115200);

	FreshResult initResult = db.init("/fresh_crud");
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	users = db.createModel("CrudUser");
	if (!users) {
		Serial.println("Failed to open CrudUser model");
		return;
	}

	JsonDocument anna;
	anna["name"] = "Anna";
	anna["age"] = 24;
	anna["role"] = "admin";
	FreshResult annaCreate = users.create(anna);
	printResult("create Anna", annaCreate);

	JsonDocument bela;
	bela["name"] = "Bela";
	bela["age"] = 17;
	bela["role"] = "guest";
	FreshResult belaCreate = users.create(bela);
	printResult("create Bela", belaCreate);

	JsonDocument cecil;
	cecil["name"] = "Cecil";
	cecil["age"] = 31;
	cecil["role"] = "guest";
	FreshResult cecilCreate = users.create(cecil);
	printResult("create Cecil", cecilCreate);

	printResult("findById Anna", users.findById(anna["_id"].as<const char *>()));
	printResult("findOne role=admin", users.findOne("role", "admin"));
	printResult(
	    "find adults",
	    users.find([](const JsonDocument &doc) {
		    return doc["age"].as<int>() >= 18;
	    })
	);

	JsonDocument agePatch;
	agePatch["age"] = 25;
	printResult("updateById Anna", users.updateById(anna["_id"].as<const char *>(), agePatch));

	JsonDocument rolePatch;
	rolePatch["role"] = "member";
	printResult(
	    "updateOne guest",
	    users.updateOne(
	        [](const JsonDocument &doc) {
		        return doc["role"] == "guest";
	        },
	        rolePatch
	    )
	);

	JsonDocument activePatch;
	activePatch["active"] = true;
	printResult(
	    "update all adults",
	    users.update(
	        [](const JsonDocument &doc) {
		        return doc["age"].as<int>() >= 18;
	        },
	        activePatch
	    )
	);

	printResult("deleteById Bela", users.deleteById(bela["_id"].as<const char *>()));
	printResult(
	    "deleteOne member",
	    users.deleteOne([](const JsonDocument &doc) {
		    return doc["role"] == "member";
	    })
	);
	printResult(
	    "deleteMany active",
	    users.deleteMany([](const JsonDocument &doc) {
		    return doc["active"] == true;
	    })
	);
}

void loop() {
	delay(1000);
}
