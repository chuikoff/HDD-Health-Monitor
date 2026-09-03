#pragma once
#include <stdio.h>
#include <stddef.h>

/* MSVC _snprintf may omit the trailing NUL on overflow. Always force it. */
#define safe_snprintf(buf, fmt, ...) \
    do { \
        (void)_snprintf((buf), sizeof(buf), (fmt), ##__VA_ARGS__); \
        (buf)[sizeof(buf) - 1] = '\0'; \
    } while (0)

#define safe_snprintf_n(buf, n, fmt, ...) \
    do { \
        size_t _n_ = (size_t)(n); \
        if (_n_ > 0) { \
            (void)_snprintf((buf), _n_, (fmt), ##__VA_ARGS__); \
            (buf)[_n_ - 1] = '\0'; \
        } \
    } while (0)
