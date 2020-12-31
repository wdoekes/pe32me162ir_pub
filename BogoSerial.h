#ifndef INCLUDED_BOGOSERIAL_H
#define INCLUDED_BOGOSERIAL_H

#include "BogoArduino.h"

class BogoSerial {
public:
    BogoSerial() {}
    void begin(int baud) {}

#define PRINT_FUNCTION(type, fmt) \
    size_t print(type p) { printf(fmt, p); return 0; } \
    size_t println(type p) { printf(fmt "\n", p); return 0; } \
    size_t print(type p, int) { printf(fmt, p); return 0; } \
    size_t println(type p, int) { printf(fmt "\n", p); return 0; }

    PRINT_FUNCTION(const char *, "%s");
    PRINT_FUNCTION(double, "%g");
    PRINT_FUNCTION(float, "%f");
    PRINT_FUNCTION(int, "%d");
    PRINT_FUNCTION(long, "%ld");
    PRINT_FUNCTION(unsigned long, "%ld");

    bool operator!() { return false; }
};

static BogoSerial Serial;

#endif //INCLUDED_BOGOSERIAL_H
