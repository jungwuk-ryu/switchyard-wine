/*
 * AppX bundle manifest parser interfaces
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

#ifndef __WINE_APPXSVC_BUNDLE_MANIFEST_H
#define __WINE_APPXSVC_BUNDLE_MANIFEST_H

#include "windef.h"

#define APPX_BUNDLE_MANIFEST_MAX_SIZE       (4 * 1024 * 1024)
/*
 * BundleManifestSchema2013.xsd permits 10,000 packages.  This implementation
 * deliberately caps hostile manifests at 4,096 packages and 65,536 resources;
 * the schema separately caps each Resources element at 200 Resource children.
 */
#define APPX_BUNDLE_MANIFEST_MAX_PACKAGES   4096
#define APPX_BUNDLE_MANIFEST_MAX_RESOURCES  65536

typedef struct appx_bundle_manifest APPX_BUNDLE_MANIFEST;

struct appx_bundle_version
{
    UINT16 major;
    UINT16 minor;
    UINT16 build;
    UINT16 revision;
};

enum appx_bundle_package_type
{
    APPX_BUNDLE_PACKAGE_APPLICATION,
    APPX_BUNDLE_PACKAGE_FRAMEWORK,
    APPX_BUNDLE_PACKAGE_RESOURCE,
    APPX_BUNDLE_PACKAGE_OPTIONAL,
    APPX_BUNDLE_PACKAGE_UNSUPPORTED
};

enum appx_bundle_architecture
{
    APPX_BUNDLE_ARCHITECTURE_NEUTRAL,
    APPX_BUNDLE_ARCHITECTURE_X86,
    APPX_BUNDLE_ARCHITECTURE_X64,
    APPX_BUNDLE_ARCHITECTURE_ARM,
    APPX_BUNDLE_ARCHITECTURE_ARM64,
    APPX_BUNDLE_ARCHITECTURE_X86A64,
    APPX_BUNDLE_ARCHITECTURE_UNSUPPORTED
};

enum appx_bundle_package_flags
{
    APPX_BUNDLE_PACKAGE_HAS_RANGE                = 0x00000001,
    APPX_BUNDLE_PACKAGE_ENCRYPTED                = 0x00000002,
    APPX_BUNDLE_PACKAGE_UNSUPPORTED_TYPE         = 0x00000004,
    APPX_BUNDLE_PACKAGE_UNSUPPORTED_ARCHITECTURE = 0x00000008,
    APPX_BUNDLE_PACKAGE_UNSUPPORTED_EXTENSION    = 0x00000010,
    APPX_BUNDLE_PACKAGE_UNSUPPORTED_QUALIFIER    = 0x00000020
};

enum appx_bundle_selection_issues
{
    APPX_BUNDLE_SELECTION_RESOURCE_PAYLOAD          = 0x00000001,
    APPX_BUNDLE_SELECTION_MATCHING_RESOURCE_PAYLOAD = 0x00000002,
    APPX_BUNDLE_SELECTION_OPTIONAL_PAYLOAD          = 0x00000004,
    APPX_BUNDLE_SELECTION_ENCRYPTED_PAYLOAD         = 0x00000008,
    APPX_BUNDLE_SELECTION_UNSUPPORTED_TYPE          = 0x00000010,
    APPX_BUNDLE_SELECTION_UNSUPPORTED_ARCHITECTURE  = 0x00000020,
    APPX_BUNDLE_SELECTION_UNSUPPORTED_EXTENSION     = 0x00000040,
    APPX_BUNDLE_SELECTION_INCOMPATIBLE_ARCHITECTURE = 0x00000080,
    APPX_BUNDLE_SELECTION_NO_PAYLOAD                = 0x00000100,
    APPX_BUNDLE_SELECTION_AMBIGUOUS_PAYLOAD         = 0x00000200,
    APPX_BUNDLE_SELECTION_UNSUPPORTED_QUALIFIER     = 0x00000400
};

struct appx_bundle_identity
{
    const WCHAR *name;
    const WCHAR *publisher;
    struct appx_bundle_version version;
};

struct appx_bundle_resource
{
    const WCHAR *language;
    const WCHAR *dx_feature_level;
    UINT32 scale;
    BOOL has_scale;
};

struct appx_bundle_package
{
    /* The 2013 schema defaults absent Type and Architecture attributes. */
    enum appx_bundle_package_type type;
    enum appx_bundle_architecture architecture;
    struct appx_bundle_version version;
    const WCHAR *type_name;
    const WCHAR *architecture_name;
    const WCHAR *resource_id;
    const WCHAR *file_name;
    const struct appx_bundle_resource *resources;
    UINT32 resource_count;
    UINT32 flags;
    UINT64 offset;
    UINT64 size;
};

/*
 * A FALSE neutral-only field requests exact matching for that qualifier.
 * Language must then be a nonempty BCP-47-style tag, and scale must be in
 * the range 1..1000.  Neutral resource declarations remain applicable.
 *
 * Payload selection deliberately does not emulate another processor
 * architecture.  It considers only a package matching host_architecture,
 * then a neutral package.  Within one architecture rank it chooses the
 * highest version and reports an equal-version tie as ambiguous.  The caller
 * may make a separately audited emulation decision before choosing the host
 * architecture passed here.
 */
struct appx_bundle_selection_policy
{
    UINT32 size;
    enum appx_bundle_architecture host_architecture;
    const WCHAR *language;
    UINT32 scale;
    BOOL language_neutral_only;
    BOOL scale_neutral_only;
};

struct appx_bundle_selection
{
    /* Set to sizeof(struct appx_bundle_selection) before calling select. */
    UINT32 size;
    const struct appx_bundle_package *payload;
    UINT32 package_index;
    UINT32 issues;
    UINT32 matching_resource_count;
};

HRESULT WINAPI appx_bundle_manifest_parse( const BYTE *data, SIZE_T size,
                                           APPX_BUNDLE_MANIFEST **manifest );
void WINAPI appx_bundle_manifest_free( APPX_BUNDLE_MANIFEST *manifest );

const struct appx_bundle_identity * WINAPI appx_bundle_manifest_get_identity(
    const APPX_BUNDLE_MANIFEST *manifest );
UINT32 WINAPI appx_bundle_manifest_get_package_count(
    const APPX_BUNDLE_MANIFEST *manifest );
const struct appx_bundle_package * WINAPI appx_bundle_manifest_get_package(
    const APPX_BUNDLE_MANIFEST *manifest, UINT32 index );
HRESULT WINAPI appx_bundle_manifest_select(
    const APPX_BUNDLE_MANIFEST *manifest,
    const struct appx_bundle_selection_policy *policy,
    struct appx_bundle_selection *selection );

#endif /* __WINE_APPXSVC_BUNDLE_MANIFEST_H */
