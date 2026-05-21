#include <stdio.h>
#include <string.h>

#define N 16

int main(void) {
    printf("static const unsigned short euclid_table[%d] = {\n", N + 1);
    for (int k = 0; k <= N; k++) {
        char pat[N];
        memset(pat, 0, sizeof pat);
        for (int i = 0; i < k; i++) {
            pat[(i * N) / (k == 0 ? 1 : k)] = 1;
        }
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
