/*
 * WoW64 GDI functions
 *
 * Copyright 2021 Jacek Caban for CodeWeavers
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

#include <stdarg.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "ddk/d3dkmthk.h"
#include "../win32u/d3dkmt_private.h"
#include "wow64win_private.h"

typedef struct
{
    INT   bmType;
    INT   bmWidth;
    INT   bmHeight;
    INT   bmWidthBytes;
    WORD  bmPlanes;
    WORD  bmBitsPixel;
    ULONG bmBits;
} BITMAP32;

typedef struct
{
    UINT          otmSize;
    TEXTMETRICW   otmTextMetrics;
    BYTE          otmFiller;
    PANOSE        otmPanoseNumber;
    UINT          otmfsSelection;
    UINT          otmfsType;
    INT           otmsCharSlopeRise;
    INT           otmsCharSlopeRun;
    INT           otmItalicAngle;
    UINT          otmEMSquare;
    INT           otmAscent;
    INT           otmDescent;
    UINT          otmLineGap;
    UINT          otmsCapEmHeight;
    UINT          otmsXHeight;
    RECT          otmrcFontBox;
    INT           otmMacAscent;
    INT           otmMacDescent;
    UINT          otmMacLineGap;
    UINT          otmusMinimumPPEM;
    POINT         otmptSubscriptSize;
    POINT         otmptSubscriptOffset;
    POINT         otmptSuperscriptSize;
    POINT         otmptSuperscriptOffset;
    UINT          otmsStrikeoutSize;
    INT           otmsStrikeoutPosition;
    INT           otmsUnderscoreSize;
    INT           otmsUnderscorePosition;
    ULONG         otmpFamilyName;
    ULONG         otmpFaceName;
    ULONG         otmpStyleName;
    ULONG         otmpFullName;
} OUTLINETEXTMETRIC32;

C_ASSERT( FIELD_OFFSET( OUTLINETEXTMETRIC32, otmpFamilyName ) ==
          FIELD_OFFSET( OUTLINETEXTMETRICW, otmpFamilyName ) );
C_ASSERT( sizeof(OUTLINETEXTMETRICW) - sizeof(OUTLINETEXTMETRIC32) ==
          4 * sizeof(ULONG) );
C_ASSERT( FIELD_OFFSET( EXTLOGPEN, elpStyleEntry ) ==
          FIELD_OFFSET( EXTLOGPEN32, elpStyleEntry ) + sizeof(ULONG) );

typedef struct
{
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hResource;
    D3DKMT_HANDLE hGlobalShare;
    ULONG pPrivateRuntimeData;
    UINT PrivateRuntimeDataSize;
    union
    {
        ULONG pStandardAllocation;
        ULONG pPrivateDriverData;
    };
    UINT PrivateDriverDataSize;
    UINT NumAllocations;
    union
    {
        ULONG pAllocationInfo;
        ULONG pAllocationInfo2;
    };
    D3DKMT_CREATEALLOCATIONFLAGS Flags;
    ULONG hPrivateRuntimeResourceHandle;
} D3DKMT_CREATEALLOCATION32;

C_ASSERT( sizeof(D3DKMT_CREATEALLOCATION32) == 44 );

typedef struct
{
    D3DKMT_HANDLE hAdapter;
    D3DKMT_CREATEDEVICEFLAGS Flags;
    D3DKMT_HANDLE hDevice;
    ULONG pCommandBuffer;
    UINT CommandBufferSize;
    ULONG pAllocationList;
    UINT AllocationListSize;
    ULONG pPatchLocationList;
    UINT PatchLocationListSize;
} D3DKMT_CREATEDEVICE32;

C_ASSERT( sizeof(D3DKMT_CREATEDEVICE32) == 36 );
C_ASSERT( offsetof(D3DKMT_CREATEDEVICE32, hDevice) == 8 );
C_ASSERT( offsetof(D3DKMT_CREATEDEVICE32, pCommandBuffer) == 12 );


static DWORD gdi_handle_type( HGDIOBJ obj )
{
    unsigned int handle = HandleToUlong( obj );
    return handle & NTGDI_HANDLE_TYPE_MASK;
}

NTSTATUS WINAPI wow64_NtGdiAbortDoc( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiAbortDoc( hdc );
}

NTSTATUS WINAPI wow64_NtGdiAbortPath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiAbortPath( hdc );
}

NTSTATUS WINAPI wow64_NtGdiAddFontMemResourceEx( UINT *args )
{
    void *ptr = get_memory_ptr( &args );
    DWORD size = get_ulong( &args );
    void *dv = get_memory_ptr( &args );
    ULONG dv_size = get_ulong( &args );
    DWORD *count = get_memory_ptr( &args );

    return HandleToUlong( NtGdiAddFontMemResourceEx( ptr, size, dv, dv_size, count ));
}

NTSTATUS WINAPI wow64_NtGdiAddFontResourceW( UINT *args )
{
    const WCHAR *str = get_memory_ptr( &args );
    ULONG size = get_ulong( &args );
    ULONG files = get_ulong( &args );
    DWORD flags = get_ulong( &args );
    DWORD tid = get_ulong( &args );
    void *dv = get_memory_ptr( &args );

    return NtGdiAddFontResourceW( str, size, files, flags, tid, dv );
}

NTSTATUS WINAPI wow64_NtGdiAlphaBlend( UINT *args )
{
    HDC hdc_dst = get_handle( &args );
    int x_dst = get_ulong( &args );
    int y_dst = get_ulong( &args );
    int width_dst = get_ulong( &args );
    int height_dst = get_ulong( &args );
    HDC hdc_src = get_handle( &args );
    int x_src = get_ulong( &args );
    int y_src = get_ulong( &args );
    int width_src = get_ulong( &args );
    int height_src = get_ulong( &args );
    DWORD blend_function = get_ulong( &args );
    HANDLE xform = get_handle( &args );

    return NtGdiAlphaBlend( hdc_dst, x_dst, y_dst, width_dst, height_dst, hdc_src,
                            x_src, y_src, width_src, height_src, blend_function, xform );
}

NTSTATUS WINAPI wow64_NtGdiAngleArc( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );
    DWORD radius = get_ulong( &args );
    DWORD start_angle = get_ulong( &args );
    DWORD sweep_angle = get_ulong( &args );

    return NtGdiAngleArc( hdc, x, y, radius, start_angle, sweep_angle );
}

NTSTATUS WINAPI wow64_NtGdiArcInternal( UINT *args )
{
    UINT type = get_ulong( &args );
    HDC hdc = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );
    INT xstart = get_ulong( &args );
    INT ystart = get_ulong( &args );
    INT xend = get_ulong( &args );
    INT yend = get_ulong( &args );

    return NtGdiArcInternal( type, hdc, left, top, right, bottom, xstart, ystart, xend, yend );
}

NTSTATUS WINAPI wow64_NtGdiBeginPath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiBeginPath( hdc );
}

NTSTATUS WINAPI wow64_NtGdiBitBlt( UINT *args )
{
    HDC hdc_dst = get_handle( &args );
    INT x_dst = get_ulong( &args );
    INT y_dst = get_ulong( &args );
    INT width = get_ulong( &args );
    INT height = get_ulong( &args );
    HDC hdc_src = get_handle( &args );
    INT x_src = get_ulong( &args );
    INT y_src = get_ulong( &args );
    DWORD rop = get_ulong( &args );
    DWORD bk_color = get_ulong( &args );
    FLONG fl = get_ulong( &args );

    return NtGdiBitBlt( hdc_dst, x_dst, y_dst, width, height, hdc_src,
                        x_src, y_src, rop, bk_color, fl );
}

NTSTATUS WINAPI wow64_NtGdiCancelDC( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiCancelDC( hdc );
}

NTSTATUS WINAPI wow64_NtGdiCloseFigure( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiCloseFigure( hdc );
}

NTSTATUS WINAPI wow64_NtGdiCombineRgn( UINT *args )
{
    HRGN dest = get_handle( &args );
    HRGN src1 = get_handle( &args );
    HRGN src2 = get_handle( &args );
    INT mode = get_ulong( &args );

    return NtGdiCombineRgn( dest, src1, src2, mode );
}

NTSTATUS WINAPI wow64_NtGdiComputeXformCoefficients( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiComputeXformCoefficients( hdc );
}

NTSTATUS WINAPI wow64_NtGdiCreateBitmap( UINT *args )
{
    INT width = get_ulong( &args );
    INT height = get_ulong( &args );
    UINT planes = get_ulong( &args );
    UINT bpp = get_ulong( &args );
    const void *bits = get_memory_ptr( &args );

    return HandleToUlong( NtGdiCreateBitmap( width, height, planes, bpp, bits ));
}

NTSTATUS WINAPI wow64_NtGdiCreateClientObj( UINT *args )
{
    ULONG type = get_ulong( &args );

    return HandleToUlong( NtGdiCreateClientObj( type ));
}

NTSTATUS WINAPI wow64_NtGdiCreateCompatibleBitmap( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT width = get_ulong( &args );
    INT height = get_ulong( &args );

    return HandleToUlong( NtGdiCreateCompatibleBitmap( hdc, width, height ));
}

NTSTATUS WINAPI wow64_NtGdiCreateCompatibleDC( UINT *args )
{
    HDC hdc = get_handle( &args );

    return HandleToUlong( NtGdiCreateCompatibleDC( hdc ));
}

NTSTATUS WINAPI wow64_NtGdiCreateDIBBrush( UINT *args )
{
    const void *data = get_memory_ptr( &args );
    UINT coloruse = get_ulong( &args );
    UINT size = get_ulong( &args );
    BOOL is_8x8 = get_ulong( &args );
    BOOL pen = get_ulong( &args );
    const void *client = get_client_ptr( &args );

    return HandleToUlong( NtGdiCreateDIBBrush( data, coloruse, size, is_8x8, pen, client ));
}

NTSTATUS WINAPI wow64_NtGdiCreateDIBSection( UINT *args )
{
    HDC hdc = get_handle( &args );
    HANDLE section = get_handle( &args );
    DWORD offset = get_ulong( &args );
    const BITMAPINFO *bmi = get_memory_ptr( &args );
    UINT usage = get_ulong( &args );
    UINT header_size = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    ULONG_PTR color_space = get_ulong( &args );
    void *bits32 = get_memory_ptr( &args );

    void *bits;
    HBITMAP ret;

    ret = NtGdiCreateDIBSection( hdc, section, offset, bmi, usage, header_size,
                                 flags, color_space, addr_32to64( &bits, bits32 ));
    put_addr( bits32, bits );
    return HandleToUlong( ret );
}

NTSTATUS WINAPI wow64_NtGdiCreateDIBitmapInternal( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT width = get_ulong( &args );
    INT height = get_ulong( &args );
    DWORD init = get_ulong( &args );
    const void *bits = get_memory_ptr( &args );
    const BITMAPINFO *data = get_memory_ptr( &args );
    UINT coloruse = get_ulong( &args );
    UINT max_info = get_ulong( &args );
    UINT max_bits = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    HANDLE xform = get_handle( &args );

    HBITMAP ret = NtGdiCreateDIBitmapInternal( hdc, width, height, init, bits, data,
                                               coloruse, max_info, max_bits, flags, xform );
    return HandleToUlong( ret );
}

NTSTATUS WINAPI wow64_NtGdiCreateEllipticRgn( UINT *args )
{
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );

    return HandleToUlong( NtGdiCreateEllipticRgn( left, top, right, bottom ));
}

NTSTATUS WINAPI wow64_NtGdiCreateHalftonePalette( UINT *args )
{
    HDC hdc = get_handle( &args );

    return HandleToUlong( NtGdiCreateHalftonePalette( hdc ));
}

NTSTATUS WINAPI wow64_NtGdiCreateHatchBrushInternal( UINT *args )
{
    INT style = get_ulong( &args );
    COLORREF color = get_ulong( &args );
    BOOL pen = get_ulong( &args );

    return HandleToULong( NtGdiCreateHatchBrushInternal( style, color, pen ));
}

NTSTATUS WINAPI wow64_NtGdiCreateMetafileDC( UINT *args )
{
    HDC hdc = get_handle( &args );

    return HandleToUlong( NtGdiCreateMetafileDC( hdc ));
}

NTSTATUS WINAPI wow64_NtGdiCreatePaletteInternal( UINT *args )
{
    const LOGPALETTE *palette = get_memory_ptr( &args );
    UINT count = get_ulong( &args );

    return HandleToUlong( NtGdiCreatePaletteInternal( palette, count ));
}

NTSTATUS WINAPI wow64_NtGdiCreatePatternBrushInternal( UINT *args )
{
    HBITMAP hbitmap = get_handle( &args );
    BOOL pen = get_ulong( &args );
    BOOL is_8x8 = get_ulong( &args );

    return HandleToUlong( NtGdiCreatePatternBrushInternal( hbitmap, pen, is_8x8 ));
}

NTSTATUS WINAPI wow64_NtGdiCreatePen( UINT *args )
{
    INT style = get_ulong( &args );
    INT width = get_ulong( &args );
    COLORREF color = get_ulong( &args );
    HBRUSH brush = get_handle( &args );

    return HandleToUlong( NtGdiCreatePen( style, width, color, brush ));
}

NTSTATUS WINAPI wow64_NtGdiCreateRectRgn( UINT *args )
{
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );

    return HandleToUlong( NtGdiCreateRectRgn( left, top, right, bottom ));
}

NTSTATUS WINAPI wow64_NtGdiCreateRoundRectRgn( UINT *args )
{
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );
    INT ellipse_width = get_ulong( &args );
    INT ellipse_height = get_ulong( &args );

    return HandleToUlong( NtGdiCreateRoundRectRgn( left, top, right, bottom,
                                                   ellipse_width, ellipse_height ));
}

NTSTATUS WINAPI wow64_NtGdiCreateSolidBrush( UINT *args )
{
    COLORREF color = get_ulong( &args );
    HBRUSH brush = get_handle( &args );

    return HandleToUlong( NtGdiCreateSolidBrush( color, brush ));
}

NTSTATUS WINAPI wow64_NtGdiDdDDIAcquireKeyedMutex( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hKeyedMutex;
        UINT64 Key;
        ULONG pTimeout;
        UINT64 FenceValue;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_ACQUIREKEYEDMUTEX desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hKeyedMutex = desc32->hKeyedMutex;
    desc.Key = desc32->Key;
    desc.pTimeout = UlongToHandle( desc32->pTimeout );
    desc.FenceValue = desc32->FenceValue;
    status = NtGdiDdDDIAcquireKeyedMutex( &desc );
    desc32->FenceValue = desc.FenceValue;

    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIAcquireKeyedMutex2( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hKeyedMutex;
        UINT64 Key;
        ULONG pTimeout;
        UINT64 FenceValue;
        ULONG pPrivateRuntimeData;
        UINT PrivateRuntimeDataSize;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_ACQUIREKEYEDMUTEX2 desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hKeyedMutex = desc32->hKeyedMutex;
    desc.Key = desc32->Key;
    desc.pTimeout = UlongToHandle( desc32->pTimeout );
    desc.FenceValue = desc32->FenceValue;
    desc.pPrivateRuntimeData = UlongToHandle( desc32->pPrivateRuntimeData );
    desc.PrivateRuntimeDataSize = desc32->PrivateRuntimeDataSize;
    status = NtGdiDdDDIAcquireKeyedMutex2( &desc );
    desc32->FenceValue = desc.FenceValue;

    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDICheckOcclusion( UINT *args )
{
    struct
    {
        ULONG hWnd;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_CHECKOCCLUSION desc = {.hWnd = UlongToHandle( desc32->hWnd )};
    return NtGdiDdDDICheckOcclusion( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDICheckVidPnExclusiveOwnership( UINT *args )
{
    const D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP *desc = get_memory_ptr( &args );

    return NtGdiDdDDICheckVidPnExclusiveOwnership( desc );
}

static NTSTATUS wow64_d3dkmt_lifecycle(
    void *desc32, enum wine_d3dkmt_lifecycle32_variant variant )
{
    struct wine_d3dkmt_lifecycle32 params =
    {
        .version = WINE_D3DKMT_LIFECYCLE32_VERSION,
        .size = sizeof(params),
        .variant = variant,
    };
    NTSTATUS status;

    switch (variant)
    {
    case WINE_D3DKMT_LIFECYCLE32_OPEN_ADAPTER_FROM_LUID:
    {
        D3DKMT_OPENADAPTERFROMLUID local;

        wow64win_read_user( &local, desc32, sizeof(local) );
        params.luid_low = local.AdapterLuid.LowPart;
        params.luid_high = local.AdapterLuid.HighPart;
        break;
    }
    case WINE_D3DKMT_LIFECYCLE32_CLOSE_ADAPTER:
    case WINE_D3DKMT_LIFECYCLE32_DESTROY_DEVICE:
        if (!desc32) return STATUS_INVALID_PARAMETER;
        wow64win_read_user( &params.input_handle, desc32,
                            sizeof(params.input_handle) );
        break;
    case WINE_D3DKMT_LIFECYCLE32_CREATE_DEVICE:
    {
        D3DKMT_CREATEDEVICE32 local;

        if (!desc32) return STATUS_INVALID_PARAMETER;
        wow64win_read_user( &local, desc32, sizeof(local) );
        params.input_handle = local.hAdapter;
        memcpy( &params.flags, &local.Flags, sizeof(params.flags) );
        break;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }

    params.guest_desc = wow64win_guest_memory_addr( desc32 );
    status = __wine_win32u_d3dkmt_lifecycle( &params );
    if (params.fault_status) RtlRaiseStatus( params.fault_status );
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDICloseAdapter( UINT *args )
{
    D3DKMT_CLOSEADAPTER *desc32 = get_memory_ptr( &args );

    return wow64_d3dkmt_lifecycle( desc32,
                                   WINE_D3DKMT_LIFECYCLE32_CLOSE_ADAPTER );
}

static NTSTATUS wow64_create_allocation( D3DKMT_CREATEALLOCATION32 *desc32,
                                           BOOL version2 )
{
    struct wine_d3dkmt_create_allocation32 params =
    {
        .version = WINE_D3DKMT_CREATE_ALLOCATION32_VERSION,
        .size = sizeof(params),
        .variant = version2 ? WINE_D3DKMT_CREATE_ALLOCATION32_V2 :
                              WINE_D3DKMT_CREATE_ALLOCATION32_V1,
    };
    D3DKMT_CREATEALLOCATION32 local;
    NTSTATUS status;

    wow64win_read_user( &local, desc32, sizeof(local) );

    params.guest_desc = wow64win_guest_memory_addr( desc32 );
    params.guest_standard_or_private = local.pStandardAllocation;
    params.guest_allocation = local.pAllocationInfo;
    params.guest_runtime = local.pPrivateRuntimeData;
    params.hDevice = local.hDevice;
    params.hResource = local.hResource;
    params.hGlobalShare = local.hGlobalShare;
    params.private_runtime_data_size = local.PrivateRuntimeDataSize;
    params.private_driver_data_size = local.PrivateDriverDataSize;
    params.num_allocations = local.NumAllocations;
    memcpy( &params.flags, &local.Flags, sizeof(params.flags) );
    params.private_runtime_resource_handle = local.hPrivateRuntimeResourceHandle;

    status = __wine_win32u_d3dkmt_create_allocation( &params );
    if (params.fault_status) RtlRaiseStatus( params.fault_status );
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDICreateAllocation( UINT *args )
{
    D3DKMT_CREATEALLOCATION32 *desc32 = get_memory_ptr( &args );

    return wow64_create_allocation( desc32, FALSE );
}

NTSTATUS WINAPI wow64_NtGdiDdDDICreateAllocation2( UINT *args )
{
    D3DKMT_CREATEALLOCATION32 *desc32 = get_memory_ptr( &args );

    return wow64_create_allocation( desc32, TRUE );
}
NTSTATUS WINAPI wow64_NtGdiDdDDICreateDCFromMemory( UINT *args )
{
    struct _D3DKMT_CREATEDCFROMMEMORY
    {
        ULONG pMemory;
        D3DDDIFORMAT Format;
        UINT Width;
        UINT Height;
        UINT Pitch;
        ULONG hDeviceDc;
        ULONG pColorTable;
        ULONG hDc;
        ULONG hBitmap;
    } *desc32 = get_memory_ptr( &args );

    D3DKMT_CREATEDCFROMMEMORY desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.pMemory = wow64win_guest_memory_ptr( desc32->pMemory );
    desc.Format = desc32->Format;
    desc.Width = desc32->Width;
    desc.Height = desc32->Height;
    desc.Pitch = desc32->Pitch;
    desc.hDeviceDc = UlongToHandle( desc32->hDeviceDc );
    desc.pColorTable = wow64win_guest_memory_ptr( desc32->pColorTable );
    desc.hDc = UlongToHandle( desc32->hDc );
    desc.hBitmap = UlongToHandle( desc32->hBitmap );

    if (!(status = NtGdiDdDDICreateDCFromMemory( &desc )))
    {
        desc32->hDc = HandleToUlong( desc.hDc );
        desc32->hBitmap = HandleToUlong( desc.hBitmap );
    }
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDICreateDevice( UINT *args )
{
    D3DKMT_CREATEDEVICE32 *desc32 = get_memory_ptr( &args );

    return wow64_d3dkmt_lifecycle( desc32,
                                   WINE_D3DKMT_LIFECYCLE32_CREATE_DEVICE );
}

NTSTATUS WINAPI wow64_NtGdiDdDDICreateKeyedMutex( UINT *args )
{
    D3DKMT_CREATEKEYEDMUTEX *desc = get_memory_ptr( &args );
    return NtGdiDdDDICreateKeyedMutex( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDICreateKeyedMutex2( UINT *args )
{
    struct
    {
        UINT64 InitialValue;
        D3DKMT_HANDLE hSharedHandle;
        D3DKMT_HANDLE hKeyedMutex;
        ULONG pPrivateRuntimeData;
        UINT PrivateRuntimeDataSize;
        D3DKMT_CREATEKEYEDMUTEX2_FLAGS Flags;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_CREATEKEYEDMUTEX2 desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;

    desc.InitialValue = desc32->InitialValue;
    desc.hSharedHandle = desc32->hSharedHandle;
    desc.hKeyedMutex = desc32->hKeyedMutex;
    desc.pPrivateRuntimeData = wow64win_guest_memory_ptr( desc32->pPrivateRuntimeData );
    desc.PrivateRuntimeDataSize = desc32->PrivateRuntimeDataSize;
    desc.Flags = desc32->Flags;
    status = NtGdiDdDDICreateKeyedMutex2( &desc );
    desc32->hKeyedMutex = desc.hKeyedMutex;
    desc32->hSharedHandle = desc.hSharedHandle;
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDICreateSynchronizationObject( UINT *args )
{
    D3DKMT_CREATESYNCHRONIZATIONOBJECT *desc = get_memory_ptr( &args );
    return NtGdiDdDDICreateSynchronizationObject( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDICreateSynchronizationObject2( UINT *args )
{
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *desc = get_memory_ptr( &args );

    if (!desc) return STATUS_INVALID_PARAMETER;
    if (desc->Info.Type == D3DDDI_CPU_NOTIFICATION)
    {
        ULONG event = HandleToUlong( desc->Info.CPUNotification.Event );
        desc->Info.CPUNotification.Event = UlongToHandle( event );
    }

    return NtGdiDdDDICreateSynchronizationObject2( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIDestroyAllocation( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hDevice;
        D3DKMT_HANDLE hResource;
        ULONG phAllocationList;
        UINT AllocationCount;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_DESTROYALLOCATION desc;

    desc.hDevice = desc32->hDevice;
    desc.hResource = desc32->hResource;
    desc.phAllocationList = wow64win_guest_memory_ptr( desc32->phAllocationList );
    desc.AllocationCount = desc32->AllocationCount;
    return NtGdiDdDDIDestroyAllocation( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIDestroyAllocation2( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hDevice;
        D3DKMT_HANDLE hResource;
        ULONG phAllocationList;
        UINT AllocationCount;
        D3DDDICB_DESTROYALLOCATION2FLAGS Flags;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_DESTROYALLOCATION2 desc;

    desc.hDevice = desc32->hDevice;
    desc.hResource = desc32->hResource;
    desc.phAllocationList = wow64win_guest_memory_ptr( desc32->phAllocationList );
    desc.AllocationCount = desc32->AllocationCount;
    desc.Flags = desc32->Flags;
    return NtGdiDdDDIDestroyAllocation2( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIDestroyDCFromMemory( UINT *args )
{
    const struct
    {
        ULONG hDc;
        ULONG hBitmap;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_DESTROYDCFROMMEMORY desc;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hDc = UlongToHandle( desc32->hDc );
    desc.hBitmap = UlongToHandle( desc32->hBitmap );

    return NtGdiDdDDIDestroyDCFromMemory( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIDestroyDevice( UINT *args )
{
    D3DKMT_DESTROYDEVICE *desc32 = get_memory_ptr( &args );

    return wow64_d3dkmt_lifecycle( desc32,
                                   WINE_D3DKMT_LIFECYCLE32_DESTROY_DEVICE );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIDestroyKeyedMutex( UINT *args )
{
    D3DKMT_DESTROYKEYEDMUTEX *desc = get_memory_ptr( &args );
    return NtGdiDdDDIDestroyKeyedMutex( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIDestroySynchronizationObject( UINT *args )
{
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *desc = get_memory_ptr( &args );
    return NtGdiDdDDIDestroySynchronizationObject( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIEnumAdapters( UINT *args )
{
    D3DKMT_ENUMADAPTERS *desc = get_memory_ptr( &args );

    return NtGdiDdDDIEnumAdapters( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIEnumAdapters2( UINT *args )
{
    struct
    {
        ULONG NumAdapters;
        ULONG pAdapters;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_ENUMADAPTERS2 desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;

    desc.NumAdapters = desc32->NumAdapters;
    desc.pAdapters = wow64win_guest_memory_ptr( desc32->pAdapters );

    status = NtGdiDdDDIEnumAdapters2( &desc );

    desc32->NumAdapters = desc.NumAdapters;

    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIEscape( UINT *args )
{
    const struct
    {
        D3DKMT_HANDLE      hAdapter;
        D3DKMT_HANDLE      hDevice;
        D3DKMT_ESCAPETYPE  Type;
        D3DDDI_ESCAPEFLAGS Flags;
        ULONG              pPrivateDriverData;
        UINT               PrivateDriverDataSize;
        D3DKMT_HANDLE      hContext;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_ESCAPE desc;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hAdapter = desc32->hAdapter;
    desc.hDevice = desc32->hDevice;
    desc.Type = desc32->Type;
    desc.Flags = desc32->Flags;
    desc.pPrivateDriverData = wow64win_guest_memory_ptr( desc32->pPrivateDriverData );
    desc.PrivateDriverDataSize = desc32->PrivateDriverDataSize;
    desc.hContext = desc32->hContext;

    return NtGdiDdDDIEscape( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenAdapterFromDeviceName( UINT *args )
{
    struct _D3DKMT_OPENADAPTERFROMDEVICENAME
    {
        ULONG pDeviceName;
        D3DKMT_HANDLE hAdapter;
        LUID AdapterLuid;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_OPENADAPTERFROMDEVICENAME desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.pDeviceName = wow64win_guest_memory_ptr( desc32->pDeviceName );
    desc.hAdapter = desc32->hAdapter;
    desc.AdapterLuid = desc32->AdapterLuid;

    if (!(status = NtGdiDdDDIOpenAdapterFromDeviceName( &desc )))
    {
        desc32->hAdapter = desc.hAdapter;
        desc32->AdapterLuid = desc.AdapterLuid;
    }
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenAdapterFromHdc( UINT *args )
{
    struct
    {
        ULONG hDc;
        D3DKMT_HANDLE hAdapter;
        LUID AdapterLuid;
        D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
    } *desc32 = get_memory_ptr( &args );

    D3DKMT_OPENADAPTERFROMHDC desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hDc = UlongToHandle( desc32->hDc );
    desc.hAdapter = desc32->hAdapter;
    desc.AdapterLuid = desc32->AdapterLuid;
    desc.VidPnSourceId = desc32->VidPnSourceId;

    if (!(status = NtGdiDdDDIOpenAdapterFromHdc( &desc )))
    {
        desc32->hAdapter = desc.hAdapter;
        desc32->AdapterLuid = desc.AdapterLuid;
        desc32->VidPnSourceId = desc.VidPnSourceId;
    }
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenAdapterFromLuid( UINT *args )
{
    D3DKMT_OPENADAPTERFROMLUID *desc32 = get_memory_ptr( &args );

    return wow64_d3dkmt_lifecycle(
        desc32, WINE_D3DKMT_LIFECYCLE32_OPEN_ADAPTER_FROM_LUID );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenKeyedMutex( UINT *args )
{
    D3DKMT_OPENKEYEDMUTEX *desc = get_memory_ptr( &args );
    return NtGdiDdDDIOpenKeyedMutex( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenKeyedMutex2( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hSharedHandle;
        D3DKMT_HANDLE hKeyedMutex;
        ULONG pPrivateRuntimeData;
        UINT PrivateRuntimeDataSize;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_OPENKEYEDMUTEX2 desc;
    NTSTATUS status;

    desc.hSharedHandle = desc32->hSharedHandle;
    desc.hKeyedMutex = desc32->hKeyedMutex;
    desc.pPrivateRuntimeData = wow64win_guest_memory_ptr( desc32->pPrivateRuntimeData );
    desc.PrivateRuntimeDataSize = desc32->PrivateRuntimeDataSize;
    status = NtGdiDdDDIOpenKeyedMutex2( &desc );
    desc32->hKeyedMutex = desc.hKeyedMutex;
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenKeyedMutexFromNtHandle( UINT *args )
{
    struct
    {
        ULONG hNtHandle;
        D3DKMT_HANDLE hKeyedMutex;
        ULONG pPrivateRuntimeData;
        UINT PrivateRuntimeDataSize;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_OPENKEYEDMUTEXFROMNTHANDLE desc;
    NTSTATUS status;

    desc.hNtHandle = UlongToHandle( desc32->hNtHandle );
    desc.hKeyedMutex = desc32->hKeyedMutex;
    desc.pPrivateRuntimeData = wow64win_guest_memory_ptr( desc32->pPrivateRuntimeData );
    desc.PrivateRuntimeDataSize = desc32->PrivateRuntimeDataSize;
    status = NtGdiDdDDIOpenKeyedMutexFromNtHandle( &desc );
    desc32->hKeyedMutex = desc.hKeyedMutex;
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenNtHandleFromName( UINT *args )
{
    struct
    {
        DWORD dwDesiredAccess;
        ULONG pObjAttrib;
        ULONG hNtHandle;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_OPENNTHANDLEFROMNAME desc;
    struct object_attr64 attr;
    NTSTATUS status;

    desc.dwDesiredAccess = desc32->dwDesiredAccess;
    desc.pObjAttrib = objattr_32to64( &attr,
                                     wow64win_guest_memory_ptr( desc32->pObjAttrib ) );
    desc.hNtHandle = UlongToHandle( desc32->hNtHandle );
    status = NtGdiDdDDIOpenNtHandleFromName( &desc );
    desc32->hNtHandle = HandleToUlong( desc.hNtHandle );
    return status;
}

static NTSTATUS wow64_open_resource( void *desc32,
                                     enum wine_d3dkmt_open_resource32_variant variant )
{
    struct wine_d3dkmt_open_resource32 params =
    {
        .version = WINE_D3DKMT_OPEN_RESOURCE32_VERSION,
        .size = sizeof(params),
        .variant = variant,
    };
    NTSTATUS status;

    params.guest_desc = wow64win_guest_memory_addr( desc32 );
    if (variant == WINE_D3DKMT_OPEN_RESOURCE32_NT_HANDLE)
    {
        struct wine_d3dkmt_open_resource_nt32_desc local;

        wow64win_read_user( &local, desc32, sizeof(local) );
        params.guest_allocations = local.allocation_info;
        params.guest_private_runtime = local.private_runtime_data;
        params.guest_resource_private = local.resource_private_driver_data;
        params.guest_total_private = local.total_private_driver_data;
        params.guest_keyed_mutex_runtime = local.keyed_mutex_private_runtime_data;
        params.device = local.device;
        params.shared_handle = local.nt_handle;
        params.allocation_count = local.allocation_count;
        params.private_runtime_data_size = local.private_runtime_data_size;
        params.resource_private_driver_data_size = local.resource_private_driver_data_size;
        params.total_private_driver_data_size = local.total_private_driver_data_size;
        params.keyed_mutex_private_runtime_data_size =
            local.keyed_mutex_private_runtime_data_size;
        params.resource = local.resource;
        params.keyed_mutex = local.keyed_mutex;
        params.sync_object = local.sync_object;
    }
    else
    {
        struct wine_d3dkmt_open_resource32_desc local;

        wow64win_read_user( &local, desc32, sizeof(local) );
        params.guest_allocations = local.allocation_info;
        params.guest_private_runtime = local.private_runtime_data;
        params.guest_resource_private = local.resource_private_driver_data;
        params.guest_total_private = local.total_private_driver_data;
        params.device = local.device;
        params.shared_handle = local.global_share;
        params.allocation_count = local.allocation_count;
        params.private_runtime_data_size = local.private_runtime_data_size;
        params.resource_private_driver_data_size = local.resource_private_driver_data_size;
        params.total_private_driver_data_size = local.total_private_driver_data_size;
        params.resource = local.resource;
    }

    status = __wine_win32u_d3dkmt_open_resource( &params );
    if (params.fault_status) RtlRaiseStatus( params.fault_status );
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenResource( UINT *args )
{
    void *desc32 = get_memory_ptr( &args );

    return wow64_open_resource( desc32, WINE_D3DKMT_OPEN_RESOURCE32_V1 );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenResource2( UINT *args )
{
    void *desc32 = get_memory_ptr( &args );

    return wow64_open_resource( desc32, WINE_D3DKMT_OPEN_RESOURCE32_V2 );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenResourceFromNtHandle( UINT *args )
{
    void *desc32 = get_memory_ptr( &args );

    return wow64_open_resource( desc32, WINE_D3DKMT_OPEN_RESOURCE32_NT_HANDLE );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenSyncObjectFromNtHandle( UINT *args )
{
    struct
    {
        ULONG hNtHandle;
        D3DKMT_HANDLE hSyncObject;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_OPENSYNCOBJECTFROMNTHANDLE desc;
    NTSTATUS status;

    desc.hNtHandle = UlongToHandle( desc32->hNtHandle );
    desc.hSyncObject = desc32->hSyncObject;
    status = NtGdiDdDDIOpenSyncObjectFromNtHandle( &desc );
    desc32->hSyncObject = desc.hSyncObject;
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenSyncObjectFromNtHandle2( UINT *args )
{
    struct
    {
        ULONG hNtHandle;
        D3DKMT_HANDLE hDevice;
        D3DDDI_SYNCHRONIZATIONOBJECT_FLAGS Flags;
        D3DKMT_HANDLE hSyncObject;
        union
        {
            struct
            {
                ULONG FenceValueCPUVirtualAddress;
                D3DGPU_VIRTUAL_ADDRESS FenceValueGPUVirtualAddress;
                UINT EngineAffinity;
            } MonitoredFence;
            UINT64 Reserved[8];
        };
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 desc;
    NTSTATUS status;

    desc.hNtHandle = ULongToHandle( desc32->hNtHandle );
    desc.hDevice = desc32->hDevice;
    desc.Flags = desc32->Flags;
    desc.hSyncObject = desc32->hSyncObject;
    desc.MonitoredFence.EngineAffinity = desc32->MonitoredFence.EngineAffinity;
    desc.MonitoredFence.FenceValueCPUVirtualAddress =
        wow64win_guest_memory_ptr( desc32->MonitoredFence.FenceValueCPUVirtualAddress );
    desc.MonitoredFence.FenceValueGPUVirtualAddress = desc32->MonitoredFence.FenceValueGPUVirtualAddress;

    status = NtGdiDdDDIOpenSyncObjectFromNtHandle2( &desc );
    desc32->MonitoredFence.FenceValueCPUVirtualAddress =
        wow64win_guest_memory_addr( desc.MonitoredFence.FenceValueCPUVirtualAddress );
    desc32->MonitoredFence.FenceValueGPUVirtualAddress = desc.MonitoredFence.FenceValueGPUVirtualAddress;
    desc32->hSyncObject = desc.hSyncObject;
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenSyncObjectNtHandleFromName( UINT *args )
{
    struct
    {
        DWORD dwDesiredAccess;
        ULONG pObjAttrib;
        ULONG hNtHandle;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME desc;
    struct object_attr64 attr;
    NTSTATUS status;

    desc.dwDesiredAccess = desc32->dwDesiredAccess;
    desc.pObjAttrib = objattr_32to64( &attr,
                                     wow64win_guest_memory_ptr( desc32->pObjAttrib ) );
    desc.hNtHandle = UlongToHandle( desc32->hNtHandle );
    status = NtGdiDdDDIOpenSyncObjectNtHandleFromName( &desc );
    desc32->hNtHandle = HandleToUlong( desc.hNtHandle );
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIOpenSynchronizationObject( UINT *args )
{
    D3DKMT_OPENSYNCHRONIZATIONOBJECT *desc = get_memory_ptr( &args );
    return NtGdiDdDDIOpenSynchronizationObject( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIQueryAdapterInfo( UINT *args )
{
    struct _D3DKMT_QUERYADAPTERINFO
    {
        D3DKMT_HANDLE           hAdapter;
        KMTQUERYADAPTERINFOTYPE Type;
        ULONG                   pPrivateDriverData;
        UINT                    PrivateDriverDataSize;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_QUERYADAPTERINFO desc;

    if (!desc32) return STATUS_INVALID_PARAMETER;

    desc.hAdapter = desc32->hAdapter;
    desc.Type = desc32->Type;
    desc.pPrivateDriverData = wow64win_guest_memory_ptr( desc32->pPrivateDriverData );
    desc.PrivateDriverDataSize = desc32->PrivateDriverDataSize;

    return NtGdiDdDDIQueryAdapterInfo( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIQueryResourceInfo( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hDevice;
        D3DKMT_HANDLE hGlobalShare;
        ULONG pPrivateRuntimeData;
        UINT PrivateRuntimeDataSize;
        UINT TotalPrivateDriverDataSize;
        UINT ResourcePrivateDriverDataSize;
        UINT NumAllocations;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_QUERYRESOURCEINFO desc;
    NTSTATUS status;

    desc.hDevice = desc32->hDevice;
    desc.hGlobalShare = desc32->hGlobalShare;
    desc.pPrivateRuntimeData = wow64win_guest_memory_ptr( desc32->pPrivateRuntimeData );
    desc.PrivateRuntimeDataSize = desc32->PrivateRuntimeDataSize;
    desc.TotalPrivateDriverDataSize = desc32->TotalPrivateDriverDataSize;
    desc.ResourcePrivateDriverDataSize = desc32->ResourcePrivateDriverDataSize;
    desc.NumAllocations = desc32->NumAllocations;
    status = NtGdiDdDDIQueryResourceInfo( &desc );
    desc32->PrivateRuntimeDataSize = desc.PrivateRuntimeDataSize;
    desc32->TotalPrivateDriverDataSize = desc.TotalPrivateDriverDataSize;
    desc32->ResourcePrivateDriverDataSize = desc.ResourcePrivateDriverDataSize;
    desc32->NumAllocations = desc.NumAllocations;
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIQueryResourceInfoFromNtHandle( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hDevice;
        ULONG hNtHandle;
        ULONG pPrivateRuntimeData;
        UINT PrivateRuntimeDataSize;
        UINT TotalPrivateDriverDataSize;
        UINT ResourcePrivateDriverDataSize;
        UINT NumAllocations;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE desc;
    NTSTATUS status;

    desc.hDevice = desc32->hDevice;
    desc.hNtHandle = UlongToHandle( desc32->hNtHandle );
    desc.pPrivateRuntimeData = wow64win_guest_memory_ptr( desc32->pPrivateRuntimeData );
    desc.PrivateRuntimeDataSize = desc32->PrivateRuntimeDataSize;
    desc.TotalPrivateDriverDataSize = desc32->TotalPrivateDriverDataSize;
    desc.ResourcePrivateDriverDataSize = desc32->ResourcePrivateDriverDataSize;
    desc.NumAllocations = desc32->NumAllocations;
    status = NtGdiDdDDIQueryResourceInfoFromNtHandle( &desc );
    desc32->PrivateRuntimeDataSize = desc.PrivateRuntimeDataSize;
    desc32->TotalPrivateDriverDataSize = desc.TotalPrivateDriverDataSize;
    desc32->ResourcePrivateDriverDataSize = desc.ResourcePrivateDriverDataSize;
    desc32->NumAllocations = desc.NumAllocations;
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIQueryStatistics( UINT *args )
{
    D3DKMT_QUERYSTATISTICS *stats = get_memory_ptr( &args );

    return NtGdiDdDDIQueryStatistics( stats );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIQueryVideoMemoryInfo( UINT *args )
{
    struct _D3DKMT_QUERYVIDEOMEMORYINFO
    {
        ULONG                       hProcess;
        D3DKMT_HANDLE               hAdapter;
        D3DKMT_MEMORY_SEGMENT_GROUP MemorySegmentGroup;
        UINT64                      Budget;
        UINT64                      CurrentUsage;
        UINT64                      CurrentReservation;
        UINT64                      AvailableForReservation;
        UINT                        PhysicalAdapterIndex;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_QUERYVIDEOMEMORYINFO desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hProcess = LongToHandle( desc32->hProcess );
    desc.hAdapter = desc32->hAdapter;
    desc.MemorySegmentGroup = desc32->MemorySegmentGroup;
    desc.PhysicalAdapterIndex = desc32->PhysicalAdapterIndex;
    desc.Budget = desc32->Budget;
    desc.CurrentUsage = desc32->CurrentUsage;
    desc.CurrentReservation = desc32->CurrentReservation;
    desc.AvailableForReservation = desc32->AvailableForReservation;

    if (!(status = NtGdiDdDDIQueryVideoMemoryInfo( &desc )))
    {
        desc32->Budget = desc.Budget;
        desc32->CurrentUsage = desc.CurrentUsage;
        desc32->CurrentReservation = desc.CurrentReservation;
        desc32->AvailableForReservation = desc.AvailableForReservation;
    }
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDIReleaseKeyedMutex( UINT *args )
{
    D3DKMT_RELEASEKEYEDMUTEX *desc = get_memory_ptr( &args );
    return NtGdiDdDDIReleaseKeyedMutex( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIReleaseKeyedMutex2( UINT *args )
{
    struct
    {
        D3DKMT_HANDLE hKeyedMutex;
        UINT64 Key;
        UINT64 FenceValue;
        ULONG pPrivateRuntimeData;
        UINT PrivateRuntimeDataSize;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_RELEASEKEYEDMUTEX2 desc;
    NTSTATUS status;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hKeyedMutex = desc32->hKeyedMutex;
    desc.Key = desc32->Key;
    desc.FenceValue = desc32->FenceValue;
    desc.pPrivateRuntimeData = UlongToHandle( desc32->pPrivateRuntimeData );
    desc.PrivateRuntimeDataSize = desc32->PrivateRuntimeDataSize;
    status = NtGdiDdDDIReleaseKeyedMutex2( &desc );

    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDISetQueuedLimit( UINT *args )
{
    D3DKMT_SETQUEUEDLIMIT *desc = get_memory_ptr( &args );

    return NtGdiDdDDISetQueuedLimit( desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDISetVidPnSourceOwner( UINT *args )
{
    const struct
    {
        D3DKMT_HANDLE hDevice;
        ULONG pType;
        ULONG pVidPnSourceId;
        UINT VidPnSourceCount;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_SETVIDPNSOURCEOWNER desc;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hDevice = desc32->hDevice;
    desc.pType = wow64win_guest_memory_ptr( desc32->pType );
    desc.pVidPnSourceId = wow64win_guest_memory_ptr( desc32->pVidPnSourceId );
    desc.VidPnSourceCount = desc32->VidPnSourceCount;

    return NtGdiDdDDISetVidPnSourceOwner( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIShareObjects( UINT *args )
{
    UINT count = get_ulong( &args );
    D3DKMT_HANDLE *handles = get_memory_ptr( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_memory_ptr( &args );
    UINT access = get_ulong( &args );
    ULONG *handle_ptr = get_memory_ptr( &args );

    struct object_attr64 attr;
    HANDLE handle = UlongToHandle( *handle_ptr );
    NTSTATUS status;

    status = NtGdiDdDDIShareObjects( count, handles, objattr_32to64( &attr, attr32 ), access, &handle );
    *handle_ptr = HandleToULong( handle );
    return status;
}

NTSTATUS WINAPI wow64_NtGdiDdDDISignalSynchronizationObjectFromCpu( UINT *args )
{
    const struct
    {
        D3DKMT_HANDLE hDevice;
        UINT ObjectCount;
        ULONG ObjectHandleArray;
        ULONG FenceValueArray;
        D3DDDICB_SIGNALFLAGS Flags;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU desc;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hDevice = desc32->hDevice;
    desc.ObjectCount = desc32->ObjectCount;
    desc.ObjectHandleArray = wow64win_guest_memory_ptr( desc32->ObjectHandleArray );
    desc.FenceValueArray = wow64win_guest_memory_ptr( desc32->FenceValueArray );
    desc.Flags = desc32->Flags;

    return NtGdiDdDDISignalSynchronizationObjectFromCpu( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDdDDIWaitForSynchronizationObjectFromCpu( UINT *args )
{
    const struct
    {
        D3DKMT_HANDLE hDevice;
        UINT ObjectCount;
        ULONG ObjectHandleArray;
        ULONG FenceValueArray;
        ULONG hAsyncEvent;
        D3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPU_FLAGS Flags;
    } *desc32 = get_memory_ptr( &args );
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU desc;

    if (!desc32) return STATUS_INVALID_PARAMETER;
    desc.hDevice = desc32->hDevice;
    desc.ObjectCount = desc32->ObjectCount;
    desc.ObjectHandleArray = wow64win_guest_memory_ptr( desc32->ObjectHandleArray );
    desc.FenceValueArray = wow64win_guest_memory_ptr( desc32->FenceValueArray );
    desc.hAsyncEvent = UlongToHandle( desc32->hAsyncEvent );
    desc.Flags = desc32->Flags;

    return NtGdiDdDDIWaitForSynchronizationObjectFromCpu( &desc );
}

NTSTATUS WINAPI wow64_NtGdiDeleteClientObj( UINT *args )
{
    HGDIOBJ obj = get_handle( &args );

    return NtGdiDeleteClientObj( obj );
}

NTSTATUS WINAPI wow64_NtGdiDeleteObjectApp( UINT *args )
{
    HGDIOBJ obj = get_handle( &args );

    return NtGdiDeleteObjectApp( obj );
}

NTSTATUS WINAPI wow64_NtGdiDescribePixelFormat( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT format = get_ulong( &args );
    UINT size = get_ulong( &args );
    PIXELFORMATDESCRIPTOR *descr = get_memory_ptr( &args );

    return NtGdiDescribePixelFormat( hdc, format, size, descr );
}

NTSTATUS WINAPI wow64_NtGdiDoPalette( UINT *args )
{
    HGDIOBJ handle = get_handle( &args );
    WORD start = get_ulong( &args );
    WORD count = get_ulong( &args );
    void *entries = get_memory_ptr( &args );
    DWORD func = get_ulong( &args );
    BOOL inbound = get_ulong( &args );

    return NtGdiDoPalette( handle, start, count, entries, func, inbound );
}

NTSTATUS WINAPI wow64_NtGdiDrawStream( UINT *args )
{
    HDC hdc = get_handle( &args );
    ULONG in = get_ulong( &args );
    void *pvin = get_memory_ptr( &args );

    return NtGdiDrawStream( hdc, in, pvin );
}

NTSTATUS WINAPI wow64_NtGdiEllipse( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );

    return NtGdiEllipse( hdc, left, top, right, bottom );
}

NTSTATUS WINAPI wow64_NtGdiEndDoc( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiEndDoc( hdc );
}

NTSTATUS WINAPI wow64_NtGdiEndPage( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiEndPage( hdc );
}

NTSTATUS WINAPI wow64_NtGdiEndPath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiEndPath( hdc );
}

NTSTATUS WINAPI wow64_NtGdiEnumFonts( UINT *args )
{
    HDC hdc = get_handle( &args );
    ULONG type = get_ulong( &args );
    ULONG win32_compat = get_ulong( &args );
    ULONG face_name_len = get_ulong( &args );
    const WCHAR *face_name = get_memory_ptr( &args );
    ULONG charset = get_ulong( &args );
    ULONG *count = get_memory_ptr( &args );
    void *buf = get_memory_ptr( &args );

    return NtGdiEnumFonts( hdc, type, win32_compat, face_name_len, face_name, charset, count, buf );
}

NTSTATUS WINAPI wow64_NtGdiExcludeClipRect( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );

    return NtGdiExcludeClipRect( hdc, left, top, right, bottom );
}

NTSTATUS WINAPI wow64_NtGdiEqualRgn( UINT *args )
{
    HRGN hrgn1 = get_handle( &args );
    HRGN hrgn2 = get_handle( &args );

    return NtGdiEqualRgn( hrgn1, hrgn2 );
}

NTSTATUS WINAPI wow64_NtGdiExtCreatePen( UINT *args )
{
    DWORD style = get_ulong( &args );
    DWORD width = get_ulong( &args );
    ULONG brush_style = get_ulong( &args );
    ULONG color = get_ulong( &args );
    ULONG_PTR client_hatch = get_ulong( &args );
    ULONG_PTR hatch = get_ulong( &args );
    DWORD style_count = get_ulong( &args );
    const DWORD *style_bits = get_memory_ptr( &args );
    ULONG dib_size = get_ulong( &args );
    BOOL old_style = get_ulong( &args );
    HBRUSH brush = get_handle( &args );

    return HandleToUlong( NtGdiExtCreatePen( style, width, brush_style, color, client_hatch,
                                             hatch, style_count, style_bits, dib_size,
                                             old_style, brush ));
}

NTSTATUS WINAPI wow64_NtGdiExtCreateRegion( UINT *args )
{
    const XFORM *xform = get_memory_ptr( &args );
    DWORD count = get_ulong( &args );
    const RGNDATA *data = get_memory_ptr( &args );

    return HandleToUlong( NtGdiExtCreateRegion( xform, count, data ));
}

NTSTATUS WINAPI wow64_NtGdiExtEscape( UINT *args )
{
    HDC hdc = get_handle( &args );
    WCHAR *driver = get_memory_ptr( &args );
    INT driver_id = get_ulong( &args );
    INT escape = get_ulong( &args );
    INT input_size = get_ulong( &args );
    const char *input = get_memory_ptr( &args );
    INT output_size = get_ulong( &args );
    char *output = get_memory_ptr( &args );

    return NtGdiExtEscape( hdc, driver, driver_id, escape, input_size, input, output_size, output );
}

NTSTATUS WINAPI wow64_NtGdiExtFloodFill( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );
    COLORREF color = get_ulong( &args );
    UINT type = get_ulong( &args );

    return NtGdiExtFloodFill( hdc, x, y, color, type );
}

NTSTATUS WINAPI wow64_NtGdiExtGetObjectW( UINT *args )
{
    HGDIOBJ handle = get_handle( &args );
    INT count = get_ulong( &args );
    ULONG buffer_addr = get_ulong( &args );
    void *buffer = wow64win_guest_memory_ptr( buffer_addr );

    switch (gdi_handle_type( handle ))
    {
    case NTGDI_OBJ_BITMAP:
        {
            BITMAP32 *bitmap32 = buffer;
            struct
            {
                BITMAP32 dsBm;
                BITMAPINFOHEADER dsBmih;
                DWORD  dsBitfields[3];
                ULONG  dshSection;
                DWORD  dsOffset;
            } *dib32 = buffer;
            DIBSECTION dib;
            int ret;

            if (buffer)
            {
                if (count < sizeof(*bitmap32)) return 0;
                count = count < sizeof(*dib32) ? sizeof(BITMAP) : sizeof(DIBSECTION);
            }

            if (!(ret = NtGdiExtGetObjectW( handle, count, buffer ? &dib : NULL ))) return 0;

            if (bitmap32)
            {
                bitmap32->bmType = dib.dsBm.bmType;
                bitmap32->bmWidth = dib.dsBm.bmWidth;
                bitmap32->bmHeight = dib.dsBm.bmHeight;
                bitmap32->bmWidthBytes = dib.dsBm.bmWidthBytes;
                bitmap32->bmPlanes = dib.dsBm.bmPlanes;
                bitmap32->bmBitsPixel = dib.dsBm.bmBitsPixel;
                bitmap32->bmBits = wow64win_guest_memory_addr( dib.dsBm.bmBits );
            }
            if (ret != sizeof(dib)) return sizeof(*bitmap32);

            if (dib32)
            {
                dib32->dsBmih = dib.dsBmih;
                dib32->dsBitfields[0] = dib.dsBitfields[0];
                dib32->dsBitfields[1] = dib.dsBitfields[1];
                dib32->dsBitfields[2] = dib.dsBitfields[2];
                dib32->dshSection = HandleToUlong( dib.dshSection );
                dib32->dsOffset = dib.dsOffset;
            }
            return sizeof(*dib32);
        }

    case NTGDI_OBJ_PEN:
    case NTGDI_OBJ_EXTPEN:
        {
            union
            {
                LOGPEN logpen;
                EXTLOGPEN extlogpen;
            } stack;
            EXTLOGPEN32 out;
            EXTLOGPEN *pen;
            void *guest_buffer;
            INT needed, native_size, ret, guest_size;
            SIZE_T style_size;
            NTSTATUS status;

            /* Preserve win32u's argument-order diagnostic.  It is performed only
             * after the native handle lookup, and the low client value must not be
             * translated into the high-shadow mapping for this special case. */
            if (buffer_addr && !(buffer_addr >> 16))
                return NtGdiExtGetObjectW( handle, count, buffer );

            /* A NULL-buffer call validates the object and obtains its bounded
             * native result size without touching guest memory or allocating
             * from the caller-controlled count. */
            if (!(needed = NtGdiExtGetObjectW( handle, 0, NULL ))) return 0;
            if (!buffer_addr)
            {
                if (needed == sizeof(LOGPEN)) return needed;
                if (needed < (INT)FIELD_OFFSET( EXTLOGPEN, elpStyleEntry )) return 0;
                return needed - sizeof(ULONG);
            }

            if (needed == sizeof(LOGPEN))
            {
                if (count < (INT)sizeof(LOGPEN)) return 0;

                /* A null cosmetic pen has a native-only EXTLOGPEN result when
                 * the adjusted 32-bit EXTLOGPEN size is requested exactly. */
                native_size = count == sizeof(EXTLOGPEN32) ? sizeof(EXTLOGPEN) :
                                                             sizeof(LOGPEN);
            }
            else
            {
                if (needed < (INT)FIELD_OFFSET( EXTLOGPEN, elpStyleEntry )) return 0;
                guest_size = needed - sizeof(ULONG);
                if (count < guest_size) return 0;
                native_size = needed;
            }

            if (native_size <= (INT)sizeof(stack)) pen = &stack.extlogpen;
            else if (!(pen = Wow64AllocateTemp( native_size ))) return 0;

            if (!(ret = NtGdiExtGetObjectW( handle, native_size, pen ))) return 0;
            if (ret > native_size) return 0;

            if (ret == sizeof(LOGPEN)) guest_size = ret;
            else
            {
                if (ret < (INT)FIELD_OFFSET( EXTLOGPEN, elpStyleEntry )) return 0;

                out.elpPenStyle = pen->elpPenStyle;
                out.elpWidth = pen->elpWidth;
                out.elpBrushStyle = pen->elpBrushStyle;
                out.elpColor = pen->elpColor;
                out.elpHatch = pen->elpHatch;
                out.elpNumEntries = pen->elpNumEntries;

                style_size = ret - FIELD_OFFSET( EXTLOGPEN, elpStyleEntry );
                memmove( (BYTE *)pen + FIELD_OFFSET( EXTLOGPEN32, elpStyleEntry ),
                         (BYTE *)pen + FIELD_OFFSET( EXTLOGPEN, elpStyleEntry ), style_size );
                memcpy( pen, &out, FIELD_OFFSET( EXTLOGPEN32, elpStyleEntry ));
                guest_size = ret - sizeof(ULONG);
            }

            if ((ULONGLONG)guest_size > 0x100000000ull - buffer_addr)
                RtlRaiseStatus( STATUS_ACCESS_VIOLATION );
            guest_buffer = wow64win_guest_memory_ptr( buffer_addr );
            status = wow64win_try_write_user( guest_buffer, pen, guest_size );
            if (status) RtlRaiseStatus( status );
            return guest_size;
        }

    default:
        return NtGdiExtGetObjectW( handle, count, buffer );
    }
}

NTSTATUS WINAPI wow64_NtGdiExtSelectClipRgn( UINT *args )
{
    HDC hdc = get_handle( &args );
    HRGN region = get_handle( &args );
    INT mode = get_ulong( &args );

    return NtGdiExtSelectClipRgn( hdc, region, mode );
}

NTSTATUS WINAPI wow64_NtGdiExtTextOutW( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );
    UINT flags = get_ulong( &args );
    const RECT *rect = get_memory_ptr( &args );
    const WCHAR *str = get_memory_ptr( &args );
    UINT count = get_ulong( &args );
    const INT *dx = get_memory_ptr( &args );
    DWORD cp = get_ulong( &args );

    return NtGdiExtTextOutW( hdc, x, y, flags, rect, str, count, dx, cp );
}

NTSTATUS WINAPI wow64_NtGdiFillPath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiFillPath( hdc );
}

NTSTATUS WINAPI wow64_NtGdiFillRgn( UINT *args )
{
    HDC hdc = get_handle( &args );
    HRGN hrgn = get_handle( &args );
    HBRUSH hbrush = get_handle( &args );

    return NtGdiFillRgn( hdc, hrgn, hbrush );
}

NTSTATUS WINAPI wow64_NtGdiFlattenPath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiFlattenPath( hdc );
}

NTSTATUS WINAPI wow64_NtGdiFlush( UINT *args )
{
    return NtGdiFlush();
}

NTSTATUS WINAPI wow64_NtGdiFontIsLinked( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiFontIsLinked( hdc );
}

NTSTATUS WINAPI wow64_NtGdiFrameRgn( UINT *args )
{
    HDC hdc = get_handle( &args );
    HRGN hrgn = get_handle( &args );
    HBRUSH brush = get_handle( &args );
    INT width = get_ulong( &args );
    INT height = get_ulong( &args );

    return NtGdiFrameRgn( hdc, hrgn, brush, width, height );
}

NTSTATUS WINAPI wow64_NtGdiGetAndSetDCDword( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT method = get_ulong( &args );
    DWORD value = get_ulong( &args );
    DWORD *result = get_memory_ptr( &args );

    return NtGdiGetAndSetDCDword( hdc, method, value, result );
}

NTSTATUS WINAPI wow64_NtGdiGetAppClipBox( UINT *args )
{
    HDC hdc = get_handle( &args );
    RECT *rect = get_memory_ptr( &args );

    return NtGdiGetAppClipBox( hdc, rect );
}

NTSTATUS WINAPI wow64_NtGdiGetBitmapBits( UINT *args )
{
    HBITMAP bitmap = get_handle( &args );
    LONG count = get_ulong( &args );
    void *bits = get_memory_ptr( &args );

    return NtGdiGetBitmapBits( bitmap, count, bits );
}

NTSTATUS WINAPI wow64_NtGdiGetBitmapDimension( UINT *args )
{
    HBITMAP bitmap = get_handle( &args );
    SIZE *size = get_memory_ptr( &args );

    return NtGdiGetBitmapDimension( bitmap, size );
}

NTSTATUS WINAPI wow64_NtGdiGetBoundsRect( UINT *args )
{
    HDC hdc = get_handle( &args );
    RECT *rect = get_memory_ptr( &args );
    UINT flags = get_ulong( &args );

    return NtGdiGetBoundsRect( hdc, rect, flags );
}

NTSTATUS WINAPI wow64_NtGdiGetCharABCWidthsW( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT first = get_ulong( &args );
    UINT last = get_ulong( &args );
    WCHAR *chars = get_memory_ptr( &args );
    ULONG flags = get_ulong( &args );
    void *buffer = get_memory_ptr( &args );

    return NtGdiGetCharABCWidthsW( hdc, first, last, chars, flags, buffer );
}

NTSTATUS WINAPI wow64_NtGdiGetCharWidthW( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT first_char = get_ulong( &args );
    UINT last_char = get_ulong( &args );
    WCHAR *chars = get_memory_ptr( &args );
    ULONG flags = get_ulong( &args );
    void *buffer = get_memory_ptr( &args );

    return NtGdiGetCharWidthW( hdc, first_char, last_char, chars, flags, buffer );
}

NTSTATUS WINAPI wow64_NtGdiGetCharWidthInfo( UINT *args )
{
    HDC hdc = get_handle( &args );
    struct char_width_info *info = get_memory_ptr( &args );

    return NtGdiGetCharWidthInfo( hdc, info );
}

NTSTATUS WINAPI wow64_NtGdiGetColorAdjustment( UINT *args )
{
    HDC hdc = get_handle( &args );
    COLORADJUSTMENT *ca = get_memory_ptr( &args );

    return NtGdiGetColorAdjustment( hdc, ca );
}

NTSTATUS WINAPI wow64_NtGdiGetDCDword( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT method = get_ulong( &args );
    DWORD *result = get_memory_ptr( &args );

    return NtGdiGetDCDword( hdc, method, result );
}

NTSTATUS WINAPI wow64_NtGdiGetDCObject( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT type = get_ulong( &args );

    return HandleToUlong( NtGdiGetDCObject( hdc, type ));
}

NTSTATUS WINAPI wow64_NtGdiGetDCPoint( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT method = get_ulong( &args );
    POINT *result = get_memory_ptr( &args );

    return NtGdiGetDCPoint( hdc, method, result );
}

NTSTATUS WINAPI wow64_NtGdiGetDIBitsInternal( UINT *args )
{
    HDC hdc = get_handle( &args );
    HBITMAP hbitmap = get_handle( &args );
    UINT startscan = get_ulong( &args );
    UINT lines = get_ulong( &args );
    void *bits = get_memory_ptr( &args );
    BITMAPINFO *info = get_memory_ptr( &args );
    UINT coloruse = get_ulong( &args );
    UINT max_bits = get_ulong( &args );
    UINT max_info = get_ulong( &args );

    return NtGdiGetDIBitsInternal( hdc, hbitmap, startscan, lines, bits, info, coloruse,
                                   max_bits, max_info );
}

NTSTATUS WINAPI wow64_NtGdiGetDeviceCaps( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT cap = get_ulong( &args );

    return NtGdiGetDeviceCaps( hdc, cap );
}

NTSTATUS WINAPI wow64_NtGdiGetDeviceGammaRamp( UINT *args )
{
    HDC hdc = get_handle( &args );
    void *ptr = get_memory_ptr( &args );

    return NtGdiGetDeviceGammaRamp( hdc, ptr );
}

NTSTATUS WINAPI wow64_NtGdiGetFontData( UINT *args )
{
    HDC hdc = get_handle( &args );
    DWORD table = get_ulong( &args );
    DWORD offset = get_ulong( &args );
    void *buffer = get_memory_ptr( &args );
    DWORD length = get_ulong( &args );

    return NtGdiGetFontData( hdc, table, offset, buffer, length );
}

NTSTATUS WINAPI wow64_NtGdiGetFontFileData( UINT *args )
{
    DWORD instance_id = get_ulong( &args );
    DWORD file_index = get_ulong( &args );
    UINT64 *offset = get_memory_ptr( &args );
    void *buff = get_memory_ptr( &args );
    SIZE_T buff_size = get_ulong( &args );

    return NtGdiGetFontFileData( instance_id, file_index, offset, buff, buff_size );
}

NTSTATUS WINAPI wow64_NtGdiGetFontFileInfo( UINT *args )
{
    DWORD instance_id = get_ulong( &args );
    DWORD file_index = get_ulong( &args );
    struct font_fileinfo *info = get_memory_ptr( &args );
    SIZE_T size = get_ulong( &args );
    ULONG *needed32 = get_memory_ptr( &args );

    SIZE_T needed;
    BOOL ret;

    ret = NtGdiGetFontFileInfo( instance_id, file_index, info, size, size_32to64( &needed, needed32 ));
    put_size( needed32, needed );
    return ret;
}

NTSTATUS WINAPI wow64_NtGdiGetFontUnicodeRanges( UINT *args )
{
    HDC hdc = get_handle( &args );
    GLYPHSET *lpgs = get_memory_ptr( &args );

    return NtGdiGetFontUnicodeRanges( hdc, lpgs );
}

NTSTATUS WINAPI wow64_NtGdiGetGlyphIndicesW( UINT *args )
{
    HDC hdc = get_handle( &args );
    const WCHAR *str = get_memory_ptr( &args );
    INT count = get_ulong( &args );
    WORD *indices = get_memory_ptr( &args );
    DWORD flags = get_ulong( &args );

    return NtGdiGetGlyphIndicesW( hdc, str, count, indices, flags );
}

NTSTATUS WINAPI wow64_NtGdiGetGlyphOutline( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT ch = get_ulong( &args );
    UINT format = get_ulong( &args );
    GLYPHMETRICS *metrics = get_memory_ptr( &args );
    DWORD size = get_ulong( &args );
    void *buffer = get_memory_ptr( &args );
    const MAT2 *mat2 = get_memory_ptr( &args );
    BOOL ignore_rotation = get_ulong( &args );

    return NtGdiGetGlyphOutline( hdc, ch, format, metrics, size, buffer, mat2, ignore_rotation );
}

NTSTATUS WINAPI wow64_NtGdiGetKerningPairs( UINT *args )
{
    HDC hdc = get_handle( &args );
    DWORD count = get_ulong( &args );
    KERNINGPAIR *kern_pair = get_memory_ptr( &args );

    return NtGdiGetKerningPairs( hdc, count, kern_pair );
}

NTSTATUS WINAPI wow64_NtGdiGetMiterLimit( UINT *args )
{
    HDC hdc = get_handle( &args );
    FLOAT *limit = get_memory_ptr( &args );

    return NtGdiGetMiterLimit( hdc, limit );
}

NTSTATUS WINAPI wow64_NtGdiGetNearestColor( UINT *args )
{
    HDC hdc = get_handle( &args );
    COLORREF color = get_ulong( &args );

    return NtGdiGetNearestColor( hdc, color );
}

NTSTATUS WINAPI wow64_NtGdiGetNearestPaletteIndex( UINT *args )
{
    HPALETTE hpalette = get_handle( &args );
    COLORREF color = get_ulong( &args );

    return NtGdiGetNearestPaletteIndex( hpalette, color );
}

NTSTATUS WINAPI wow64_NtGdiGetOutlineTextMetricsInternalW( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT size = get_ulong( &args );
    ULONG otm32_addr = get_ulong( &args );
    OUTLINETEXTMETRIC32 *otm32 = wow64win_guest_memory_ptr( otm32_addr );
    ULONG opts = get_ulong( &args );

    OUTLINETEXTMETRICW *otm, otm_buf;
    OUTLINETEXTMETRIC32 out;
    UINT needed, ret, size64, size32, copy_size;
    ULONG_PTR offset;
    SIZE_T tail_size;
    NTSTATUS status;
    static const size_t otm_size_diff = sizeof(*otm) - sizeof(*otm32);

    /* Validate the DC and obtain a provider-owned upper bound before using the
     * caller-controlled size or probing the output. */
    if (!(needed = NtGdiGetOutlineTextMetricsInternalW( hdc, 0, NULL, opts ))) return 0;
    if (!otm32_addr)
        return needed >= sizeof(*otm) ? needed - otm_size_diff : needed;

    if (size <= sizeof(*otm32)) size64 = sizeof(otm_buf);
    else if (size > ~(UINT)0 - otm_size_diff) size64 = needed;
    else size64 = size + otm_size_diff;
    size64 = min( size64, needed );

    if (size64 <= sizeof(otm_buf)) otm = &otm_buf;
    else if (!(otm = Wow64AllocateTemp( size64 ))) return 0;

    if (!(ret = NtGdiGetOutlineTextMetricsInternalW( hdc, size64, otm, opts ))) return 0;
    if (ret > size64 || ret < sizeof(*otm)) return ret > size64 ? 0 : ret;

    memcpy( &out, otm, FIELD_OFFSET( OUTLINETEXTMETRIC32, otmpFamilyName ));
    if (out.otmSize >= sizeof(*otm)) out.otmSize -= otm_size_diff;

    offset = (ULONG_PTR)otm->otmpFamilyName;
    out.otmpFamilyName = offset >= otm_size_diff && offset - otm_size_diff <= MAXDWORD ?
                         offset - otm_size_diff : 0;
    offset = (ULONG_PTR)otm->otmpFaceName;
    out.otmpFaceName = offset >= otm_size_diff && offset - otm_size_diff <= MAXDWORD ?
                       offset - otm_size_diff : 0;
    offset = (ULONG_PTR)otm->otmpStyleName;
    out.otmpStyleName = offset >= otm_size_diff && offset - otm_size_diff <= MAXDWORD ?
                        offset - otm_size_diff : 0;
    offset = (ULONG_PTR)otm->otmpFullName;
    out.otmpFullName = offset >= otm_size_diff && offset - otm_size_diff <= MAXDWORD ?
                       offset - otm_size_diff : 0;

    tail_size = ret - sizeof(*otm);
    memmove( (BYTE *)otm + sizeof(out), (BYTE *)otm + sizeof(*otm), tail_size );
    memcpy( otm, &out, sizeof(out) );

    size32 = ret - otm_size_diff;
    copy_size = min( size, size32 );
    if (copy_size)
    {
        if ((ULONGLONG)copy_size > 0x100000000ull - otm32_addr)
            RtlRaiseStatus( STATUS_ACCESS_VIOLATION );
        status = wow64win_try_write_user( otm32, otm, copy_size );
        if (status) RtlRaiseStatus( status );
    }
    return size32;
}

NTSTATUS WINAPI wow64_NtGdiGetPath( UINT *args )
{
    HDC hdc = get_handle( &args );
    POINT *points = get_memory_ptr( &args );
    BYTE *types = get_memory_ptr( &args );
    INT size = get_ulong( &args );

    return NtGdiGetPath( hdc, points, types, size );
}

NTSTATUS WINAPI wow64_NtGdiGetPixel( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );

    return NtGdiGetPixel( hdc, x, y );
}

NTSTATUS WINAPI wow64_NtGdiGetRandomRgn( UINT *args )
{
    HDC hdc = get_handle( &args );
    HRGN region = get_handle( &args );
    INT code = get_ulong( &args );

    return NtGdiGetRandomRgn( hdc, region, code );
}

NTSTATUS WINAPI wow64_NtGdiGetRasterizerCaps( UINT *args )
{
    RASTERIZER_STATUS *status = get_memory_ptr( &args );
    UINT size = get_ulong( &args );

    return NtGdiGetRasterizerCaps( status, size );
}

NTSTATUS WINAPI wow64_NtGdiGetRealizationInfo( UINT *args )
{
    HDC hdc = get_handle( &args );
    struct font_realization_info *info = get_memory_ptr( &args );

    return NtGdiGetRealizationInfo( hdc, info );
}

NTSTATUS WINAPI wow64_NtGdiGetRegionData( UINT *args )
{
    HRGN hrgn = get_handle( &args );
    DWORD count = get_ulong( &args );
    RGNDATA *data = get_memory_ptr( &args );

    return NtGdiGetRegionData( hrgn, count, data );
}

NTSTATUS WINAPI wow64_NtGdiGetTextCharsetInfo( UINT *args )
{
    HDC hdc = get_handle( &args );
    FONTSIGNATURE *fs = get_memory_ptr( &args );
    DWORD flags = get_ulong( &args );

    return NtGdiGetTextCharsetInfo( hdc, fs, flags );
}

NTSTATUS WINAPI wow64_NtGdiGetTextExtentExW( UINT *args )
{
    HDC hdc = get_handle( &args );
    const WCHAR *str = get_memory_ptr( &args );
    INT count = get_ulong( &args );
    INT max_ext = get_ulong( &args );
    INT *nfit = get_memory_ptr( &args );
    INT *dxs = get_memory_ptr( &args );
    SIZE *size = get_memory_ptr( &args );
    UINT flags = get_ulong( &args );

    return NtGdiGetTextExtentExW( hdc, str, count, max_ext, nfit, dxs, size, flags );
}

NTSTATUS WINAPI wow64_NtGdiGetTextFaceW( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT count = get_ulong( &args );
    WCHAR *name = get_memory_ptr( &args );
    BOOL alias_name = get_ulong( &args );

    return NtGdiGetTextFaceW( hdc, count, name, alias_name );
}

NTSTATUS WINAPI wow64_NtGdiGetTextMetricsW( UINT *args )
{
    HDC hdc = get_handle( &args );
    TEXTMETRICW *metrics = get_memory_ptr( &args );
    ULONG flags = get_ulong( &args );

    return NtGdiGetTextMetricsW( hdc, metrics, flags );
}

NTSTATUS WINAPI wow64_NtGdiGradientFill( UINT *args )
{
    HDC hdc = get_handle( &args );
    TRIVERTEX *vert_array = get_memory_ptr( &args );
    ULONG nvert = get_ulong( &args );
    void *grad_array = get_memory_ptr( &args );
    ULONG ngrad = get_ulong( &args );
    ULONG mode = get_ulong( &args );

    return NtGdiGradientFill( hdc, vert_array, nvert, grad_array, ngrad, mode );
}

NTSTATUS WINAPI wow64_NtGdiIcmBrushInfo( UINT *args )
{
    HDC hdc = get_handle( &args );
    HBRUSH handle = get_handle( &args );
    BITMAPINFO *info = get_memory_ptr( &args );
    void *bits = get_memory_ptr( &args );
    ULONG *bits_size = get_memory_ptr( &args );
    UINT *usage = get_memory_ptr( &args );
    BOOL *unk = get_memory_ptr( &args );
    UINT mode = get_ulong( &args );

    return NtGdiIcmBrushInfo( hdc, handle, info, bits, bits_size, usage, unk, mode );
}

NTSTATUS WINAPI wow64_NtGdiInvertRgn( UINT *args )
{
    HDC hdc = get_handle( &args );
    HRGN hrgn = get_handle( &args );

    return NtGdiInvertRgn( hdc, hrgn );
}

NTSTATUS WINAPI wow64_NtGdiLineTo( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );

    return NtGdiLineTo( hdc, x, y );
}

NTSTATUS WINAPI wow64_NtGdiMakeFontDir( UINT *args )
{
    DWORD embed = get_ulong( &args );
    BYTE *buffer = get_memory_ptr( &args );
    UINT size = get_ulong( &args );
    WCHAR *path = get_memory_ptr( &args );
    UINT len = get_ulong( &args );

    return NtGdiMakeFontDir( embed, buffer, size, path, len );
}

NTSTATUS WINAPI wow64_NtGdiMaskBlt( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x_dst = get_ulong( &args );
    INT y_dst = get_ulong( &args );
    INT width_dst = get_ulong( &args );
    INT height_dst = get_ulong( &args );
    HDC hdc_src = get_handle( &args );
    INT x_src = get_ulong( &args );
    INT y_src = get_ulong( &args );
    HBITMAP mask = get_handle( &args );
    INT x_mask = get_ulong( &args );
    INT y_mask = get_ulong( &args );
    DWORD rop = get_ulong( &args );
    DWORD bk_color = get_ulong( &args );

    return NtGdiMaskBlt( hdc, x_dst, y_dst, width_dst, height_dst, hdc_src,
                         x_src, y_src, mask, x_mask, y_mask, rop, bk_color );
}

NTSTATUS WINAPI wow64_NtGdiModifyWorldTransform( UINT *args )
{
    HDC hdc = get_handle( &args );
    const XFORM *xform = get_memory_ptr( &args );
    DWORD mode = get_ulong( &args );

    return NtGdiModifyWorldTransform( hdc, xform, mode );
}

NTSTATUS WINAPI wow64_NtGdiMoveTo( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );
    POINT *pt = get_memory_ptr( &args );

    return NtGdiMoveTo( hdc, x, y, pt );
}

NTSTATUS WINAPI wow64_NtGdiPolyDraw( UINT *args )
{
    HDC hdc = get_handle( &args );
    const POINT *points = get_memory_ptr( &args );
    const BYTE *types = get_memory_ptr( &args );
    DWORD count = get_ulong( &args );

    return NtGdiPolyDraw( hdc, points, types, count );
}

NTSTATUS WINAPI wow64_NtGdiPolyPolyDraw( UINT *args )
{
    HDC hdc = get_handle( &args );
    const POINT *points = get_memory_ptr( &args );
    const ULONG *counts = get_memory_ptr( &args );
    DWORD count = get_ulong( &args );
    UINT function = get_ulong( &args );

    return NtGdiPolyPolyDraw( hdc, points, counts, count, function );
}

NTSTATUS WINAPI wow64_NtGdiRectangle( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );

    return NtGdiRectangle( hdc, left, top, right, bottom );
}

NTSTATUS WINAPI wow64_NtGdiResetDC( UINT *args )
{
    HDC hdc = get_handle( &args );
    const DEVMODEW *devmode = get_memory_ptr( &args );
    BOOL *banding = get_memory_ptr( &args );
    DRIVER_INFO_2W *driver_info = get_memory_ptr( &args );
    void *dev = get_memory_ptr( &args );

    return NtGdiResetDC( hdc, devmode, banding, driver_info, dev );
}

NTSTATUS WINAPI wow64_NtGdiResizePalette( UINT *args )
{
    HPALETTE palette = get_handle( &args );
    UINT count = get_ulong( &args );

    return NtGdiResizePalette( palette, count );
}

NTSTATUS WINAPI wow64_NtGdiRestoreDC( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT level = get_ulong( &args );

    return NtGdiRestoreDC( hdc, level );
}

NTSTATUS WINAPI wow64_NtGdiGetRgnBox( UINT *args )
{
    HRGN hrgn = get_handle( &args );
    RECT *rect = get_memory_ptr( &args );

    return NtGdiGetRgnBox( hrgn, rect );
}

NTSTATUS WINAPI wow64_NtGdiRoundRect( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );
    INT ell_width = get_ulong( &args );
    INT ell_height = get_ulong( &args );

    return NtGdiRoundRect( hdc, left, top, right, bottom, ell_width, ell_height );
}

NTSTATUS WINAPI wow64_NtGdiGetSpoolMessage( UINT *args )
{
    void *ptr1 = get_memory_ptr( &args );
    DWORD data2 = get_ulong( &args );
    void *ptr3 = get_memory_ptr( &args );
    DWORD data4 = get_ulong( &args );

    return NtGdiGetSpoolMessage( ptr1, data2, ptr3, data4 );
}

NTSTATUS WINAPI wow64_NtGdiGetSystemPaletteUse( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiGetSystemPaletteUse( hdc );
}

NTSTATUS WINAPI wow64_NtGdiGetTransform( UINT *args )
{
    HDC hdc = get_handle( &args );
    DWORD which = get_ulong( &args );
    XFORM *xform = get_memory_ptr( &args );

    return NtGdiGetTransform( hdc, which, xform );
}

NTSTATUS WINAPI wow64_NtGdiHfontCreate( UINT *args )
{
    const void *logfont = get_memory_ptr( &args );
    ULONG unk2 = get_ulong( &args );
    ULONG unk3 = get_ulong( &args );
    ULONG unk4 = get_ulong( &args );
    void *data = get_memory_ptr( &args );

    return HandleToUlong( NtGdiHfontCreate( logfont, unk2, unk3, unk4, data ));
}

NTSTATUS WINAPI wow64_NtGdiInitSpool( UINT *args )
{
    return NtGdiInitSpool();
}

NTSTATUS WINAPI wow64_NtGdiIntersectClipRect( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );

    return NtGdiIntersectClipRect( hdc, left, top, right, bottom );
}

NTSTATUS WINAPI wow64_NtGdiOffsetClipRgn( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );

    return NtGdiOffsetClipRgn( hdc, x, y );
}

NTSTATUS WINAPI wow64_NtGdiOffsetRgn( UINT *args )
{
    HRGN hrgn = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );

    return NtGdiOffsetRgn( hrgn, x, y );
}

NTSTATUS WINAPI wow64_NtGdiOpenDCW( UINT *args )
{
    UNICODE_STRING32 *device32 = get_memory_ptr( &args );
    const DEVMODEW *devmode = get_memory_ptr( &args );
    UNICODE_STRING32 *output32 = get_memory_ptr( &args );
    ULONG type = get_ulong( &args );
    BOOL is_display = get_ulong( &args );
    HANDLE hspool = get_handle( &args );
    DRIVER_INFO_2W *driver_info = get_memory_ptr( &args );
    void *pdev = get_memory_ptr( &args );

    UNICODE_STRING device, output;
    HDC ret = NtGdiOpenDCW( unicode_str_32to64( &device, device32 ), devmode,
                            unicode_str_32to64( &output, output32 ), type,
                            is_display, hspool, driver_info, pdev );
    return HandleToUlong( ret );
}

NTSTATUS WINAPI wow64_NtGdiPatBlt( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT width = get_ulong( &args );
    INT height = get_ulong( &args );
    DWORD rop = get_ulong( &args );

    return NtGdiPatBlt( hdc, left, top, width, height, rop );
}

NTSTATUS WINAPI wow64_NtGdiPathToRegion( UINT *args )
{
    HDC hdc = get_handle( &args );

    return HandleToUlong( NtGdiPathToRegion( hdc ));
}

NTSTATUS WINAPI wow64_NtGdiPlgBlt( UINT *args )
{
    HDC hdc = get_handle( &args );
    const POINT *point = get_memory_ptr( &args );
    HDC hdc_src = get_handle( &args );
    INT x_src = get_ulong( &args );
    INT y_src = get_ulong( &args );
    INT width = get_ulong( &args );
    INT height = get_ulong( &args );
    HBITMAP mask = get_handle( &args );
    INT x_mask = get_ulong( &args );
    INT y_mask = get_ulong( &args );
    DWORD bk_color = get_ulong( &args );

    return NtGdiPlgBlt( hdc, point, hdc_src, x_src, y_src, width, height,
                        mask, x_mask, y_mask, bk_color );
}

NTSTATUS WINAPI wow64_NtGdiPtInRegion( UINT *args )
{
    HRGN hrgn = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );

    return NtGdiPtInRegion( hrgn, x, y );
}

NTSTATUS WINAPI wow64_NtGdiPtVisible( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );

    return NtGdiPtVisible( hdc, x, y );
}

NTSTATUS WINAPI wow64_NtGdiRectInRegion( UINT *args )
{
    HRGN hrgn = get_handle( &args );
    const RECT *rect = get_memory_ptr( &args );

    return NtGdiRectInRegion( hrgn, rect );
}

NTSTATUS WINAPI wow64_NtGdiRectVisible( UINT *args )
{
    HDC hdc = get_handle( &args );
    const RECT *rect = get_memory_ptr( &args );

    return NtGdiRectVisible( hdc, rect );
}

NTSTATUS WINAPI wow64_NtGdiRemoveFontMemResourceEx( UINT *args )
{
    HANDLE handle = get_handle( &args );

    return NtGdiRemoveFontMemResourceEx( handle );
}

NTSTATUS WINAPI wow64_NtGdiRemoveFontResourceW( UINT *args )
{
    const WCHAR *str = get_memory_ptr( &args );
    ULONG size = get_ulong( &args );
    ULONG files = get_ulong( &args );
    DWORD flags = get_ulong( &args );
    DWORD tid = get_ulong( &args );
    void *dv = get_memory_ptr( &args );

    return NtGdiRemoveFontResourceW( str, size, files, flags, tid, dv );
}

NTSTATUS WINAPI wow64_NtGdiSaveDC( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiSaveDC( hdc );
}

NTSTATUS WINAPI wow64_NtGdiScaleViewportExtEx( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x_num = get_ulong( &args );
    INT x_denom = get_ulong( &args );
    INT y_num = get_ulong( &args );
    INT y_denom = get_ulong( &args );
    SIZE *size = get_memory_ptr( &args );

    return NtGdiScaleViewportExtEx( hdc, x_num, x_denom, y_num, y_denom, size );
}

NTSTATUS WINAPI wow64_NtGdiScaleWindowExtEx( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x_num = get_ulong( &args );
    INT x_denom = get_ulong( &args );
    INT y_num = get_ulong( &args );
    INT y_denom = get_ulong( &args );
    SIZE *size = get_memory_ptr( &args );

    return NtGdiScaleWindowExtEx( hdc, x_num, x_denom, y_num, y_denom, size );
}

NTSTATUS WINAPI wow64_NtGdiSelectBitmap( UINT *args )
{
    HDC hdc = get_handle( &args );
    HGDIOBJ handle = get_handle( &args );

    return HandleToUlong( NtGdiSelectBitmap( hdc, handle ));
}

NTSTATUS WINAPI wow64_NtGdiSelectBrush( UINT *args )
{
    HDC hdc = get_handle( &args );
    HGDIOBJ handle = get_handle( &args );

    return HandleToUlong( NtGdiSelectBrush( hdc, handle ));
}

NTSTATUS WINAPI wow64_NtGdiSelectClipPath( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT mode = get_ulong( &args );

    return NtGdiSelectClipPath( hdc, mode );
}

NTSTATUS WINAPI wow64_NtGdiSelectFont( UINT *args )
{
    HDC hdc = get_handle( &args );
    HGDIOBJ handle = get_handle( &args );

    return HandleToUlong( NtGdiSelectFont( hdc, handle ));
}

NTSTATUS WINAPI wow64_NtGdiSelectPen( UINT *args )
{
    HDC hdc = get_handle( &args );
    HGDIOBJ handle = get_handle( &args );

    return HandleToUlong( NtGdiSelectPen( hdc, handle ));
}

NTSTATUS WINAPI wow64_NtGdiSetBitmapBits( UINT *args )
{
    HBITMAP hbitmap = get_handle( &args );
    LONG count = get_ulong( &args );
    const void *bits = get_memory_ptr( &args );

    return NtGdiSetBitmapBits( hbitmap, count, bits );
}

NTSTATUS WINAPI wow64_NtGdiSetBitmapDimension( UINT *args )
{
    HBITMAP hbitmap = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );
    SIZE *prev_size = get_memory_ptr( &args );

    return NtGdiSetBitmapDimension( hbitmap, x, y, prev_size );
}

NTSTATUS WINAPI wow64_NtGdiSetBoundsRect( UINT *args )
{
    HDC hdc = get_handle( &args );
    const RECT *rect = get_memory_ptr( &args );
    UINT flags = get_ulong( &args );

    return NtGdiSetBoundsRect( hdc, rect, flags );
}

NTSTATUS WINAPI wow64_NtGdiSetBrushOrg( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );
    POINT *prev_org = get_memory_ptr( &args );

    return NtGdiSetBrushOrg( hdc, x, y, prev_org );
}

NTSTATUS WINAPI wow64_NtGdiSetColorAdjustment( UINT *args )
{
    HDC hdc = get_handle( &args );
    const COLORADJUSTMENT *ca = get_memory_ptr( &args );

    return NtGdiSetColorAdjustment( hdc, ca );
}

NTSTATUS WINAPI wow64_NtGdiSetDIBitsToDeviceInternal( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x_dst = get_ulong( &args );
    INT y_dst = get_ulong( &args );
    DWORD cx = get_ulong( &args );
    DWORD cy = get_ulong( &args );
    INT x_src = get_ulong( &args );
    INT y_src = get_ulong( &args );
    UINT startscan = get_ulong( &args );
    UINT lines = get_ulong( &args );
    const void *bits = get_memory_ptr( &args );
    const BITMAPINFO *bmi = get_memory_ptr( &args );
    UINT coloruse = get_ulong( &args );
    UINT max_bits = get_ulong( &args );
    UINT max_info = get_ulong( &args );
    BOOL xform_coords = get_ulong( &args );
    HANDLE xform = get_handle( &args );

    return NtGdiSetDIBitsToDeviceInternal( hdc, x_dst, y_dst, cx, cy, x_src, y_src,
                                           startscan, lines, bits, bmi, coloruse,
                                           max_bits, max_info, xform_coords, xform );
}

NTSTATUS WINAPI wow64_NtGdiSetDeviceGammaRamp( UINT *args )
{
    HDC hdc = get_handle( &args );
    void *ptr = get_memory_ptr( &args );

    return NtGdiSetDeviceGammaRamp( hdc, ptr );
}

NTSTATUS WINAPI wow64_NtGdiSetLayout( UINT *args )
{
    HDC hdc = get_handle( &args );
    LONG wox = get_ulong( &args );
    DWORD layout = get_ulong( &args );

    return NtGdiSetLayout( hdc, wox, layout );
}

NTSTATUS WINAPI wow64_NtGdiSetMagicColors( UINT *args )
{
    HDC hdc = get_handle( &args );
    DWORD magic = get_ulong( &args );
    ULONG index = get_ulong( &args );

    return NtGdiSetMagicColors( hdc, magic, index );
}

NTSTATUS WINAPI wow64_NtGdiSetMetaRgn( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiSetMetaRgn( hdc );
}

NTSTATUS WINAPI wow64_NtGdiSetMiterLimit( UINT *args )
{
    HDC hdc = get_handle( &args );
    DWORD limit = get_ulong( &args );
    FLOAT *old_limit = get_memory_ptr( &args );

    return NtGdiSetMiterLimit( hdc, limit, old_limit );
}

NTSTATUS WINAPI wow64_NtGdiSetPixel( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x = get_ulong( &args );
    INT y = get_ulong( &args );
    COLORREF color = get_ulong( &args );

    return NtGdiSetPixel( hdc, x, y, color );
}

NTSTATUS WINAPI wow64_NtGdiSetPixelFormat( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT format = get_ulong( &args );

    return NtGdiSetPixelFormat( hdc, format );
}

NTSTATUS WINAPI wow64_NtGdiSetRectRgn( UINT *args )
{
    HRGN hrgn = get_handle( &args );
    INT left = get_ulong( &args );
    INT top = get_ulong( &args );
    INT right = get_ulong( &args );
    INT bottom = get_ulong( &args );

    return NtGdiSetRectRgn( hrgn, left, top, right, bottom );
}

NTSTATUS WINAPI wow64_NtGdiSetSystemPaletteUse( UINT *args )
{
    HDC hdc = get_handle( &args );
    UINT use = get_ulong( &args );

    return NtGdiSetSystemPaletteUse( hdc, use );
}

NTSTATUS WINAPI wow64_NtGdiSetTextJustification( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT extra = get_ulong( &args );
    INT breaks = get_ulong( &args );

    return NtGdiSetTextJustification( hdc, extra, breaks );
}

NTSTATUS WINAPI wow64_NtGdiSetVirtualResolution( UINT *args )
{
    HDC hdc = get_handle( &args );
    DWORD horz_res = get_ulong( &args );
    DWORD vert_res = get_ulong( &args );
    DWORD horz_size = get_ulong( &args );
    DWORD vert_size = get_ulong( &args );

    return NtGdiSetVirtualResolution( hdc, horz_res, vert_res, horz_size, vert_size );
}

NTSTATUS WINAPI wow64_NtGdiStartDoc( UINT *args )
{
    HDC hdc = get_handle( &args );
    const struct
    {
        INT   cbSize;
        ULONG lpszDocName;
        ULONG lpszOutput;
        ULONG lpszDatatype;
        DWORD fwType;
    } *doc32 = get_memory_ptr( &args );
    BOOL *banding = get_memory_ptr( &args );
    INT job = get_ulong( &args );

    DOCINFOW doc;
    doc.cbSize = sizeof(doc);
    doc.lpszDocName = wow64win_guest_memory_ptr( doc32->lpszDocName );
    doc.lpszOutput = wow64win_guest_memory_ptr( doc32->lpszOutput );
    doc.lpszDatatype = wow64win_guest_memory_ptr( doc32->lpszDatatype );
    doc.fwType = doc32->fwType;

    return NtGdiStartDoc( hdc, &doc, banding, job );
}

NTSTATUS WINAPI wow64_NtGdiStartPage( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiStartPage( hdc );
}

NTSTATUS WINAPI wow64_NtGdiStretchBlt( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x_dst = get_ulong( &args );
    INT y_dst = get_ulong( &args );
    INT width_dst = get_ulong( &args );
    INT height_dst = get_ulong( &args );
    HDC hdc_src = get_handle( &args );
    INT x_src = get_ulong( &args );
    INT y_src = get_ulong( &args );
    INT width_src = get_ulong( &args );
    INT height_src = get_ulong( &args );
    DWORD rop = get_ulong( &args );
    COLORREF bk_color = get_ulong( &args );

    return NtGdiStretchBlt( hdc, x_dst, y_dst, width_dst, height_dst, hdc_src,
                            x_src, y_src, width_src, height_src, rop, bk_color );
}

NTSTATUS WINAPI wow64_NtGdiStretchDIBitsInternal( UINT *args )
{
    HDC hdc = get_handle( &args );
    INT x_dst = get_ulong( &args );
    INT y_dst = get_ulong( &args );
    INT width_dst = get_ulong( &args );
    INT height_dst = get_ulong( &args );
    INT x_src = get_ulong( &args );
    INT y_src = get_ulong( &args );
    INT width_src = get_ulong( &args );
    INT height_src = get_ulong( &args );
    const void *bits = get_memory_ptr( &args );
    const BITMAPINFO *bmi = get_memory_ptr( &args );
    UINT coloruse = get_ulong( &args );
    DWORD rop = get_ulong( &args );
    UINT max_info = get_ulong( &args );
    UINT max_bits = get_ulong( &args );
    HANDLE xform = get_handle( &args );

    return NtGdiStretchDIBitsInternal( hdc, x_dst, y_dst, width_dst, height_dst,
                                       x_src, y_src, width_src, height_src, bits, bmi,
                                       coloruse, rop, max_info, max_bits, xform );
}

NTSTATUS WINAPI wow64_NtGdiStrokeAndFillPath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiStrokeAndFillPath( hdc );
}

NTSTATUS WINAPI wow64_NtGdiStrokePath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiStrokePath( hdc );
}

NTSTATUS WINAPI wow64_NtGdiSwapBuffers( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiSwapBuffers( hdc );
}

NTSTATUS WINAPI wow64_NtGdiTransparentBlt( UINT *args )
{
    HDC hdc = get_handle( &args );
    int x_dst = get_ulong( &args );
    int y_dst = get_ulong( &args );
    int width_dst = get_ulong( &args );
    int height_dst = get_ulong( &args );
    HDC hdc_src = get_handle( &args );
    int x_src = get_ulong( &args );
    int y_src = get_ulong( &args );
    int width_src = get_ulong( &args );
    int height_src = get_ulong( &args );
    UINT color = get_ulong( &args );

    return NtGdiTransparentBlt( hdc, x_dst, y_dst, width_dst, height_dst, hdc_src,
                                x_src, y_src, width_src, height_src, color );
}

NTSTATUS WINAPI wow64_NtGdiTransformPoints( UINT *args )
{
    HDC hdc = get_handle( &args );
    const POINT *points_in = get_memory_ptr( &args );
    POINT *points_out = get_memory_ptr( &args );
    INT count = get_ulong( &args );
    UINT mode = get_ulong( &args );

    return NtGdiTransformPoints( hdc, points_in, points_out, count, mode );
}

NTSTATUS WINAPI wow64_NtGdiUnrealizeObject( UINT *args )
{
    HGDIOBJ obj = get_handle( &args );

    return NtGdiUnrealizeObject( obj );
}

NTSTATUS WINAPI wow64_NtGdiUpdateColors( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiUpdateColors( hdc );
}

NTSTATUS WINAPI wow64_NtGdiWidenPath( UINT *args )
{
    HDC hdc = get_handle( &args );

    return NtGdiWidenPath( hdc );
}
