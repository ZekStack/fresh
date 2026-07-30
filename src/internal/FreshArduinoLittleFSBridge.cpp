#include "FreshArduinoLittleFSBridge.h"

#include <LittleFS.h>

bool FreshArduinoLittleFSMount(
    bool formatOnFailure,
    const char *mountPath,
    uint8_t maxOpenFiles,
    const char *partitionLabel
) {
	return LittleFS.begin(formatOnFailure, mountPath, maxOpenFiles, partitionLabel);
}

void FreshArduinoLittleFSUnmount() {
	LittleFS.end();
}

const char *FreshArduinoLittleFSMountPath() {
	return LittleFS.mountpoint();
}
