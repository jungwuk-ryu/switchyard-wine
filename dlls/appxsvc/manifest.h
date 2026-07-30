/*
 * AppX manifest parser interfaces
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

#ifndef __WINE_APPXSVC_MANIFEST_H
#define __WINE_APPXSVC_MANIFEST_H

#include "windef.h"

#define APPX_MANIFEST_MAX_SIZE                 (4 * 1024 * 1024)
#define APPX_MANIFEST_MAX_APPLICATIONS         100
#define APPX_MANIFEST_MAX_DEPENDENCIES         128
#define APPX_MANIFEST_MAX_TARGET_FAMILIES      32
#define APPX_MANIFEST_MAX_INPROC_CLASSES       1024

typedef struct appx_manifest APPX_MANIFEST;

enum appx_manifest_architecture
{
    APPX_MANIFEST_ARCHITECTURE_NEUTRAL,
    APPX_MANIFEST_ARCHITECTURE_X86,
    APPX_MANIFEST_ARCHITECTURE_X64,
    APPX_MANIFEST_ARCHITECTURE_ARM,
    APPX_MANIFEST_ARCHITECTURE_ARM64,
    APPX_MANIFEST_ARCHITECTURE_X86A64
};

enum appx_manifest_activation_kind
{
    APPX_MANIFEST_ACTIVATION_UNSUPPORTED,
    APPX_MANIFEST_ACTIVATION_FULL_TRUST,
    APPX_MANIFEST_ACTIVATION_PACKAGED_CLASSIC,
    APPX_MANIFEST_ACTIVATION_WIN32
};

enum appx_manifest_threading_model
{
    APPX_MANIFEST_THREADING_BOTH,
    APPX_MANIFEST_THREADING_STA,
    APPX_MANIFEST_THREADING_MTA
};

/*
 * These values are persisted in inspection records.  Append new values rather
 * than renumbering existing ones.
 */
enum appx_manifest_unsupported_reason
{
    APPX_MANIFEST_UNSUPPORTED_UNKNOWN_NAMESPACE = 1,
    APPX_MANIFEST_UNSUPPORTED_IGNORABLE_CONTENT,
    APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION,
    APPX_MANIFEST_UNSUPPORTED_APPCONTAINER,
    APPX_MANIFEST_UNSUPPORTED_RUNTIME_BEHAVIOR,
    APPX_MANIFEST_UNSUPPORTED_OUT_OF_PROCESS_SERVER,
    APPX_MANIFEST_UNSUPPORTED_OPTIONAL_DEPENDENCY,
    APPX_MANIFEST_UNSUPPORTED_APPLICATION_PARAMETERS,
    APPX_MANIFEST_UNSUPPORTED_CURRENT_DIRECTORY,
    APPX_MANIFEST_UNSUPPORTED_RESOURCE_PACKAGE,
    APPX_MANIFEST_UNSUPPORTED_EXTENSION,
    APPX_MANIFEST_UNSUPPORTED_MISSING_RUN_FULL_TRUST,
    APPX_MANIFEST_UNSUPPORTED_TARGET_DEVICE_FAMILY,
    APPX_MANIFEST_UNSUPPORTED_UNSIGNED_DEPENDENCY
};

struct appx_manifest_version
{
    UINT16 major;
    UINT16 minor;
    UINT16 build;
    UINT16 revision;
};

struct appx_manifest_identity
{
    const WCHAR *name;
    const WCHAR *publisher;
    const WCHAR *resource_id;
    const WCHAR *publisher_id;
    const WCHAR *full_name;
    const WCHAR *family_name;
    struct appx_manifest_version version;
    enum appx_manifest_architecture architecture;
};

struct appx_manifest_application
{
    const WCHAR *id;
    const WCHAR *executable;
    const WCHAR *entry_point;
    const WCHAR *runtime_behavior;
    const WCHAR *trust_level;
    const WCHAR *parameters;
    const WCHAR *current_directory_path;
    enum appx_manifest_activation_kind activation_kind;
};

struct appx_manifest_dependency
{
    const WCHAR *name;
    const WCHAR *publisher;
    struct appx_manifest_version min_version;
    UINT16 max_major_version_tested;
    BOOL has_max_major_version_tested;
    BOOL optional;
};

struct appx_manifest_target_family
{
    const WCHAR *name;
    struct appx_manifest_version min_version;
    struct appx_manifest_version max_version_tested;
    BOOL has_max_version_tested;
};

struct appx_manifest_inproc_class
{
    const WCHAR *path;
    const WCHAR *activatable_class_id;
    enum appx_manifest_threading_model threading_model;
};

HRESULT WINAPI appx_manifest_parse( const BYTE *data, SIZE_T size, APPX_MANIFEST **manifest );
void WINAPI appx_manifest_free( APPX_MANIFEST *manifest );

const struct appx_manifest_identity * WINAPI appx_manifest_get_identity(
    const APPX_MANIFEST *manifest );
BOOL WINAPI appx_manifest_is_supported( const APPX_MANIFEST *manifest );
BOOL WINAPI appx_manifest_is_framework( const APPX_MANIFEST *manifest );
BOOL WINAPI appx_manifest_is_resource_package( const APPX_MANIFEST *manifest );
BOOL WINAPI appx_manifest_has_run_full_trust( const APPX_MANIFEST *manifest );

UINT32 WINAPI appx_manifest_get_application_count( const APPX_MANIFEST *manifest );
const struct appx_manifest_application * WINAPI appx_manifest_get_application(
    const APPX_MANIFEST *manifest, UINT32 index );
UINT32 WINAPI appx_manifest_get_dependency_count( const APPX_MANIFEST *manifest );
const struct appx_manifest_dependency * WINAPI appx_manifest_get_dependency(
    const APPX_MANIFEST *manifest, UINT32 index );
UINT32 WINAPI appx_manifest_get_target_family_count( const APPX_MANIFEST *manifest );
const struct appx_manifest_target_family * WINAPI appx_manifest_get_target_family(
    const APPX_MANIFEST *manifest, UINT32 index );
UINT32 WINAPI appx_manifest_get_inproc_class_count( const APPX_MANIFEST *manifest );
const struct appx_manifest_inproc_class * WINAPI appx_manifest_get_inproc_class(
    const APPX_MANIFEST *manifest, UINT32 index );
UINT32 WINAPI appx_manifest_get_unsupported_reason_count( const APPX_MANIFEST *manifest );
enum appx_manifest_unsupported_reason WINAPI appx_manifest_get_unsupported_reason(
    const APPX_MANIFEST *manifest, UINT32 index );

#endif /* __WINE_APPXSVC_MANIFEST_H */
