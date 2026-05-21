/* Build-time generator: emits euclid_table[N+1] as 16-bit masks of
   Bjorklund Euclidean rhythms E(k, N) for k = 0..N, with N = 16.

   Bjorklund's algorithm distributes k pulses across n steps as
   evenly as possible by repeated bucket-pair merging, analogous to
   Euclid's GCD algorithm. Compared to the previous floor-based
   approximation, true Bjorklund produces the canonical tresillo,
   cinquillo, and other rhythm patterns musicians recognize.

   Reference: Toussaint, "The Euclidean Algorithm Generates
   Traditional Musical Rhythms" (2005). */
#include <stdio.h>
#include <string.h>

#define N 16

/* Recursively concatenate count[level] copies of the level-1
   sub-sequence followed by an optional level-2 remainder. Leaves
   are level == -2 (a '1' pulse) and level == -1 (a '0' rest). */
static void build(const int *counts, const int *remainders,
                  int level, char *out, int *idx) {
    if (level == -1) {
        out[(*idx)++] = 0;
    } else if (level == -2) {
        out[(*idx)++] = 1;
    } else {
        for (int i = 0; i < counts[level]; i++) {
            build(counts, remainders, level - 1, out, idx);
        }
        if (remainders[level] != 0) {
            build(counts, remainders, level - 2, out, idx);
        }
    }
}

/* Fill `out[N]` with the Bjorklund-distributed pattern for E(k, N).
   Special cases: k == 0 -> all zeros; k >= N -> all ones. */
static void bjorklund(int k, int n, char *out) {
    memset(out, 0, n);
    if (k <= 0) return;
    if (k >= n) {
        for (int i = 0; i < n; i++) out[i] = 1;
        return;
    }

    int counts[32];
    int remainders[32];
    int level = 0;
    int divisor = n - k;
    remainders[0] = k;

    /* Euclid-style: split divisor by remainder until remainder <= 1. */
    while (1) {
        counts[level] = divisor / remainders[level];
        remainders[level + 1] = divisor % remainders[level];
        divisor = remainders[level];
        level++;
        if (remainders[level] <= 1) break;
    }
    counts[level] = divisor;

    int idx = 0;
    build(counts, remainders, level, out, &idx);
}

int main(void) {
    char pat[N];
    printf("static const unsigned short euclid_table[%d] = {\n", N + 1);
    for (int k = 0; k <= N; k++) {
        bjorklund(k, N, pat);
        unsigned short bits = 0;
        for (int i = 0; i < N; i++) {
            if (pat[i]) bits |= (1u << (N - 1 - i));
        }
        printf("    0x%04x", bits);
        if (k < N) printf(",");
        if ((k + 1) % 4 == 0) printf("\n");
    }
    printf("};\n");
    return 0;
}
