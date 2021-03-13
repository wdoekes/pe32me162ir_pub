#ifndef INCLUDED_BOGO_XTOA_H
#define INCLUDED_BOGO_XTOA_H

#include <cstdio>

inline char *dtostrf(float value, int size, int prec, char *buf)
{
    snprintf(buf, 20, "%*.*f", size, prec, value);
    return buf;
}

inline char *dtostrf(double value, int size, int prec, char *buf)
{
    snprintf(buf, 20, "%*.*lf", size, prec, value);
    return buf;
}

template<class T> inline void xtoa(const char *fmt, T value, char *buf)
{
    snprintf(buf, 30, fmt, value);
}

inline void utoa(unsigned value, char* buf, int base)
{
    switch (base) {
    case 8: xtoa("%o", value, buf); break;
    case 10: xtoa("%u", value, buf); break;
    case 16: xtoa("%x", value, buf); break;
    default: abort();
    }
}

inline void itoa(int value, char* buf, int base)
{
    switch (base) {
    case 8: xtoa("%o", value, buf); break;
    case 10: xtoa("%d", value, buf); break;
    case 16: xtoa("%x", value, buf); break;
    default: abort();
    }
}

inline void ltoa(long value, char* buf, int base)
{
    switch (base) {
    case 8: xtoa("%lo", value, buf); break;
    case 10: xtoa("%ld", value, buf); break;
    case 16: xtoa("%lx", value, buf); break;
    default: abort();
    }
}

inline void ultoa(unsigned long value, char* buf, int base)
{
    switch (base) {
    case 8: xtoa("%lo", value, buf); break;
    case 10: xtoa("%lu", value, buf); break;
    case 16: xtoa("%lx", value, buf); break;
    default: abort();
    }
}

#endif //INCLUDED_BOGO_XTOA_H
