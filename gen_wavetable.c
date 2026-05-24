/* Build-time generator: emit wavetable.h, an 8-row by 256-column
   table of int16 waveform samples that the runtime wavetable
   voice (VOICE_WT) interpolates between. Headroom (peak amplitude
   = ~24576 = int16_max * 0.75) leaves margin so a chord stack of
   wavetable voices does not dominate the mix vs FM voices. */
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N_WT_WAVES   8
#define WT_WAVE_LEN  256
#define WT_PEAK      24576.0   /* int16_max * 0.75 - 15% headroom */

int main(void) {
    printf("#ifndef WAVETABLE_H\n");
    printf("#define WAVETABLE_H\n\n");
    printf("#include <stdint.h>\n\n");
    printf("#define N_WT_WAVES   %d\n",  N_WT_WAVES);
    printf("#define WT_WAVE_LEN  %d\n\n", WT_WAVE_LEN);
    printf("static const int16_t WAVETABLE[N_WT_WAVES][WT_WAVE_LEN] = {\n");
    for (int w = 0; w < N_WT_WAVES; w++) {
        printf("    {");
        for (int i = 0; i < WT_WAVE_LEN; i++) {
            double t = (double)i / (double)WT_WAVE_LEN;
            double y = 0.0;
            switch (w) {
                case 0: /* sine */
                    y = sin(2.0 * M_PI * t);
                    break;
                case 1: /* sine + 3rd harmonic at 0.5 - warm */
                    y = sin(2.0 * M_PI * t) + 0.5 * sin(6.0 * M_PI * t);
                    y /= 1.5;
                    break;
                case 2: /* sine + 3rd + 5th - organ-like */
                    y = sin(2.0 * M_PI * t)
                      + 0.5  * sin(6.0  * M_PI * t)
                      + 0.25 * sin(10.0 * M_PI * t);
                    y /= 1.75;
                    break;
                case 3: /* triangle */
                    y = 4.0 * fabs(t - 0.5) - 1.0;
                    break;
                case 4: /* band-limited saw (8 harmonics) */
                    for (int k = 1; k <= 8; k++)
                        y += sin(2.0 * M_PI * k * t) / k;
                    y *= 2.0 / M_PI;
                    break;
                case 5: /* band-limited square (odd harmonics 1..7) */
                    for (int k = 1; k <= 7; k += 2)
                        y += sin(2.0 * M_PI * k * t) / k;
                    y *= 4.0 / M_PI;
                    break;
                case 6: /* pulse 25% (band-limited, 8 harmonics) */
                    for (int k = 1; k <= 8; k++)
                        y += (2.0 / (M_PI * k))
                           * sin(M_PI * k * 0.25)
                           * cos(2.0 * M_PI * k * (t - 0.125));
                    break;
                case 7: /* bell inharmonic */
                    y = 0.6  * sin(2.0 * M_PI * t)
                      + 0.3  * sin(2.0 * M_PI * 2.76 * t)
                      + 0.1  * sin(2.0 * M_PI * 5.40 * t);
                    break;
            }
            if (y >  1.0) y =  1.0;
            if (y < -1.0) y = -1.0;
            int16_t s = (int16_t)(y * WT_PEAK);
            printf("%6d", s);
            if (i < WT_WAVE_LEN - 1) printf(",");
            if ((i + 1) % 8 == 0) printf("\n     ");
        }
        printf("},\n");
    }
    printf("};\n\n");
    printf("#endif\n");
    return 0;
}
