/*
 * AppX package architecture policy
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_APPXSVC_ARCHITECTURE_H
#define __WINE_APPXSVC_ARCHITECTURE_H

#include "windef.h"
#include "processthreadsapi.h"

#include "catalog.h"

#define APPX_ARCHITECTURE_POLICY_VERSION 1
#define APPX_ARCHITECTURE_MAX_PE_HEADER_OFFSET (1024u * 1024)
#define APPX_ARCHITECTURE_MAX_OPTIONAL_HEADER_SIZE 4096
#define APPX_ARCHITECTURE_MAX_SECTIONS 96

#define APPX_ARCHITECTURE_MASK(architecture) (1u << (architecture))
#define APPX_ARCHITECTURE_KNOWN_MASK \
    ((1u << (APPX_CATALOG_ARCHITECTURE_X86A64 + 1)) - 1)

struct appx_architecture_policy
{
    UINT32 size;
    UINT32 version;
    UINT32 supported_mask;
    enum appx_catalog_architecture preferred;
};

BOOL appx_architecture_is_valid(
    enum appx_catalog_architecture architecture );
BOOL appx_architecture_is_concrete(
    enum appx_catalog_architecture architecture );
BOOL appx_architecture_is_compatible(
    enum appx_catalog_architecture package_architecture,
    enum appx_catalog_architecture target_architecture );

/*
 * This pure initializer is also the deterministic test seam for the host
 * query.  X86A64 is enabled only when the ARM64 kernel and x86 user mode are
 * enabled.  It denotes x86-on-ARM64 package identity, not ARM64EC.
 */
HRESULT appx_architecture_policy_init(
    enum appx_catalog_architecture preferred,
    MACHINE_ATTRIBUTES i386_attributes,
    MACHINE_ATTRIBUTES amd64_attributes,
    MACHINE_ATTRIBUTES armnt_attributes,
    MACHINE_ATTRIBUTES arm64_attributes,
    struct appx_architecture_policy *policy );
HRESULT appx_architecture_query_host_policy(
    enum appx_catalog_architecture preferred,
    struct appx_architecture_policy *policy );
BOOL appx_architecture_policy_supports(
    const struct appx_architecture_policy *policy,
    enum appx_catalog_architecture architecture );
UINT32 appx_architecture_policy_rank(
    const struct appx_architecture_policy *policy,
    enum appx_catalog_architecture architecture );

/*
 * Read only the bounded DOS and PE headers at explicit offsets.  S_OK returns
 * the raw IMAGE_FILE_MACHINE value, S_FALSE identifies a non-PE or malformed
 * header, and I/O failures are returned as failures.  The caller's file
 * position is restored before return.
 */
HRESULT appx_architecture_read_pe_machine( HANDLE file, USHORT *machine );

/*
 * CHPE_X86 maps to the X86A64 package domain.  ARM64EC is deliberately
 * different: its dependencies use the X64 domain.  ARM64X and unknown machine
 * types are unsupported and return WRONG_PROCESSOR_ARCHITECTURE.
 */
HRESULT appx_architecture_map_machine(
    USHORT machine, enum appx_catalog_architecture *architecture );

/*
 * Validate the executable on its already-held handle.  A neutral manifest may
 * use any host-supported executable target; a concrete manifest must match the
 * effective dependency domain exactly.
 */
HRESULT appx_architecture_validate_executable(
    HANDLE file, enum appx_catalog_architecture manifest_architecture,
    const struct appx_architecture_policy *policy,
    enum appx_catalog_architecture *target_architecture );

#endif /* __WINE_APPXSVC_ARCHITECTURE_H */
