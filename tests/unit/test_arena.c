/* Unit tests for arena.c.
 *
 * The arena is a single static pool with a bump allocator. Because
 * the pool and bump cursor are file-static, we exercise them via
 * arena_alloc / arena_used; we cannot reset across tests.  All tests
 * in this file therefore run sequentially against a single arena.
 */
#include "test.h"
#include "../../arena.h"
#include <stdint.h>

TEST(arena_starts_empty) {
    /* This must be the first test to actually see "used = 0".
       Allocator is global so subsequent tests cannot assert this. */
    ASSERT_EQ(arena_used(), 0);
}

TEST(arena_alloc_returns_nonnull) {
    void *p = arena_alloc(16);
    ASSERT_TRUE(p != NULL);
}

TEST(arena_bumps_used) {
    size_t before = arena_used();
    arena_alloc(32);
    size_t after = arena_used();
    ASSERT_EQ(after - before, 32);
}

TEST(arena_8byte_aligns_up) {
    /* Asking for 9 bytes should bump by 16 (rounded up to 8-byte
       multiple). 9 -> next multiple of 8 is 16. */
    size_t before = arena_used();
    arena_alloc(9);
    size_t after = arena_used();
    ASSERT_EQ(after - before, 16);
}

TEST(arena_pointers_are_8byte_aligned) {
    /* Each allocation must start on an 8-byte boundary. */
    void *a = arena_alloc(7);
    void *b = arena_alloc(7);
    void *c = arena_alloc(7);
    ASSERT_EQ(((uintptr_t)a) % 8, 0);
    ASSERT_EQ(((uintptr_t)b) % 8, 0);
    ASSERT_EQ(((uintptr_t)c) % 8, 0);
}

TEST(arena_returns_distinct_pointers) {
    char *a = arena_alloc(16);
    char *b = arena_alloc(16);
    ASSERT_TRUE(b - a >= 16);
}

TEST(arena_writes_persist) {
    /* Sanity: arena memory is actually writable and readable. */
    char *p = arena_alloc(64);
    for (int i = 0; i < 64; i++) p[i] = (char)(i * 3 + 1);
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        if (p[i] != (char)(i * 3 + 1)) { ok = 0; break; }
    }
    ASSERT_TRUE(ok);
}

int main(void) {
    return RUN_ALL();
}
