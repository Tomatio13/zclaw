#ifndef UTF8_UTILS_H
#define UTF8_UTILS_H

#include <stddef.h>

size_t utf8_safe_prefix_bytes(const char *src, size_t max_bytes);
void utf8_safe_strlcpy(char *dst, size_t dst_size, const char *src);

#endif  // UTF8_UTILS_H
