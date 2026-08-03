#pragma once

#include "FreshMutex.h"

#include <atomic>
#include <memory>

class FreshStorage;

struct FreshStorageAccessState {
	// Recursive because facade entry invokes the same guarded storage methods
	// used by internal persistence while retaining this lifetime barrier.
	FreshMutex mutex;
	std::atomic<FreshStorage *> storage{nullptr};
};

class FreshStorageAccessOwner {
  public:
	explicit FreshStorageAccessOwner(FreshStorage *storage);
	~FreshStorageAccessOwner();

	FreshStorageAccessOwner(const FreshStorageAccessOwner &) = delete;
	FreshStorageAccessOwner &operator=(const FreshStorageAccessOwner &) = delete;
	FreshStorageAccessOwner(FreshStorageAccessOwner &&other) noexcept;
	FreshStorageAccessOwner &operator=(FreshStorageAccessOwner &&other) = delete;

	std::shared_ptr<FreshStorageAccessState> state() const {
		return _state;
	}

	void rebind(FreshStorage *storage);

  private:
	std::shared_ptr<FreshStorageAccessState> _state;
};
