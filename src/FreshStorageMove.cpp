#include "FreshStorage.h"

#include "internal/FreshStorageAccessState.h"

#include <utility>

FreshStorage::FreshStorage(FreshStorage &&other) noexcept
    : _type(other._type),
      _state(other._state),
      _mountPath(std::move(other._mountPath)),
      _protectedPath(std::move(other._protectedPath)),
      _fileRegistry(std::move(other._fileRegistry)),
      _accessOwner(std::move(other._accessOwner)) {
	if (_accessOwner) _accessOwner->rebind(this);
	other._state = FreshStorageState::Uninitialized;
	other._protectedPath.clear();
}
