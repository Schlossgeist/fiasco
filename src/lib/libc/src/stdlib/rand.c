#include <stdlib.h>

static int random_int = 0; // = 0xF1A5C0;

int rand(void)
{
    if (random_int == 0) {
        unsigned lo, hi;
        asm volatile("mrrc p15, 0, %0, %1, c14"
                     : "=r"(lo), "=r"(hi));
        random_int = lo;
    }

    random_int ^= random_int << 13;
    random_int ^= random_int >> 17;
    random_int ^= random_int << 5;
    return random_int;
}
