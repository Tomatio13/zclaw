#include "utf8_utils.h"

#include <stdbool.h>

size_t utf8_safe_prefix_bytes(const char *src, size_t max_bytes)
{
    if (!src || max_bytes == 0) {
        return 0;
    }

    size_t i = 0;
    size_t last_safe = 0;
    while (src[i] != '\0' && i < max_bytes) {
        unsigned char c = (unsigned char)src[i];
        size_t seq_len = 1;

        if ((c & 0x80) == 0x00) {
            seq_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            seq_len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            seq_len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            seq_len = 4;
        } else {
            seq_len = 1;
        }

        if (i + seq_len > max_bytes) {
            break;
        }

        bool valid_continuations = true;
        for (size_t j = 1; j < seq_len; j++) {
            unsigned char cc = (unsigned char)src[i + j];
            if (cc == '\0' || (cc & 0xC0) != 0x80) {
                valid_continuations = false;
                break;
            }
        }

        if (!valid_continuations) {
            seq_len = 1;
        }

        i += seq_len;
        last_safe = i;
    }

    return last_safe;
}

void utf8_safe_strlcpy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t max_copy = dst_size - 1;
    size_t copy_len = utf8_safe_prefix_bytes(src, max_copy);
    for (size_t i = 0; i < copy_len; i++) {
        dst[i] = src[i];
    }
    dst[copy_len] = '\0';
}
