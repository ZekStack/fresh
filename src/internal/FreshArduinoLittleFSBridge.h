#pragma once

#include <cstddef>
#include <cstdint>

bool FreshArduinoLittleFSMount(
    bool formatOnFailure,
    const char *mountPath,
    uint8_t maxOpenFiles,
    const char *partitionLabel
);
void FreshArduinoLittleFSUnmount();
const char *FreshArduinoLittleFSMountPath();
