/*
 * Wine AppX deployment service interfaces
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

#ifndef __WINE_APPXSVC_H
#define __WINE_APPXSVC_H

#include "windef.h"

#define WINE_APPX_PATH_DIRECTORY       0x00000001
#define WINE_APPX_MAX_ENTRY_NAME_BYTES 0x00100000
#define WINE_APPX_MAX_PATH_CHARS       32767
#define WINE_APPX_MAX_COMPONENT_CHARS  255

HRESULT WINAPI wine_appx_validate_archive_path( const BYTE *utf8, UINT32 utf8_length, UINT32 flags,
                                                UINT32 *path_length, WCHAR *path );

#endif /* __WINE_APPXSVC_H */
