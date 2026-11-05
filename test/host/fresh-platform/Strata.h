#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
namespace Strata {
enum class Placement { Default, Internal, PreferExternal, RequireExternal };
enum class Region { Unknown, Internal, External };
struct MemoryPolicy {
	Placement allocation = Placement::Default;
	Placement taskStack = Placement::Internal;
};
inline bool validMemoryPolicy(const MemoryPolicy &) {
	return true;
}
inline void *allocate(size_t n, Placement) {
	return std::malloc(n);
}
inline void *reallocate(void *p, size_t n, Placement) {
	return std::realloc(p, n);
}
inline void free(void *p) {
	std::free(p);
}
inline Region regionOf(const void *) {
	return Region::Internal;
}
template <class T> T *allocateArray(size_t n, Placement p) {
	return static_cast<T *>(allocate(n * sizeof(T), p));
}
} // namespace Strata

namespace Strata {
template <class T> struct Allocator {
	using value_type = T;
	Placement placement;
	explicit Allocator(Placement p = Placement::Default) : placement(p) {
	}
	template <class U> Allocator(const Allocator<U> &other) : placement(other.placement) {
	}
	T *allocate(size_t n) {
		auto p = allocateArray<T>(n, placement);
		if (!p)
			throw std::bad_alloc();
		return p;
	}
	void deallocate(T *p, size_t) {
		Strata::free(p);
	}
	template <class U> bool operator==(const Allocator<U> &o) const {
		return placement == o.placement;
	}
};
} // namespace Strata
