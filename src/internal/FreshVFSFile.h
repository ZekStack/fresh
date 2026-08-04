#pragma once

#include "../FreshFile.h"
#include "../FreshStorage.h"

#include <memory>

FreshResult FreshOpenVFSFile(
    const char *resolvedPath,
    FreshOpenMode mode,
    std::unique_ptr<FreshFileBackend> &backend
);
