/* Minimal hand-rolled C test framework for stretto.
 *
 * Usage:
 *   #include "tests/unit/test.h"
 *
 *   TEST(name_of_test) {
 *       ASSERT_EQ(2 + 2, 4);
 *       ASSERT_NEAR(3.14, 3.0, 0.5);
 *   }
 *
 *   int main(void) { return RUN_ALL(); }
 *
 * Each TEST registers itself via constructor attributes. RUN_ALL runs
 * every registered test, prints a summary, returns the number of
 * failed tests (so the shell sees nonzero on failure).
 */
#ifndef STRETTO_TEST_H
#define STRETTO_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Common synth-init for tests. Runs voice_pool_init + effects_init +
   gen_seed(0) + gen_init exactly once per test binary (each binary
   has its own static guard). Tests that need a deterministic synth
   state should call test_init_synth() at the top of each TEST(). The
   init is idempotent thanks to the guard, so calling it from every
   test is cheap. */
#include "../../voice.h"
#include "../../effects.h"
#include "../../gen.h"

static inline void test_init_synth(void) {
    static int done = 0;
    if (done) return;
    voice_pool_init();
    effects_init();
    gen_seed(0);
    gen_init();
    done = 1;
}

#define MAX_TESTS 256

typedef void (*test_fn)(int *failed);

static struct {
    const char *name;
    test_fn     fn;
} t_registry[MAX_TESTS];
static int t_count = 0;

static inline void t_register(const char *name, test_fn fn) {
    if (t_count < MAX_TESTS) {
        t_registry[t_count].name = name;
        t_registry[t_count].fn   = fn;
        t_count++;
    }
}

#define TEST(name)                                                       \
    static void test_##name(int *failed);                                \
    __attribute__((constructor))                                         \
    static void register_##name(void) {                                  \
        t_register(#name, test_##name);                                  \
    }                                                                    \
    static void test_##name(int *failed)

#define ASSERT_TRUE(cond) do {                                           \
    if (!(cond)) {                                                       \
        fprintf(stderr, "    ASSERT_TRUE failed at %s:%d: %s\n",         \
                __FILE__, __LINE__, #cond);                              \
        (*failed)++;                                                     \
    }                                                                    \
} while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do {                                             \
    long long _a = (long long)(a);                                       \
    long long _b = (long long)(b);                                       \
    if (_a != _b) {                                                      \
        fprintf(stderr, "    ASSERT_EQ failed at %s:%d: %s == %s "       \
                "  (got %lld vs %lld)\n",                                \
                __FILE__, __LINE__, #a, #b, _a, _b);                     \
        (*failed)++;                                                     \
    }                                                                    \
} while (0)

#define ASSERT_NE(a, b) do {                                             \
    long long _a = (long long)(a);                                       \
    long long _b = (long long)(b);                                       \
    if (_a == _b) {                                                      \
        fprintf(stderr, "    ASSERT_NE failed at %s:%d: %s != %s "       \
                "  (both == %lld)\n",                                    \
                __FILE__, __LINE__, #a, #b, _a);                         \
        (*failed)++;                                                     \
    }                                                                    \
} while (0)

#define ASSERT_NEAR(a, b, tol) do {                                      \
    double _a = (double)(a);                                             \
    double _b = (double)(b);                                             \
    double _t = (double)(tol);                                           \
    if (fabs(_a - _b) > _t) {                                            \
        fprintf(stderr, "    ASSERT_NEAR failed at %s:%d: "              \
                "%s ~ %s +/- %s  (|%g - %g| = %g)\n",                    \
                __FILE__, __LINE__, #a, #b, #tol, _a, _b, fabs(_a - _b));\
        (*failed)++;                                                     \
    }                                                                    \
} while (0)

#define ASSERT_BETWEEN(x, lo, hi) do {                                   \
    long long _x  = (long long)(x);                                      \
    long long _lo = (long long)(lo);                                     \
    long long _hi = (long long)(hi);                                     \
    if (_x < _lo || _x > _hi) {                                          \
        fprintf(stderr, "    ASSERT_BETWEEN failed at %s:%d: "           \
                "%s in [%s, %s]  (got %lld)\n",                          \
                __FILE__, __LINE__, #x, #lo, #hi, _x);                   \
        (*failed)++;                                                     \
    }                                                                    \
} while (0)

#define RUN_ALL() ({                                                     \
    int total_failed = 0;                                                \
    int passed = 0;                                                      \
    for (int i = 0; i < t_count; i++) {                                  \
        int failed = 0;                                                  \
        printf("  %-40s ", t_registry[i].name);                          \
        fflush(stdout);                                                  \
        t_registry[i].fn(&failed);                                       \
        if (failed) {                                                    \
            printf("FAIL (%d assertion%s)\n",                            \
                   failed, failed == 1 ? "" : "s");                      \
            total_failed++;                                              \
        } else {                                                         \
            printf("ok\n");                                              \
            passed++;                                                    \
        }                                                                \
    }                                                                    \
    printf("\n%d passed, %d failed (of %d)\n",                           \
           passed, total_failed, t_count);                               \
    total_failed;                                                        \
})

#endif
