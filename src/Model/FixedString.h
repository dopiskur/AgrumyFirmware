#ifndef FixedString_H
#define FixedString_H

#include <string.h>
#include <stddef.h>

// Bounded copy into a fixed char array - always NUL-terminated, silently truncates, nullptr copies as "".
template <size_t N>
inline void copyStr(char (&dst)[N], const char* src)
{
    if (src == nullptr)
    {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, N - 1);
    dst[N - 1] = 0;
}

#endif
