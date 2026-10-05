/*
 * Private ARM64EC context metadata for x64 flags without an ARM equivalent.
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

#ifndef __WINE_ARM64EC_GUEST_FLAGS_H
#define __WINE_ARM64EC_GUEST_FLAGS_H

/* These bits belong to the internal context/restore metadata, never CPSR or
 * host PSTATE. Valid zero-valued flags must be distinguishable from a native
 * context with no x64 metadata. The CONTROL group owns their replacement. */
#define ARM64EC_GUEST_FLAGS_VALID 0x02000000u
#define ARM64EC_GUEST_FLAGS_BITS  0x0000e000u
#define ARM64EC_GUEST_FLAGS_MASK  (ARM64EC_GUEST_FLAGS_VALID | ARM64EC_GUEST_FLAGS_BITS)

C_ASSERT( !(ARM64EC_GUEST_FLAGS_MASK &
            (CONTEXT_ARM64_ALL | CONTEXT_ARM64_UNWOUND_TO_CALL |
             CONTEXT_ARM64_RET_TO_GUEST | CONTEXT_EXCEPTION_REPORTING |
             CONTEXT_EXCEPTION_ACTIVE | CONTEXT_SERVICE_ACTIVE | CONTEXT_EXCEPTION_REQUEST)) );

static inline ULONG arm64ec_guest_flags_to_context( ULONG eflags )
{
    return ARM64EC_GUEST_FLAGS_VALID | ((eflags & 0x004) << 11) |
           ((eflags & 0x010) << 10) | ((eflags & 0x400) << 5);
}

static inline ULONG arm64ec_guest_context_flags( ULONG flags )
{
    ULONG required = ARM64EC_GUEST_FLAGS_VALID | CONTEXT_ARM64_RET_TO_GUEST | 1u;

    if ((flags & required) != required) return 0;
    return flags & (ARM64EC_GUEST_FLAGS_MASK | CONTEXT_ARM64_RET_TO_GUEST);
}

static inline ULONG arm64ec_guest_flags_from_context( ULONG flags )
{
    flags = arm64ec_guest_context_flags( flags );
    return ((flags & 0x2000) >> 11) | ((flags & 0x4000) >> 10) |
           ((flags & 0x8000) >> 5);
}

#endif /* __WINE_ARM64EC_GUEST_FLAGS_H */
