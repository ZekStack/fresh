#pragma once

#include "../FreshFile.h"
#include "FreshMutex.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

enum class FreshFileOrigin : uint8_t {
	Application,
	Internal,
};

struct FreshStorageFileRegistry;

struct FreshFileState {
	FreshMutex mutex;
	std::unique_ptr<FreshFileBackend> backend;
	std::weak_ptr<FreshStorageFileRegistry> registry;
	FreshFileOrigin origin = FreshFileOrigin::Application;
	bool registered = false;
	int lastError = 0;
};

struct FreshStorageFileRegistry {
	explicit FreshStorageFileRegistry(size_t limit) : maxOpenFiles(limit) {
	}

	FreshMutex mutex;
	std::vector<std::weak_ptr<FreshFileState>> files;
	std::atomic<size_t> totalOpenFiles{0};
	std::atomic<size_t> applicationOpenFiles{0};
	std::atomic<size_t> internalOpenFiles{0};
	std::atomic<bool> acceptApplicationFiles{true};
	size_t maxOpenFiles = SIZE_MAX;
};

FreshResult FreshCloseFileState(
    const std::shared_ptr<FreshFileState> &state,
    bool syncBeforeClose
);
