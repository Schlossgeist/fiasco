#pragma once

inline
unsigned long long __entropy(void)
{
    unsigned lo, hi;
    asm volatile("mrrc p15, 0, %0, %1, c14"
                 : "=r"(lo), "=r"(hi));
    return ((unsigned long long) hi) << 32 | lo;
}
