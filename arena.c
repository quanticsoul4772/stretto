#include "arena.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t pool[HEAP_BYTES] __attribute__((aligned(64)));
static size_t bump;

void *arena_alloc(size_t n) {
    size_t aligned = (n + 7) & ~(size_t)7;
    if (bump + aligned > HEAP_BYTES) {
        fprintf(stderr, "arena: oom %zu+%zu > %d\n", bump, aligned, HEAP_BYTES);
        exit(1);
    }
    void *p = &pool[bump];
    bump += aligned;
    return p;
}

size_t arena_used(void) {
    return bump;
}
