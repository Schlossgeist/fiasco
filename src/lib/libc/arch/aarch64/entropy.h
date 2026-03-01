#pragma once

inline
unsigned long long __entropy(void)
{
    unsigned long long rndm;
    asm volatile("mrs %0, CNTPCT_EL0"
                 : "=r" (rndm));
    return rndm;
}
