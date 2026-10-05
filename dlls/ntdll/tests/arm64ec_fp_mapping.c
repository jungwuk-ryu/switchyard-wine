/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Pure shared-helper tests: never changes machine FP control/status registers. */
#if 0
#pragma makedep standalone
#endif
#include <stdio.h>
#include <string.h>
#include "../unwind.h"

#if defined(__aarch64__) || defined(__arm64ec__)
_Static_assert( sizeof(UINT) == 4 && sizeof(UINT64) == 8, "FP control widths" );
/* A one-bit oracle independently checks grouping, inversion and ignored bits. */
static const unsigned int status_bits[6] = {0, 7, 1, 2, 3, 4};
static const unsigned int control_bits[10] = {19, 8, 15, 9, 10, 11, 12, 23, 22, 24};

static UINT64 expected_to( UINT value )
{
    UINT fpcr = 0, fpsr = 0;
    unsigned int bit;
    for (bit = 0; bit < 6; ++bit)
        if (value & (1u << bit)) fpsr |= 1u << status_bits[bit];
    for (bit = 6; bit < 16; ++bit)
        if (!!(value & (1u << bit)) ^ (bit >= 7 && bit <= 12))
            fpcr |= 1u << control_bits[bit - 6];
    return fpcr | ((UINT64)fpsr << 32);
}

static UINT expected_from( UINT fpcr, UINT fpsr )
{
    UINT value = 0;
    unsigned int bit;
    for (bit = 0; bit < 6; ++bit)
        if (fpsr & (1u << status_bits[bit])) value |= 1u << bit;
    for (bit = 6; bit < 16; ++bit)
        if (!!(fpcr & (1u << control_bits[bit - 6])) ^ (bit >= 7 && bit <= 12))
            value |= 1u << bit;
    return value;
}

static UINT64 random_word( UINT64 *state )
{
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state;
}

static int check( UINT64 input, UINT64 fpcr, UINT64 fpsr )
{
    if (mxcsr_to_fpcsr( input ) != expected_to( input ) ||
        fpcsr_to_mxcsr( fpcr, fpsr ) != expected_from( fpcr, fpsr ))
    {
        fprintf( stderr, "FP mapping mismatch input=%llx fpcr=%llx fpsr=%llx\n",
                 (unsigned long long)input, (unsigned long long)fpcr, (unsigned long long)fpsr );
        return 0;
    }
    return 1;
}

static int check_context( UINT value, UINT64 packed )
{
    ARM64EC_NT_CONTEXT input = {0}, output;
    struct
    {
        UINT64 before[2];
        ARM64_NT_CONTEXT arm;
        UINT64 after[2];
    } guarded;
    input.ContextFlags = CONTEXT_AMD64_FULL;
    input.AMD64_MxCsr = value;
    input.AMD64_EFlags = 0x616;
    input.Sp = 0x1234567890ULL;
    input.Pc = 0x140001000ULL;
    memset( &guarded, 0xa5, sizeof(guarded) );
    memset( &output, 0x5a, sizeof(output) );
    context_x64_to_arm_guest_return( &guarded.arm, &input );
    context_arm_to_x64( &output, &guarded.arm );
    if (guarded.before[0] != 0xa5a5a5a5a5a5a5a5ULL || guarded.before[1] != 0xa5a5a5a5a5a5a5a5ULL ||
        guarded.after[0] != 0xa5a5a5a5a5a5a5a5ULL || guarded.after[1] != 0xa5a5a5a5a5a5a5a5ULL ||
        output.AMD64_MxCsr != value || output.AMD64_MxCsr_copy != value ||
        output.AMD64_EFlags != input.AMD64_EFlags || output.Sp != input.Sp || output.Pc != input.Pc ||
        ((UINT64)guarded.arm.Fpcr | ((UINT64)guarded.arm.Fpsr << 32)) != packed)
    {
        fprintf( stderr, "FP context mismatch case=%u\n", value );
        return 0;
    }
    return 1;
}
#endif

int main(void)
{
#if defined(__aarch64__) || defined(__arm64ec__)
    unsigned int value, noise, count = 0;
    UINT fpcr_mask = 0, fpsr_mask = 0;
    UINT64 random = 0xbca40d750be9aa76ULL;
    for (value = 0; value < 6; ++value) fpsr_mask |= 1u << status_bits[value];
    for (value = 0; value < 10; ++value) fpcr_mask |= 1u << control_bits[value];
    for (value = 0; value < 65536; ++value)
    {
        UINT64 packed = expected_to( value );
        for (noise = 0; noise < 3; ++noise)
        {
            UINT64 input = value, fpcr = (UINT)packed, fpsr = packed >> 32;
            if (noise)
            {
                input |= random_word( &random ) & ~0xffffULL;
                fpcr |= random_word( &random ) & ~(UINT64)fpcr_mask;
                fpsr |= random_word( &random ) & ~(UINT64)fpsr_mask;
            }
            if (!check( input, fpcr, fpsr )) return 1;
            ++count;
        }
        if (!check_context( value, packed )) return 1;
    }
    for (value = 0; value < 65536; ++value)
    {
        UINT64 input = random_word( &random ), fpcr = random_word( &random ), fpsr = random_word( &random );
        if (!check( input, fpcr, fpsr )) return 1;
        ++count;
    }
    printf( "FP_MAPPING_OK arithmetic=%u context=65536 exhaustive16bits unused/high64bits canaries flags\n", count );
#else
    puts( "FP mapping converters unavailable on this non-ARM64 host" );
#endif
    return 0;
}
