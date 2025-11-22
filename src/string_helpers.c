#include "string_helpers.h"
#include <stdarg.h>
#include <string.h>

/* Portable, small integer -> string conversion that bounds to bufsz. */
char *itoa_new(unsigned int val, char *buf, size_t bufsz)
{
    size_t i = 0;
    char tmp[12]; /* enough for 32-bit unsigned */
    size_t t = 0;
    size_t start = 0;

    if (bufsz == 0) return buf;
    if (val == 0) {
        if (bufsz > 1) {
            buf[0] = '0';
            buf[1] = '\0';
        } else if (bufsz == 1) {
            buf[0] = '\0';
        }
        return buf;
    }

    while (val > 0 && t < sizeof(tmp)-1) {
        tmp[t++] = (char)('0' + (val % 10u));
        val /= 10u;
    }

    /* reverse into destination, respecting bufsz */
    if (t + 1 > bufsz) {
        /* not enough space, truncate from the front */
        start = t + 1 - bufsz;
        for (i = start; i < t; ++i) {
            buf[i - start] = tmp[t - 1 - i];
        }
        buf[bufsz - 1] = '\0';
    } else {
        for (i = 0; i < t; ++i) {
            buf[i] = tmp[t - 1 - i];
        }
        buf[t] = '\0';
    }
    return buf;
}

/* Minimal printf-like append supporting %% %s and %u. Returns -1 on truncation. */
int append_fmt(char *dst, size_t dstsize, size_t *pos, const char *fmt, ...)
{
    va_list ap;
    const char *p;
    const char *s;
    size_t len;
    unsigned int v;
    char numbuf[12];
    size_t start = 0;

    if (!dst || !pos || dstsize == 0) return -1;
    if (*pos >= dstsize) return -1;

    start = *pos;
    va_start(ap, fmt);
    p = fmt;
    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == '%') {
                if (*pos + 1 >= dstsize) { va_end(ap); return -1; }
                dst[(*pos)++] = '%';
                p++;
                continue;
            } else if (*p == 's') {
                s = va_arg(ap, const char*);
                len = 0;
                if (!s) s = "(null)";
                while (s[len]) len++;
                if (*pos + len >= dstsize) {
                    /* copy as much as fits and NUL-terminate */
                    size_t rem = dstsize > *pos ? dstsize - *pos : 0;
                    if (rem > 1) {
                        size_t tocopy = rem - 1;
                        memcpy(dst + *pos, s, tocopy);
                        *pos += tocopy;
                    }
                    if (*pos < dstsize) dst[*pos] = '\0';
                    va_end(ap);
                    return -1;
                }
                memcpy(dst + *pos, s, len);
                *pos += len;
                p++;
                continue;
            } else if (*p == 'u' || *p == 'd') {
                v = va_arg(ap, unsigned int);
                itoa_new(v, numbuf, sizeof(numbuf));
                len = 0;
                while (numbuf[len]) len++;
                if (*pos + len >= dstsize) {
                    size_t rem = dstsize > *pos ? dstsize - *pos : 0;
                    if (rem > 1) {
                        size_t tocopy = rem - 1;
                        memcpy(dst + *pos, numbuf, tocopy);
                        *pos += tocopy;
                    }
                    if (*pos < dstsize) dst[*pos] = '\0';
                    va_end(ap);
                    return -1;
                }
                memcpy(dst + *pos, numbuf, len);
                *pos += len;
                p++;
                continue;
            } else {
                /* unknown format, treat literally */
                if (*pos + 1 >= dstsize) { va_end(ap); return -1; }
                dst[(*pos)++] = '%';
                /* do not consume p here so next loop handles char */
                continue;
            }
            } else {
            if (*pos + 1 >= dstsize) {
                /* not enough room for char, ensure NUL termination and return -1 */
                if (*pos < dstsize) dst[*pos] = '\0';
                va_end(ap);
                return -1;
            }
            dst[(*pos)++] = *p++;
        }
    }
    /* NUL-terminate if space */
    if (*pos < dstsize) dst[*pos] = '\0';
    va_end(ap);
    return (int)(*pos - start);
}
