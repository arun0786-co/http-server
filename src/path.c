#include "path.h"

#include <string.h>

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hp_resolve(const char *target, size_t len, char *out, size_t out_size) {
    if (len == 0 || target[0] != '/') return 400;
    char dec[8192];
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = target[i];
        if (c == '?' || c == '#') break;
        if (c == '%') {
            if (i + 2 >= len) return 400;
            int hi = hex(target[i + 1]), lo = hex(target[i + 2]);
            if (hi < 0 || lo < 0) return 400;
            c = (char)(hi * 16 + lo);
            i += 2;
        }
        /* No control bytes, raw or decoded. A NUL in particular would end the C string early:
           "/..<NUL>x" would be opened as "/..", outside the root. The parser already refuses
           raw control bytes in a target, but this check must not depend on that. */
        if ((unsigned char)c < 0x20 || c == 0x7f) return 400;
        if (n + 1 >= sizeof dec) return 414;
        dec[n++] = c;
    }
    /* walk the segments, keeping a stack of where each kept segment starts in `out` */
    size_t starts[1024], depth = 0, o = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && dec[i] == '/') ++i;
        size_t s = i;
        while (i < n && dec[i] != '/') ++i;
        size_t seg = i - s;
        if (seg == 0 || (seg == 1 && dec[s] == '.')) continue;
        if (seg == 2 && dec[s] == '.' && dec[s + 1] == '.') {
            if (depth == 0) return 403;
            o = starts[--depth];
            continue;
        }
        if (memchr(dec + s, '\\', seg)) return 400;
        if (depth == sizeof starts / sizeof starts[0] || o + 1 + seg + 1 > out_size) return 414;
        starts[depth++] = o;
        out[o++] = '/';
        memcpy(out + o, dec + s, seg);
        o += seg;
    }
    if (o == 0) out[o++] = '/';
    if (o + 1 > out_size) return 414;
    out[o] = '\0';
    return 0;
}
