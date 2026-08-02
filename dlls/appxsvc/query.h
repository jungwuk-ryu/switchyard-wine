/*
 * AppX installed package query helpers
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_APPXSVC_QUERY_H
#define __WINE_APPXSVC_QUERY_H

#include "windef.h"

LONG WINAPI wine_appx_get_package_path_by_full_name(
    const WCHAR *full_name, UINT32 *length, WCHAR *path );
LONG WINAPI wine_appx_get_packages_by_family(
    const WCHAR *family_name, UINT32 *count, WCHAR **full_names,
    UINT32 *buffer_length, WCHAR *buffer );
LONG WINAPI wine_appx_get_package_publisher_by_full_name(
    const WCHAR *full_name, UINT32 *length, WCHAR *publisher );

#endif /* __WINE_APPXSVC_QUERY_H */
