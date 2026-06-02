#ifndef FLUX_TEST_H
#define FLUX_TEST_H

/*
 * Minimal C unit-test scaffolding.  Header-only macros + a TEST_MAIN
 * generator that gives each suite its own main() and counters.
 *
 * Usage:
 *   TEST(name) { ASSERT_EQ(x, y); }
 *   TEST_MAIN("suite", { RUN_TEST(name); RUN_TEST(other); })
 *
 * No external dependency, no test discovery — just direct calls. Fast to
 * compile (no cmocka linkage). Exit code = number of failures (capped at 1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_test_failures;
extern int g_test_run;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                       \
    g_test_run++;                                                 \
    printf("  %-44s ", #name);                                    \
    fflush(stdout);                                               \
    int _prev = g_test_failures;                                  \
    test_##name();                                                \
    if (g_test_failures == _prev) printf("OK\n");                 \
} while (0)

#define ASSERT_TRUE(cond) do {                                    \
    if (!(cond)) {                                                \
        printf("FAIL\n    %s:%d: ASSERT_TRUE(%s)\n",              \
               __FILE__, __LINE__, #cond);                        \
        g_test_failures++;                                        \
        return;                                                   \
    }                                                             \
} while (0)

#define ASSERT_EQ(a, b) do {                                      \
    long long _a = (long long)(a), _b = (long long)(b);           \
    if (_a != _b) {                                               \
        printf("FAIL\n    %s:%d: ASSERT_EQ(%s=%lld, %s=%lld)\n",  \
               __FILE__, __LINE__, #a, _a, #b, _b);               \
        g_test_failures++;                                        \
        return;                                                   \
    }                                                             \
} while (0)

#define ASSERT_STR_EQ(a, b) do {                                  \
    const char *_a = (a), *_b = (b);                              \
    if (strcmp(_a, _b) != 0) {                                    \
        printf("FAIL\n    %s:%d: ASSERT_STR_EQ(%s=\"%s\", %s=\"%s\")\n", \
               __FILE__, __LINE__, #a, _a, #b, _b);               \
        g_test_failures++;                                        \
        return;                                                   \
    }                                                             \
} while (0)

#define ASSERT_MEM_EQ(a, b, n) do {                               \
    if (memcmp((a), (b), (n)) != 0) {                             \
        printf("FAIL\n    %s:%d: ASSERT_MEM_EQ(%s, %s, %zu)\n",   \
               __FILE__, __LINE__, #a, #b, (size_t)(n));          \
        g_test_failures++;                                        \
        return;                                                   \
    }                                                             \
} while (0)

#define TEST_MAIN(suite_name, BLOCK)                              \
int g_test_failures = 0;                                          \
int g_test_run = 0;                                               \
int main(void) {                                                  \
    printf("%s:\n", suite_name);                                  \
    BLOCK                                                         \
    int passed = g_test_run - g_test_failures;                    \
    printf("  ----------------------------------------------\n"); \
    printf("  %d passed, %d failed (of %d)\n\n",                  \
           passed, g_test_failures, g_test_run);                  \
    return g_test_failures ? 1 : 0;                               \
}

#endif
