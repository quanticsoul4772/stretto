#include <stdio.h>
#include <math.h>

#define ENV_TABLE_SIZE 256

int main(void) {
    printf("static const unsigned char env_table[%d] = {\n", ENV_TABLE_SIZE);
    double denom = 1.0 - exp(-5.0);
    for (int i = 0; i < ENV_TABLE_SIZE; i++) {
        double x = (double)i / (ENV_TABLE_SIZE - 1);
        double y = (1.0 - exp(-x * 5.0)) / denom;
        int v = (int)(y * 255.0 + 0.5);
        if (v < 0) v = 0; else if (v > 255) v = 255;
        printf("    %3d", v);
        if (i < ENV_TABLE_SIZE - 1) printf(",");
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("};\n");
    return 0;
}
