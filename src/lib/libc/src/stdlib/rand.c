#include <stdlib.h>

#include "entropy.h"

static int random_int = 0; // = 0xF1A5C0;

int rand(void)
{
    if (random_int == 0)
        random_int = __entropy();

    random_int ^= random_int << 13;
    random_int ^= random_int >> 17;
    random_int ^= random_int << 5;

    if (random_int < 0)
        return -random_int;

    return random_int;
}
