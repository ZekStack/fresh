## FreshDB

---

### Rules

*   The data must be in ram and synced to LittleFS via a dedicated sync task in the background
*   Must prioritize PSRAM if available
*   tracks LittleFS usage
*   Must never touch flash in public methods except initialization. Every other thing must happen in the dedicated sync task
*   Must have pluggable compressions / decompressions when writing and reading to / from flash. Default is a lightweight MessagePack format which ArduinoJson v7 supports very well
*   Must use ESP IDF flavour FreeRTOS task with byte stack sizes
*   Must automatically handle "createdAt", “updatedAt” and "\_id" fields. time is epoch system time from 1970, “\_id” is an auto generated unique id for the document
*   All callbacks must be std::bindable so we can bind private methods in a class
*   Must be fully thread safe with FreeRTOS mutexes
*   No exceptions, no throw. everything is handled by return results
*   We can use everything up to c++ 20 but be mindful about embedded constrains

---

### Example usage

```src
#include <FreshDB.h>

Fresh db;
FreshModel users;
FreshModel sensors;
FreshModel logs;

FreshConfig config = {
	.syncIntervalMS = 5000; 		// default 5sec
	.syncTaskPriority = 1; 			// default 1
	.syncTaskCore = tskNO_AFFINITY; // default any core
	.syncTaskStackSize = 8146; 		// default is 8146 bytes
	.eraseOnFileSystemFailure = false	// will erase the whole LittleFS if mount fails
	.compressionType = FreshCompressionType::MessagePack // will implement several others later
	.defaltModelType = FreshModelType::General // Normal mongoose like document based db models by default
};

void setup(){
	Serial.begin(115200);
	
	// DB reads every document from LittleFS into ram at init time.
	FreshResult result = db.init("/db",config);
	if( !result ){ Serial.println(result.message); return; }
	
	// setup custom callback when a sync happens
	db.onSync([](FreshResult syncResult){
		Serial.println(syncResult.message);
	});
	
	// Print out what event happened ( update, create, etc... ). Mostly for logging reasons
	db.onEvent([](FreshEvent event){
		FreshEventType type = event.type;
		std::string modelName = event.modelName;
		std::string eventName = db.eventToString(type);
		Serial.printf("%s event happened on %s\n", eventName, modelName);
	});
	
	// setup a callback so the db not necessary uses the system time
	// but an alternative one for the documents
	db.onTimeGet([](){
		return millis();
	});
	
	// Creating an in memory model. It will create the LittleFS directory if it does not exists yet
	users = db.createModel("User");
	if( !users ){ Serial.println("Failed to create User model"); return; }
	
	JsonDocument newUser;
	newUser["name"] = "Panna";
	newUser["age"] = 19;
	// Will create a new user, modifies newUser json doc in place to avoid copy
	FreshResult createRes = users.create(newUser);
	if( !createRes ){ Serial.println(createRes.message); return; }
	
	// Finds a doc by it's id
	FreshResult findByIdRes = users.findById(newUser["_id"]);
	if( !findByIdRes ){ Serial.println(findByIdRes.message); return; }
	JsonDocument foundById = findByIdRes.doc;
	// Do something with the found doc
	
	// Returns the first match
	FreshResult findOneRes = users.findOne("age",19);
	if( !findOneRes ){ Serial.println(findOneRes.message); return; }
	JsonDocument foundOneResDoc = findOneRes.doc;
	// Do something with the found doc
	
	// Finds several by a custom filter
	FreshResult findRes = users.find([](JsonDocument doc){ return doc["age"] > 18; });
	if( !findRes ){ Serial.println(findRes.message); return; }
	JsonDocument arrayOfDocs = findRes.doc;
	// Do something with the found doc
	
	// Returns at the first match if stopAtFirst is true but still an array
	bool stopAtFirst = true;
	FreshResult findFirstRes = users.find([](JsonDocument doc){ return doc["age"] > 18; }, stopAtFirst);
	if( !findFirstRes ){ Serial.println(findFirstRes.message); return; }
	JsonDocument arrayOfDocsButOnlyFirstOne = findFirstRes.doc;
	// Do something with the found doc
	
	// Updates the first match
	JsonDocument pannaUser;
	pannaUser["age"] = 20;
	FreshResult updateOneRes = users.updateOne([](JsonDocument doc){ return doc["age"] == 19; },pannaUser);
	if( !updateOneRes ){ Serial.println(updateOneRes.message); return; }
	
	// Updates every match
	JsonDocument pannaUser;
	pannaUser["age"] = 20;
	FreshResult updateRes = users.update([](JsonDocument doc){ return doc["age"] == 19; }, pannaUser);
	if( !updateRes ){ Serial.println(updateRes.message); return; }
	
	// Updates by id
	JsonDocument newPannaUser;
	pannaUser["name"] = "NewPanna";
	FreshResult updateByIdRes = users.updateById(newUser["_id"],newPannaUser);
	if( !updateByIdRes ){ Serial.println(updateByIdRes.message); return; }
}

// Testing model specific things
void sensorModelTesting(){
	sensors = db.createModel("Sensor");
	if( !users ){ Serial.println("Failed to create Sensor model"); return; }
	
	// set a custom validator function which will be called on create or update
	sensors.setValidator([](JsonDocument doc){
		return !doc["type"].isNull();
	});
	
	// Or return a message on failure
	sensors.setValidator([](JsonDocument doc){
		bool hasType = !doc["type"].isNull();
		return {
			.result = hasType,
			.message = "A sensor must have a type name!", 
		};
	});
}

// Testing db specific things
void otherDBMethodsTesting(){
	// remove an entire model. It will be removed from LittleFS at next sync
	FreshResult dropModelResult = db.dropModel("Sensor");
	if( !dropModelResult ){
		Serial.println("Failed to drop Sensor model");
	}
	
	// remove every model from the db. Everything will be removed from LittleFS at next sync
	FreshResult dropAllResult = db.dropAllModel();
	if( !dropAllResult ){
		Serial.println("Failed to drop all model from db");
	}
	
	// remove a set of models. All selected models will be removed from LittleFS at next sync
	FreshResult dropSelectedResult = db.dropModels({"Sensor","User"});
	if( !dropSelectedResult ){
		Serial.println("Failed to drop selected models from db");
	}
	
	// rename a model ( old, new )
	FreshResult renameModelResult = db.renameModel("Sensor","Sensors");
	if( !renameModelResult ){
		Serial.println("Failed to rename Sensor model");
	}
	
	// Normal persistence is handled by the background sync task.
}

// Testing alternative models
void alternativeModelsTesting(){
	// A stream model will only be a single file which can be opened and streamed into
	// It must support traditional streams and arduino streams as well
	logs = db.createModel("Log", FreshModelType::Stream);
	if( !logs ){ Serial.println("Failed to create Log model"); return; }

    // append an object
	JsonDocument doc;
	doc["lala"] = "lele";
	logs.append(doc);
}
```

My mental model is the following for the library

```src
// Model
class FreshModel {
	public:
		FreshModel(const char* name); // constructor
	private:
};
// DB
class Fresh {
	public:
		FreshResult init(const char* dbName, FreshConfig config);
		FreshModel createModel(const char* modelName); // Creates a FreshModel in memory and later creates the directory
	private:
		void syncTask();
};
```

Database backup

*   backup must use a single large file
*   backup must be streamable so we can send it trought ESPAsyncWebServer response for user download
*   backup must use a really large compression so it does not take up so much space on the file system
*   backup must be able to get an alternative db object as param so we are able to stream it to another db which is only for backup

```src
void createBackup(){
	db.onBackupProgress([](FreshBackupInfo info){
	    int percent = (info.progress * 100) / info.total;
		Serial.printf("DB Backup progress: %%d", percent);
	});
	db.onBackupEnd([](FreshBackupInfo info){
		// size is in kb
		Serial.println("DB Backup done! Final backup size: %d", info.size);
	});
	db.onBackupStart([](FreshBackupInfo info){
		Serial.println("DB Backup starting! Estimated backup size: %d", info.estimatedSize);
	});
	db.onBackupError([](FreshBackupInfo info){
		std::string infoString = db.backupErrorToString(info.error);
		Serial.println(infoString);
	});
	// backup uses the sync task and it streams the whole db into one file.
	// The backup itself must not happen in the caller's task.
	db.startBackup();
}
```
