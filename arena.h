#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/* 128 KB. Bumped from PLAN.md's 64 KB target after stereo delay
   (44 KB) plus Schroeder reverb (~28 KB) plus voice pool (~10 KB)
   exceeded the original budget. The size threshold from PLAN.md
   Phase 4 is already broken on the packed binary; the arena follows. */
#define HEAP_BYTES 131072

void *arena_alloc(size_t n);
size_t arena_used(void);

#endif
