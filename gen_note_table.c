#include <stdio.h>
#include <math.h>

#define SAMPLE_RATE 48000
#define KS_MAX_LEN  512

int main(void) {
    printf("static const unsigned int note_phase_inc[128] = {\n");
    for (int n = 0; n < 128; n++) {
        double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        unsigned int phase_inc = (unsigned int)((freq * 4294967296.0) / SAMPLE_RATE);
        printf("    %10uu", phase_inc);
        if (n < 127) printf(",");
        if ((n + 1) % 4 == 0) printf("\n");
    }
    printf("};\n\n");

    printf("static const unsigned short note_ks_len[128] = {\n");
    for (int n = 0; n < 128; n++) {
        double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        int ks_len = (int)((double)SAMPLE_RATE / freq + 0.5);
        if (ks_len > KS_MAX_LEN) ks_len = KS_MAX_LEN;
        if (ks_len < 4) ks_len = 4;
        printf("    %4d", ks_len);
        if (n < 127) printf(",");
        if ((n + 1) % 16 == 0) printf("\n");
    }
    printf("};\n");
    return 0;
}
