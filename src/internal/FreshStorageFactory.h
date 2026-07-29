#pragma once

#include "../FreshStorage.h"

#include <memory>

struct FreshResult;

FreshResult FreshValidateStorageConfig(
    FreshStorageType type,
    const FreshLittleFSConfig &littleFS,
    const FreshSDConfig &sd
);

FreshResult FreshCreateStorage(
    FreshStorageType type,
    const FreshLittleFSConfig &littleFS,
    const FreshSDConfig &sd,
    std::unique_ptr<FreshStorage> &storage
);
