/*
 * Replacement for the one libgcc helper that cannot be linked as-is.
 *
 * The toolchain's prebuilt libgcc.a was compiled with -mcmodel=medlow, and its
 * __clzdi2 reads a lookup table (__clz_tab) through an absolute address that
 * only reaches the low 2 GiB. Our image lives at 0x8000_0000, so that link
 * fails. Defining the function here keeps the archive member from being
 * pulled in. The algorithm is a table-free binary search so that GCC does not
 * turn it back into a call to __clzdi2.
 */

#define BITS_IN_UNSIGNED_LONG 64

int __clzdi2(unsigned long value)
{
    if (value == 0)
    {
        /* Undefined for the builtin; return the bit width as a safe answer. */
        return BITS_IN_UNSIGNED_LONG;
    }

    int leadingZeroCount = 0;
    for (int searchWidth = BITS_IN_UNSIGNED_LONG / 2; searchWidth > 0; searchWidth /= 2)
    {
        unsigned long upperPart = value >> (BITS_IN_UNSIGNED_LONG - searchWidth);
        if (upperPart == 0)
        {
            leadingZeroCount += searchWidth;
            value <<= searchWidth;
        }
    }
    return leadingZeroCount;
}
