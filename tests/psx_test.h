#pragma once

/*
 * Minimal host-side test harness. No external dependencies.
 *
 * Usage:
 *   #include "psx_test.h"
 *   int main(void) {
 *       PSX_CHECK(1 + 1 == 2);
 *       return PSX_TEST_SUMMARY();
 *   }
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int psx_test_failures;
static int psx_test_checks;

#define PSX_CHECK(cond)                                                      \
    do {                                                                     \
        psx_test_checks++;                                                   \
        if(!(cond)) {                                                        \
            psx_test_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while(0)

#define PSX_CHECK_MSG(cond, ...)                                             \
    do {                                                                     \
        psx_test_checks++;                                                   \
        if(!(cond)) {                                                        \
            psx_test_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);  \
            fprintf(stderr, __VA_ARGS__);                                    \
            fputc('\n', stderr);                                             \
        }                                                                    \
    } while(0)

#define PSX_CHECK_EQ(a, b)                                                   \
    do {                                                                     \
        long long _a = (long long)(a);                                       \
        long long _b = (long long)(b);                                       \
        psx_test_checks++;                                                   \
        if(_a != _b) {                                                       \
            psx_test_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n",         \
                    __FILE__, __LINE__, #a, #b, _a, _b);                     \
        }                                                                    \
    } while(0)

#define PSX_CHECK_STR_EQ(a, b)                                               \
    do {                                                                     \
        const char *_a = (a);                                                \
        const char *_b = (b);                                                \
        psx_test_checks++;                                                   \
        if(!_a || !_b || strcmp(_a, _b) != 0) {                              \
            psx_test_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: \"%s\" == \"%s\"\n", __FILE__,      \
                    __LINE__, _a ? _a : "(null)", _b ? _b : "(null)");       \
        }                                                                    \
    } while(0)

#define PSX_TEST_SUMMARY()                                                   \
    (printf("%s: %d checks, %d failures\n", __FILE__, psx_test_checks,       \
            psx_test_failures),                                              \
     psx_test_failures == 0 ? 0 : 1)
