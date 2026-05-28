#include <Arduino.h>
#include <Fresh.h>
#include <vector>

Fresh db;
Fresh restoreDb;
FreshModel users;
FreshModel restoredUsers;
std::vector<uint8_t> backupBytes;
size_t streamedBytes = 0;
bool backupRequested = false;
bool backupFinished = false;
bool restoreDone = false;

void printBackupPercent(const char *label, FreshBackupInfo info) {
	int percent = info.total == 0 ? 0 : static_cast<int>((info.progress * 100) / info.total);
	Serial.printf("%s %d%% size=%u\n", label, percent, static_cast<unsigned>(info.size));
}

void setup() {
	Serial.begin(115200);

	FreshConfig config;
	config.backupBufferSize = 2048;
	FreshResult initResult = db.init("/fresh_backup", config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}
	FreshResult restoreInitResult = restoreDb.init("/fresh_backup_restore", config);
	if (!restoreInitResult) {
		Serial.println(restoreInitResult.message.c_str());
		return;
	}

	db.onBackupStart([](FreshBackupInfo info) {
		Serial.printf("backup start estimated=%u\n", static_cast<unsigned>(info.estimatedSize));
		backupBytes.clear();
		backupBytes.reserve(info.estimatedSize);
	});
	db.onBackupProgress([](FreshBackupInfo info) {
		printBackupPercent("backup progress", info);
	});
	db.onBackupEnd([](FreshBackupInfo info) {
		printBackupPercent("backup done", info);
		backupFinished = true;
	});
	db.onBackupError([](FreshBackupInfo info) {
		Serial.printf("backup error=%u message=%s\n", static_cast<unsigned>(info.error), info.result.message.c_str());
		backupFinished = true;
	});

	users = db.createModel("BackupUser");
	for (int i = 0; i < 3; ++i) {
		JsonDocument user;
		user["name"] = String("user-") + i;
		user["score"] = i * 10;
		users.create(user);
	}

	db.forceSync();
	FreshResult backupResult = db.startBackup();
	backupRequested = static_cast<bool>(backupResult);
	Serial.println(backupResult.message.c_str());
}

void loop() {
	if (!backupRequested) {
		delay(1000);
		return;
	}

	uint8_t buffer[256];
	size_t read = db.readBackup(buffer, sizeof(buffer), 50);
	if (read > 0) {
		backupBytes.insert(backupBytes.end(), buffer, buffer + read);
		streamedBytes += read;
		Serial.printf("read backup chunk=%u total=%u\n", static_cast<unsigned>(read), static_cast<unsigned>(streamedBytes));
	}

	if (backupFinished && read == 0 && !restoreDone) {
		FreshResult importResult = restoreDb.backupImport(backupBytes.data(), backupBytes.size());
		Serial.printf("import: %s affected=%u\n", importResult.message.c_str(), static_cast<unsigned>(importResult.affectedCount));
		restoredUsers = restoreDb.model("BackupUser");
		if (restoredUsers) {
			FreshResult found = restoredUsers.findOne("name", "user-1");
			Serial.printf("restored find: %s affected=%u\n", found.message.c_str(), static_cast<unsigned>(found.affectedCount));
		}
		restoreDone = true;
	}

	FreshResult status = db.backupStatus();
	if (status.status != FreshStatus::Ok && status.status != FreshStatus::BackupNotRunning) {
		Serial.println(status.message.c_str());
		backupFinished = true;
	}
}
