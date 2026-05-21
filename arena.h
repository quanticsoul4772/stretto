#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

#define HEAP_BYTES 65536

void *arena_alloc(size_t n);
size_t arena_used(void);

#endif
