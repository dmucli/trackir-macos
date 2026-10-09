#include "common/np_shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t seed = 1;
static uint32_t lcg(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }
int main(int argc, char **argv)
{
    int cases = argc > 1 ? atoi(argv[1]) : 20;
    for (int c = 0; c < cases; c++) {
        np_trackir_data d;
        memset(&d, 0, sizeof d);
        d.status = 0;
        d.frame_signature = (uint16_t)(lcg() & 0x7fff);
        float *f = &d.roll;
        for (int i = 0; i < 6; i++)
            f[i] = (float)((int)(lcg() % 32767) - 16383) * 0.5f;
        uint8_t key[8];
        for (int i = 0; i < 8; i++) key[i] = (uint8_t)lcg();
        np_trackir_data plain = d;
        np_data_encrypt(&d, key, lcg);
        const uint8_t *p = (const uint8_t *)&plain, *e = (const uint8_t *)&d;
        for (int i = 0; i < 8; i++) printf("%02x", key[i]);
        printf(" ");
        for (int i = 0; i < 68; i++) printf("%02x", p[i]);
        printf(" ");
        for (int i = 0; i < 68; i++) printf("%02x", e[i]);
        printf("\n");
    }
    return 0;
}
