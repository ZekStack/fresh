#pragma once
#include "Print.h"
class Stream : public Print {
  public:
	virtual int available() = 0;
	virtual int read() = 0;
	virtual int peek() = 0;
	virtual void flush() = 0;
	virtual size_t readBytes(char *p, size_t n) {
		size_t i = 0;
		while (i < n) {
			int byte = read();
			if (byte < 0)
				break;
			p[i++] = static_cast<char>(byte);
		}
		return i;
	}
	size_t readBytes(uint8_t *p, size_t n) {
		return readBytes(reinterpret_cast<char *>(p), n);
	}
};
