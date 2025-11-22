#ifndef STRING_HELPERS_H
#define STRING_HELPERS_H

#include <stddef.h>

char *itoa_new(unsigned int val, char *buf, size_t bufsz);
int append_fmt(char *dst, size_t dstsize, size_t *pos, const char *fmt, ...);

#endif /* STRING_HELPERS_H */
