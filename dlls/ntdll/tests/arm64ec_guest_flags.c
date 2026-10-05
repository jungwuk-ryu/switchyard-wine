/*
 * Verify full x64 flags across the actual ARM64EC context converters.
 *
 * Copyright 2026 Switchyard Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <string.h>
#include "../unwind.h"

C_ASSERT( sizeof(ARM64_NT_CONTEXT) == 0x390 );
C_ASSERT( sizeof(ARM64EC_NT_CONTEXT) == 0x4d0 );

int main(void)
{
    static const UINT bits[] = {1, 4, 16, 64, 128, 256, 1024, 2048};
    unsigned int combination, bit, count = 0;

    for (combination = 0; combination < 256; ++combination)
    {
        ULONG eflags = 0x202, metadata;

        for (bit = 0; bit < sizeof(bits) / sizeof(bits[0]); ++bit)
            if (combination & (1u << bit)) eflags |= bits[bit];
        metadata = arm64ec_guest_flags_to_context( eflags ) |
                   CONTEXT_ARM64_CONTROL | CONTEXT_ARM64_RET_TO_GUEST;
        if (arm64ec_guest_flags_from_context( metadata ) != (eflags & 0x414) ||
            arm64ec_guest_flags_from_context( metadata & ~ARM64EC_GUEST_FLAGS_VALID ) ||
            arm64ec_guest_flags_from_context( metadata & ~CONTEXT_ARM64_RET_TO_GUEST ) ||
            arm64ec_guest_flags_from_context( metadata & ~1u ))
            return 1;
    }

#if defined(__aarch64__) || defined(__arm64ec__)
    {
        static const ULONG groups[] = {CONTEXT_AMD64_CONTROL, CONTEXT_AMD64_FULL};
        unsigned int group;

        for (group = 0; group < sizeof(groups) / sizeof(groups[0]); ++group)
        {
            for (combination = 0; combination < 256; ++combination)
            {
                ARM64EC_NT_CONTEXT input = {0}, output;
                struct
                {
                    UINT64 before[2];
                    ARM64_NT_CONTEXT context;
                    UINT64 after[2];
                } arm = {{0x123456789abcdef0, 0xfedcba9876543210}, {0},
                         {0x3141592653589793, 0x2718281828459045}};

                input.ContextFlags = groups[group];
                input.AMD64_EFlags = 0x202;
                for (bit = 0; bit < sizeof(bits) / sizeof(bits[0]); ++bit)
                    if (combination & (1u << bit)) input.AMD64_EFlags |= bits[bit];
                input.X8 = 0x5a123456789abcde;
                input.Sp = 0x1234567890;
                input.Pc = 0x140001000;
                context_x64_to_arm_guest_return( &arm.context, &input );
                context_arm_to_x64( &output, &arm.context );
                if (output.AMD64_EFlags != input.AMD64_EFlags ||
                    output.ContextFlags != input.ContextFlags ||
                    output.X8 != input.X8 ||
                    output.Sp != input.Sp || output.Pc != input.Pc ||
                    arm.context.Cpsr != eflags_to_cpsr( input.AMD64_EFlags ) ||
                    arm.before[0] != 0x123456789abcdef0 ||
                    arm.before[1] != 0xfedcba9876543210 ||
                    arm.after[0] != 0x3141592653589793 ||
                    arm.after[1] != 0x2718281828459045)
                {
                    fprintf( stderr, "ARM64EC flags group=%u case=%u input=%#x output=%#x\n",
                             group, combination, input.AMD64_EFlags, output.AMD64_EFlags );
                    return 1;
                }
                ++count;
            }
        }
        {
            ARM64EC_NT_CONTEXT input = {0}, output;
            ARM64_NT_CONTEXT arm;

            input.ContextFlags = CONTEXT_AMD64_INTEGER;
            input.AMD64_EFlags = 0x616;
            context_x64_to_arm_guest_return( &arm, &input );
            if (arm.ContextFlags & ARM64EC_GUEST_FLAGS_MASK) return 1;
            input.ContextFlags = CONTEXT_AMD64_FULL;
            context_x64_to_arm( &arm, &input );
            if (arm.ContextFlags & ARM64EC_GUEST_FLAGS_MASK) return 1;
            context_arm_to_x64( &output, &arm );
            if (output.AMD64_EFlags != cpsr_to_eflags( arm.Cpsr )) return 1;
        }
    }
#endif

    printf( "ARM64EC_GUEST_FLAGS_OK cases=%u\n", count );
    return 0;
}
