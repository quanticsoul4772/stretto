#include <stdio.h>
#include <math.h>

#define TABLE_SIZE 1024
#define PEAK_AMPLITUDE 24576

int main(void) {
    printf("static const short sin_table[%d] = {\n", TABLE_SIZE);
    for (int i = 0; i < TABLE_SIZE; i++) {
        double phase = (double)i / TABLE_SIZE * 2.0 * M_PI;
        short sample = (short)(sinf(phase) * PEAK_AMPLITUDE);
        printf("    %6d", sample);
        if (i < TABLE_SIZE - 1) {
            printf(",");
        }
        if ((i + 1) % 8 == 0) {
            printf("\n");
        }
    }
    printf("\n};\n");
    return 0;
}
