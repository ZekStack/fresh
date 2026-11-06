#pragma once
struct SignalSubscriptionHandle {
	void unsubscribe() {
	}
};
namespace zek::signal {
class Signal {
  public:
	template <class T, class E, class F> SignalSubscriptionHandle subscribeHandle(E, F) {
		return {};
	}
};
} // namespace zek::signal
using Signal = zek::signal::Signal;
