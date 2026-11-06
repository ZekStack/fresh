#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
class Print {
  public:
	int writeError = 0;
	int getWriteError() const {
		return writeError;
	}
	void setWriteError(int error = 1) {
		writeError = error;
	}
	void clearWriteError() {
		writeError = 0;
	}
	virtual ~Print() = default;
	virtual size_t write(uint8_t) = 0;
	virtual size_t write(const uint8_t *p, size_t n) {
		size_t done = 0;
		while (done < n && write(p[done]))
			++done;
		return done;
	}
	size_t write(const char *p) {
		return write(reinterpret_cast<const uint8_t *>(p), std::strlen(p));
	}
};
