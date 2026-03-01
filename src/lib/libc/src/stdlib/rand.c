#include <stdlib.h>

#include "entropy.h"

static unsigned rndm = 0; // = 0xF1A5C0;

int rand(void)
{
    if (rndm == 0)
        rndm = __entropy();

    rndm ^= rndm << 13;
    rndm ^= rndm >> 17;
    rndm ^= rndm << 5;

    return rndm >> 1;
}
