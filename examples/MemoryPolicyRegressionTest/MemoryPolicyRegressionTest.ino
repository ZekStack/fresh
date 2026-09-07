#include <Fresh.h>

namespace {

bool failed = false;

void expect(bool condition, const char *message) {
	if (condition) return;
	failed = true;
	Serial.print("FAIL: ");
	Serial.println(message);
}

void verifyLittleFSConstraint() {
	FreshConfig config;
	config.syncIntervalMS = 60000;
	config.memory.allocation = Strata::Placement::Internal;
	config.memory.taskStack = Strata::Placement::PreferExternal;

	Fresh db;
	FreshResult init = db.init("/fresh-memory-policy-regression", config);
	expect(static_cast<bool>(init), "LittleFS should accept PreferExternal and constrain the task stack");
	if (!init) return;

	const FreshDiagnostics diagnostics = db.diagnostics();
	expect(
	    diagnostics.allocationPlacement == Strata::Placement::Internal,
	    "allocation diagnostics must preserve the configured instance policy"
	);
	expect(
	    diagnostics.requestedSyncTaskStackPlacement == Strata::Placement::PreferExternal,
	    "diagnostics must preserve the requested task-stack placement"
	);
	expect(
	    diagnostics.effectiveSyncTaskStackPlacement == Strata::Placement::Internal,
	    "LittleFS must constrain PreferExternal task stacks to internal memory"
	);
	expect(
	    diagnostics.syncTaskStackConstraint == FreshTaskStackConstraint::StorageRequiresInternal,
	    "LittleFS must report the storage task-stack constraint"
	);
	expect(
	    diagnostics.syncTaskStackRegion == Strata::Region::Internal,
	    "LittleFS sync task must actually use an internal stack"
	);
	expect(
	    diagnostics.backupBufferPlacement == Strata::Placement::Internal,
	    "backup buffer must inherit the configured allocation policy"
	);
	expect(
	    diagnostics.backupBufferRegion == Strata::Region::Internal,
	    "internal backup-buffer policy must produce an internal allocation"
	);

	FreshResult stopped = db.deinit(FreshDeinitOptions{.sync = false, .timeoutMS = 5000});
	expect(static_cast<bool>(stopped), "memory-policy database should deinitialize cleanly");
}

void verifyHardExternalRequirement() {
	FreshConfig config;
	config.memory.taskStack = Strata::Placement::RequireExternal;

	Fresh db;
	FreshResult init = db.init("/fresh-memory-policy-regression", config);
	expect(!init, "LittleFS must reject a hard external task-stack requirement");
	expect(
	    init.status == FreshStatus::InvalidArgument,
	    "incompatible task-stack requirements must report InvalidArgument"
	);
}

} // namespace

void setup() {
	Serial.begin(115200);
	delay(250);

	verifyLittleFSConstraint();
	verifyHardExternalRequirement();

	Serial.println(failed ? "MemoryPolicyRegressionTest FAILED" : "MemoryPolicyRegressionTest PASSED");
}

void loop() {
	delay(1000);
}
