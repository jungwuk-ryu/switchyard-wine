/*
 * Unit test suite for version functions
 *
 * Copyright 2006 Robert Shearman
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "wine/test.h"
#include "winbase.h"
#include "winioctl.h"
#include "winternl.h"
#include "appmodel.h"
#include "wine/appx_package_graph.h"

static BOOL (WINAPI * pGetProductInfo)(DWORD, DWORD, DWORD, DWORD, DWORD *);
static UINT (WINAPI * pEnumSystemFirmwareTables)(DWORD, void *, DWORD);
static UINT (WINAPI * pGetSystemFirmwareTable)(DWORD, DWORD, void *, DWORD);
static LONG (WINAPI * pGetCurrentApplicationUserModelId)(UINT32 *, WCHAR *);
static LONG (WINAPI * pGetCurrentPackageFamilyName)(UINT32 *, WCHAR *);
static LONG (WINAPI * pGetCurrentPackageFullName)(UINT32 *, WCHAR *);
static LONG (WINAPI * pGetCurrentPackageId)(UINT32 *, BYTE *);
static LONG (WINAPI * pGetCurrentPackageInfo)(UINT32, UINT32 *, BYTE *, UINT32 *);
static LONG (WINAPI * pGetCurrentPackageInfo2)(UINT32, PackagePathType, UINT32 *, BYTE *, UINT32 *);
static LONG (WINAPI * pGetCurrentPackagePath)(UINT32 *, WCHAR *);
static LONG (WINAPI * pGetCurrentPackagePath2)(PackagePathType, UINT32 *, WCHAR *);
static LONG (WINAPI * pGetPackageFamilyName)(HANDLE, UINT32 *, WCHAR *);
static LONG (WINAPI * pGetPackageFullName)(HANDLE, UINT32 *, WCHAR *);
static LONG (WINAPI * pGetPackagePathByFullName)(const WCHAR *, UINT32 *, WCHAR *);
static LONG (WINAPI * pGetPackagePathByFullName2)(const WCHAR *, PackagePathType, UINT32 *, WCHAR *);
static LONG (WINAPI * pGetPackagesByPackageFamily)(const WCHAR *, UINT32 *, WCHAR **, UINT32 *, WCHAR *);
static LONG (WINAPI * pPackageFamilyNameFromFullName)(const WCHAR *, UINT32 *, WCHAR *);
static LONG (WINAPI * pPackageFamilyNameFromId)(const PACKAGE_ID *, UINT32 *, WCHAR *);
static LONG (WINAPI * pPackageFullNameFromId)(const PACKAGE_ID *, UINT32 *, WCHAR *);
static LONG (WINAPI * pPackageIdFromFullName)(const WCHAR *, UINT32, UINT32 *, BYTE *);
static LONG (WINAPI * pPackageNameAndPublisherIdFromFamilyName)(const WCHAR *, UINT32 *, WCHAR *,
                                                               UINT32 *, WCHAR *);
static NTSTATUS (WINAPI * pNtQuerySystemInformation)(SYSTEM_INFORMATION_CLASS, void *, ULONG, ULONG *);
static NTSTATUS (WINAPI * pRtlGetVersion)(RTL_OSVERSIONINFOEXW *);

#define GET_PROC(func)                                     \
    p##func = (void *)GetProcAddress(hmod, #func);

/* Firmware table providers */
#define ACPI 0x41435049
#define FIRM 0x4649524D
#define RSMB 0x52534D42

static void init_function_pointers(void)
{
    HMODULE hmod;

    hmod = GetModuleHandleA("kernel32.dll");

    GET_PROC(GetProductInfo);
    GET_PROC(EnumSystemFirmwareTables);
    GET_PROC(GetSystemFirmwareTable);
    GET_PROC(GetCurrentApplicationUserModelId);
    GET_PROC(GetCurrentPackageFamilyName);
    GET_PROC(GetCurrentPackageFullName);
    GET_PROC(GetCurrentPackageId);
    GET_PROC(GetCurrentPackageInfo);
    GET_PROC(GetCurrentPackageInfo2);
    GET_PROC(GetCurrentPackagePath);
    GET_PROC(GetCurrentPackagePath2);
    GET_PROC(GetPackageFamilyName);
    GET_PROC(GetPackageFullName);
    GET_PROC(GetPackagePathByFullName);
    GET_PROC(GetPackagePathByFullName2);
    GET_PROC(GetPackagesByPackageFamily);
    GET_PROC(PackageFamilyNameFromFullName);
    GET_PROC(PackageFamilyNameFromId);
    GET_PROC(PackageFullNameFromId);
    GET_PROC(PackageIdFromFullName);
    GET_PROC(PackageNameAndPublisherIdFromFamilyName);

    hmod = GetModuleHandleA("ntdll.dll");

    GET_PROC(NtQuerySystemInformation);
    GET_PROC(RtlGetVersion);
}

static void test_GetProductInfo(void)
{
    DWORD product;
    DWORD res;
    DWORD table[] = {9,8,7,6,
                     7,0,0,0,
                     6,2,0,0,
                     6,1,2,0,
                     6,1,1,0,
                     6,1,0,2,
                     6,1,0,0,
                     6,0,3,0,
                     6,0,2,0,
                     6,0,1,5,
                     6,0,1,0,
                     6,0,0,0,
                     5,3,0,0,
                     5,2,0,0,
                     5,1,0,0,
                     5,0,0,0,
                     0};

    DWORD *entry = table;

    if (!pGetProductInfo)
    {
        /* Not present before Vista */
        win_skip("GetProductInfo() not available\n");
        return;
    }

    while (*entry)
    {
        /* SetLastError() / GetLastError(): value is untouched */
        product = 0xdeadbeef;
        SetLastError(0xdeadbeef);
        res = pGetProductInfo(entry[0], entry[1], entry[2], entry[3], &product);

        if (entry[0] >= 6)
            ok(res && (product > PRODUCT_UNDEFINED) && (product <= PRODUCT_ENTERPRISE_S_N_EVALUATION),
               "got %ld and 0x%lx (expected TRUE and a valid PRODUCT_* value)\n", res, product);
        else
            ok(!res && !product && (GetLastError() == 0xdeadbeef),
               "got %ld and 0x%lx with 0x%lx (expected FALSE and PRODUCT_UNDEFINED with LastError untouched)\n",
               res, product, GetLastError());

        entry+= 4;
    }

    /* NULL pointer is not a problem */
    SetLastError(0xdeadbeef);
    res = pGetProductInfo(6, 1, 0, 0, NULL);
    ok( (!res) && (GetLastError() == 0xdeadbeef),
        "got %ld with 0x%lx (expected FALSE with LastError untouched\n", res, GetLastError());
}

static void test_GetVersionEx(void)
{
    OSVERSIONINFOA infoA;
    OSVERSIONINFOEXA infoExA;
    BOOL ret;

    if (0)
    {
        /* Silently crashes on XP */
        GetVersionExA(NULL);
    }

    SetLastError(0xdeadbeef);
    memset(&infoA,0,sizeof infoA);
    ret = GetVersionExA(&infoA);
    ok(!ret, "Expected GetVersionExA to fail\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER ||
        GetLastError() == 0xdeadbeef /* Win9x */,
        "Expected ERROR_INSUFFICIENT_BUFFER or 0xdeadbeef (Win9x), got %ld\n",
        GetLastError());

    SetLastError(0xdeadbeef);
    infoA.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA) / 2;
    ret = GetVersionExA(&infoA);
    ok(!ret, "Expected GetVersionExA to fail\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER ||
        GetLastError() == 0xdeadbeef /* Win9x */,
        "Expected ERROR_INSUFFICIENT_BUFFER or 0xdeadbeef (Win9x), got %ld\n",
        GetLastError());

    SetLastError(0xdeadbeef);
    infoA.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA) * 2;
    ret = GetVersionExA(&infoA);
    ok(!ret, "Expected GetVersionExA to fail\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER ||
        GetLastError() == 0xdeadbeef /* Win9x */,
        "Expected ERROR_INSUFFICIENT_BUFFER or 0xdeadbeef (Win9x), got %ld\n",
        GetLastError());

    SetLastError(0xdeadbeef);
    infoA.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
    ret = GetVersionExA(&infoA);
    ok(ret, "Expected GetVersionExA to succeed\n");
    ok(GetLastError() == 0xdeadbeef,
        "Expected 0xdeadbeef, got %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    infoExA.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXA);
    ret = GetVersionExA((OSVERSIONINFOA *)&infoExA);
    ok(ret, "GetVersionExA failed.\n");

    if (!infoExA.wServicePackMajor && !infoExA.wServicePackMinor)
        ok(!infoExA.szCSDVersion[0], "got '%s'\n", infoExA.szCSDVersion);
}

static void test_VerifyVersionInfo(void)
{
    enum srcversion_mode
    {
        SRCVERSION_ZERO         = 0,
        SRCVERSION_CURRENT      = 1,
        SRCVERSION_INC_MINOR    = 2,
        SRCVERSION_INC_SP_MINOR = 3,
        SRCVERSION_INC_SP_MAJOR = 4,
        SRCVERSION_DEC_SP_MAJOR = 5,
        SRCVERSION_DEC_MAJOR    = 6,
        SRCVERSION_INC_BUILD    = 7,
        SRCVERSION_REQUIRES_SP  = 0x1000,
    };

    struct verify_version_test
    {
        DWORD verifymask; /* Type mask for VerifyVersionInfo() */
        DWORD srcinfo;    /* The way current version info is modified. */
        DWORD err;        /* Error code on failure, 0 on success. */

        DWORD typemask1;
        DWORD condition1;
        DWORD typemask2;
        DWORD condition2;
        DWORD typemask3;
        DWORD condition3;
        DWORD typemask4;
        DWORD condition4;
    } verify_version_tests[] =
    {
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_AND,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            0,

            VER_MAJORVERSION, VER_LESS_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_AND,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_OR,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
        },
        {
            VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            0,

            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            0,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MAJORVERSION, VER_LESS_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_MAJOR,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_AND,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION,
            SRCVERSION_ZERO,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
            SRCVERSION_ZERO,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_SUITENAME,
            SRCVERSION_ZERO,
            0,

            VER_SUITENAME, VER_AND,
        },
        {
            VER_SUITENAME,
            SRCVERSION_ZERO,
            0,

            VER_SUITENAME, VER_OR,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MINORVERSION, VER_LESS_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_BUILD,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_BUILD,
            0,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MINORVERSION, VER_GREATER,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_LESS_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_AND,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            ERROR_OLD_WIN_VERSION,

            VER_SERVICEPACKMAJOR, VER_GREATER,
            VER_SERVICEPACKMINOR, VER_EQUAL,
        },
    };

    OSVERSIONINFOEXA info;
    DWORD servicepack;
    unsigned int i;
    BOOL ret;

    /* Before we start doing some tests we should check what the version of
     * the ServicePack is. Tests on a box with no ServicePack will fail otherwise.
     */
    info.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXA);
    GetVersionExA((OSVERSIONINFOA *)&info);
    servicepack = info.wServicePackMajor;
    if (servicepack == 0)
        skip("There is no ServicePack on this system. Some tests will be skipped.\n");

    /* Win8.1+ returns Win8 version in GetVersionEx when there's no app manifest targeting 8.1 */
    if (info.dwMajorVersion == 6 && info.dwMinorVersion == 2)
    {
        RTL_OSVERSIONINFOEXW rtlinfo;
        rtlinfo.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOEXW);
        ok(!pRtlGetVersion(&rtlinfo), "RtlGetVersion failed\n");

        if (rtlinfo.dwMajorVersion != 6 || rtlinfo.dwMinorVersion != 2)
        {
            skip("GetVersionEx and VerifyVersionInfo are faking values\n");
            return;
        }
    }

    for (i = 0; i < ARRAY_SIZE(verify_version_tests); i++)
    {
        struct verify_version_test *test = &verify_version_tests[i];
        DWORD srcinfo = test->srcinfo;
        ULONGLONG mask;

        if (servicepack == 0 && srcinfo & SRCVERSION_REQUIRES_SP)
            continue;
        srcinfo &= ~SRCVERSION_REQUIRES_SP;

        info.dwOSVersionInfoSize = sizeof(info);
        GetVersionExA((OSVERSIONINFOA *)&info);

        switch (srcinfo)
        {
        case SRCVERSION_ZERO:
            memset(&info, 0, sizeof(info));
            break;
        case SRCVERSION_INC_MINOR:
            info.dwMinorVersion++;
            break;
        case SRCVERSION_INC_SP_MINOR:
            info.wServicePackMinor++;
            break;
        case SRCVERSION_INC_SP_MAJOR:
            info.wServicePackMajor++;
            break;
        case SRCVERSION_DEC_SP_MAJOR:
            info.wServicePackMajor--;
            break;
        case SRCVERSION_DEC_MAJOR:
            info.dwMajorVersion--;
            break;
        case SRCVERSION_INC_BUILD:
            info.dwBuildNumber++;
            break;
        default:
            ;
        }

        mask = VerSetConditionMask(0, test->typemask1, test->condition1);
        if (test->typemask2)
            mask = VerSetConditionMask(mask, test->typemask2, test->condition2);
        if (test->typemask3)
            mask = VerSetConditionMask(mask, test->typemask3, test->condition3);
        if (test->typemask4)
            mask = VerSetConditionMask(mask, test->typemask4, test->condition4);

        SetLastError(0xdeadbeef);
        ret = VerifyVersionInfoA(&info, test->verifymask, mask);
        ok(test->err ? !ret : ret, "%u: unexpected return value %d.\n", i, ret);
        if (!ret)
            ok(GetLastError() == test->err, "%u: unexpected error code %ld, expected %ld.\n", i, GetLastError(), test->err);
    }

    /* test handling of version numbers */
    /* v3.10 is always less than v4.x even
     * if the minor version is tested */
    info.dwMajorVersion = 3;
    info.dwMinorVersion = 10;
    ret = VerifyVersionInfoA(&info, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
        VerSetConditionMask(VerSetConditionMask(0, VER_MINORVERSION, VER_GREATER_EQUAL),
            VER_MAJORVERSION, VER_GREATER_EQUAL));
    ok(ret, "VerifyVersionInfoA failed with error %ld\n", GetLastError());

    info.dwMinorVersion = 0;
    info.wServicePackMajor = 10;
    ret = VerifyVersionInfoA(&info, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
        VerSetConditionMask(VerSetConditionMask(0, VER_MINORVERSION, VER_GREATER_EQUAL),
            VER_MAJORVERSION, VER_GREATER_EQUAL));
    ok(ret, "VerifyVersionInfoA failed with error %ld\n", GetLastError());

    info.wServicePackMajor = 0;
    info.wServicePackMinor = 10;
    ret = VerifyVersionInfoA(&info, VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
        VerSetConditionMask(VerSetConditionMask(0, VER_MINORVERSION, VER_GREATER_EQUAL),
            VER_MAJORVERSION, VER_GREATER_EQUAL));
    ok(ret, "VerifyVersionInfoA failed with error %ld\n", GetLastError());

    /* test bad dwOSVersionInfoSize */
    info.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXA);
    GetVersionExA((OSVERSIONINFOA *)&info);
    info.dwOSVersionInfoSize = 0;
    ret = VerifyVersionInfoA(&info, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
        VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL));
    ok(ret, "VerifyVersionInfoA failed with error %ld\n", GetLastError());
}

static void test_SystemFirmwareTable(void)
{
    static const ULONG min_sfti_len = FIELD_OFFSET(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);
    ULONG expected_len;
    UINT len;
    NTSTATUS status;
    SYSTEM_FIRMWARE_TABLE_INFORMATION *sfti;
    UCHAR *smbios_table;

    if (!pGetSystemFirmwareTable || !pEnumSystemFirmwareTables)
    {
        win_skip("SystemFirmwareTable functions not available\n");
        return;
    }

    sfti = HeapAlloc(GetProcessHeap(), 0, sizeof(*sfti));
    ok(!!sfti, "Failed to allocate memory\n");
    sfti->ProviderSignature = RSMB;
    sfti->Action = SystemFirmwareTable_Get;
    sfti->TableID = 0;
    status = pNtQuerySystemInformation(SystemFirmwareTableInformation, sfti, min_sfti_len, &expected_len);
    if (expected_len == 0) /* xp, 2003 */
    {
        win_skip("SystemFirmwareTableInformation is not available\n");
        HeapFree(GetProcessHeap(), 0, sfti);
        return;
    }
    ok( status == STATUS_BUFFER_TOO_SMALL, "NtQuerySystemInformation failed %lx\n", status );
    sfti = HeapReAlloc(GetProcessHeap(), 0, sfti, expected_len);
    status = pNtQuerySystemInformation(SystemFirmwareTableInformation, sfti, expected_len, &expected_len);
    ok( !status, "NtQuerySystemInformation failed %lx\n", status );

    expected_len -= min_sfti_len;
    ok( sfti->TableBufferLength == expected_len, "wrong len %lu/%lx\n",
        sfti->TableBufferLength, expected_len );
    len = pGetSystemFirmwareTable(RSMB, 0, NULL, 0);
    ok(len == expected_len, "Expected length %lu, got %u\n", expected_len, len);

    smbios_table = HeapAlloc(GetProcessHeap(), 0, expected_len);
    len = pGetSystemFirmwareTable(RSMB, 0, smbios_table, expected_len);
    ok(len == expected_len, "Expected length %lu, got %u\n", expected_len, len);
    ok(len == 0 || !memcmp(smbios_table, sfti->TableBuffer, 6),
       "Expected prologue %02x %02x %02x %02x %02x %02x, got %02x %02x %02x %02x %02x %02x\n",
       sfti->TableBuffer[0], sfti->TableBuffer[1], sfti->TableBuffer[2],
       sfti->TableBuffer[3], sfti->TableBuffer[4], sfti->TableBuffer[5],
       smbios_table[0], smbios_table[1], smbios_table[2],
       smbios_table[3], smbios_table[4], smbios_table[5]);
    HeapFree(GetProcessHeap(), 0, smbios_table);

    sfti->Action = SystemFirmwareTable_Enumerate;
    status = pNtQuerySystemInformation(SystemFirmwareTableInformation, sfti, min_sfti_len, &expected_len);
    ok( status == STATUS_BUFFER_TOO_SMALL, "NtQuerySystemInformation failed %lx\n", status );
    sfti = HeapReAlloc(GetProcessHeap(), 0, sfti, expected_len);
    status = pNtQuerySystemInformation(SystemFirmwareTableInformation, sfti, expected_len, &expected_len);
    ok( !status, "NtQuerySystemInformation failed %lx\n", status );
    ok( expected_len == min_sfti_len + sizeof(UINT), "wrong len %lu\n", expected_len );
    ok( sfti->TableBufferLength == sizeof(UINT), "wrong len %lu\n", sfti->TableBufferLength );
    ok( *(UINT *)sfti->TableBuffer == 0, "wrong table id %x\n", *(UINT *)sfti->TableBuffer );

    len = pEnumSystemFirmwareTables( RSMB, NULL, 0 );
    ok( len == sizeof(UINT), "wrong len %u\n", len );
    smbios_table = malloc( len );
    len = pEnumSystemFirmwareTables( RSMB, smbios_table, len );
    ok( len == sizeof(UINT), "wrong len %u\n", len );
    ok( *(UINT *)smbios_table == 0, "wrong table id %x\n", *(UINT *)smbios_table );
    free( smbios_table );

    HeapFree(GetProcessHeap(), 0, sfti);
}

#define TEST_GRAPH_HEADER_SIZE              WINE_APPX_GRAPH_BLOB_HEADER_SIZE
#define TEST_GRAPH_PACKAGE_SIZE             WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE
#define TEST_GRAPH_PACKAGE_COUNT_OFFSET     44
#define TEST_GRAPH_PACKAGES_OFFSET          48
#define TEST_GRAPH_LOADER_COUNT_OFFSET      52
#define TEST_GRAPH_LOADERS_OFFSET           56
#define TEST_GRAPH_STRINGS_OFFSET           60
#define TEST_GRAPH_STRINGS_SIZE_OFFSET      64
#define TEST_GRAPH_ACTIVATION_OFFSET        68
#define TEST_GRAPH_CLASS_COUNT_OFFSET       104
#define TEST_GRAPH_CLASSES_OFFSET           108

#define TEST_PACKAGE_FILTER_HEAD            0x00000010
#define TEST_PACKAGE_FILTER_DIRECT          0x00000020
#define TEST_PACKAGE_FILTER_RESOURCE        0x00000040
#define TEST_PACKAGE_FILTER_STATIC          0x00080000
#define TEST_PACKAGE_FILTER_DYNAMIC         0x00100000

struct test_package_info
{
    UINT32 reserved;
    UINT32 flags;
    WCHAR *path;
    WCHAR *package_full_name;
    WCHAR *package_family_name;
    PACKAGE_ID package_id;
};

struct test_graph_builder
{
    BYTE *data;
    UINT32 size;
    UINT32 capacity;
};

static void test_graph_write_u16(BYTE *data, UINT16 value)
{
    data[0] = value;
    data[1] = value >> 8;
}

static void test_graph_write_u32(BYTE *data, UINT32 value)
{
    test_graph_write_u16(data, value);
    test_graph_write_u16(data + 2, value >> 16);
}

static void test_graph_write_u64(BYTE *data, UINT64 value)
{
    test_graph_write_u32(data, value);
    test_graph_write_u32(data + 4, value >> 32);
}

static BOOL test_graph_append_string(struct test_graph_builder *builder,
        UINT32 ref_offset, const WCHAR *string)
{
    UINT32 chars = lstrlenW(string) + 1, bytes = chars * sizeof(WCHAR);

    if (builder->size > builder->capacity - bytes) return FALSE;
    test_graph_write_u32(builder->data + ref_offset, builder->size);
    test_graph_write_u32(builder->data + ref_offset + 4, chars);
    memcpy(builder->data + builder->size, string, bytes);
    builder->size += bytes;
    return TRUE;
}

static BOOL test_graph_get_image_identity(DWORD *volume_serial,
        DWORD *file_index_high, DWORD *file_index_low,
        BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE])
{
    BY_HANDLE_FILE_INFORMATION info;
    FILE_OBJECTID_BUFFER native_id;
    IO_STATUS_BLOCK io;
    WCHAR module[MAX_PATH];
    HANDLE file;
    BOOL ret;
    UINT32 i;

    if (!GetModuleFileNameW(NULL, module, ARRAY_SIZE(module))) return FALSE;
    file = CreateFileW(module, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = GetFileInformationByHandle(file, &info) &&
            !NtFsControlFile(file, 0, NULL, NULL, &io, FSCTL_GET_OBJECT_ID,
                    NULL, 0, &native_id, sizeof(native_id));
    CloseHandle(file);
    if (!ret || (!info.nFileIndexHigh && !info.nFileIndexLow)) return FALSE;
    for (i = 0; i < WINE_APPX_GRAPH_OBJECT_ID_SIZE; i++)
        if (native_id.ObjectId[i]) break;
    if (i == WINE_APPX_GRAPH_OBJECT_ID_SIZE) return FALSE;
    *volume_serial = info.dwVolumeSerialNumber;
    *file_index_high = info.nFileIndexHigh;
    *file_index_low = info.nFileIndexLow;
    memcpy(object_id, native_id.ObjectId, WINE_APPX_GRAPH_OBJECT_ID_SIZE);
    return TRUE;
}

static BYTE *build_test_package_graph(UINT32 *graph_size)
{
    static const WCHAR * const package0_strings[] =
    {
        L"TestPackage", L"CN=Test Publisher", L"", L"0abcdefghjkme",
        L"TestPackage_1.2.3.4_neutral__0abcdefghjkme",
        L"TestPackage_0abcdefghjkme", L"C:\\Packages\\TestPackage",
    };
    static const WCHAR * const package1_strings[] =
    {
        L"TestFramework", L"CN=Test Publisher", L"", L"0abcdefghjkme",
        L"TestFramework_5.6.7.8_neutral__0abcdefghjkme",
        L"TestFramework_0abcdefghjkme", L"C:\\Packages\\TestFramework",
    };
    static const WCHAR * const package2_strings[] =
    {
        L"TestTransitive", L"CN=Test Publisher", L"", L"0abcdefghjkme",
        L"TestTransitive_9.10.11.12_neutral__0abcdefghjkme",
        L"TestTransitive_0abcdefghjkme", L"C:\\Packages\\TestTransitive",
    };
    static const UINT32 package_ref_offsets[] = {56, 64, 72, 80, 88, 96, 104};
    struct test_graph_builder builder;
    BYTE *package0, *package1, *package2;
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
    DWORD volume_serial, file_index_high, file_index_low;
    UINT32 fixed_size, i;

    if (!test_graph_get_image_identity(&volume_serial, &file_index_high,
            &file_index_low, object_id))
        return NULL;
    builder.capacity = 8192;
    if (!(builder.data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, builder.capacity)))
        return NULL;
    fixed_size = TEST_GRAPH_HEADER_SIZE + 3 * TEST_GRAPH_PACKAGE_SIZE;
    builder.size = fixed_size;
    package0 = builder.data + TEST_GRAPH_HEADER_SIZE;
    package1 = package0 + TEST_GRAPH_PACKAGE_SIZE;
    package2 = package1 + TEST_GRAPH_PACKAGE_SIZE;

    if (!test_graph_append_string(&builder, 72, L"App") ||
        !test_graph_append_string(&builder, 80, L"TestPackage_0abcdefghjkme!App") ||
        !test_graph_append_string(&builder, 88, L"test.exe") ||
        !test_graph_append_string(&builder, 96, L""))
        goto failed;
    for (i = 0; i < ARRAY_SIZE(package_ref_offsets); ++i)
        if (!test_graph_append_string(&builder, TEST_GRAPH_HEADER_SIZE +
                package_ref_offsets[i], package0_strings[i]))
            goto failed;
    for (i = 0; i < ARRAY_SIZE(package_ref_offsets); ++i)
        if (!test_graph_append_string(&builder, TEST_GRAPH_HEADER_SIZE +
                TEST_GRAPH_PACKAGE_SIZE + package_ref_offsets[i], package1_strings[i]))
            goto failed;
    for (i = 0; i < ARRAY_SIZE(package_ref_offsets); ++i)
        if (!test_graph_append_string(&builder, TEST_GRAPH_HEADER_SIZE +
                2 * TEST_GRAPH_PACKAGE_SIZE + package_ref_offsets[i], package2_strings[i]))
            goto failed;

    memcpy(builder.data, "SWXGRAPH", 8);
    test_graph_write_u32(builder.data + 8, WINE_APPX_GRAPH_BLOB_VERSION);
    test_graph_write_u32(builder.data + 12, TEST_GRAPH_HEADER_SIZE);
    test_graph_write_u32(builder.data + 16, builder.size);
    test_graph_write_u32(builder.data + 40,
            WINE_APPX_GRAPH_CURRENT_ARCHITECTURE);
    test_graph_write_u32(builder.data + TEST_GRAPH_PACKAGE_COUNT_OFFSET, 3);
    test_graph_write_u32(builder.data + TEST_GRAPH_PACKAGES_OFFSET, TEST_GRAPH_HEADER_SIZE);
    test_graph_write_u32(builder.data + TEST_GRAPH_LOADER_COUNT_OFFSET, 0);
    test_graph_write_u32(builder.data + TEST_GRAPH_LOADERS_OFFSET, fixed_size);
    test_graph_write_u32(builder.data + TEST_GRAPH_CLASS_COUNT_OFFSET, 0);
    test_graph_write_u32(builder.data + TEST_GRAPH_CLASSES_OFFSET, fixed_size);
    test_graph_write_u32(builder.data +
            WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET, volume_serial);
    test_graph_write_u32(builder.data +
            WINE_APPX_GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET, file_index_high);
    test_graph_write_u32(builder.data +
            WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET, file_index_low);
    memcpy(builder.data + WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET, object_id,
            sizeof(object_id));
    test_graph_write_u32(builder.data + TEST_GRAPH_STRINGS_OFFSET, fixed_size);
    test_graph_write_u32(builder.data + TEST_GRAPH_STRINGS_SIZE_OFFSET,
            builder.size - fixed_size);
    test_graph_write_u32(builder.data + TEST_GRAPH_ACTIVATION_OFFSET, 1);

    test_graph_write_u64(package0, 1 | ((UINT64)2 << 16) |
            ((UINT64)3 << 32) | ((UINT64)4 << 48));
    test_graph_write_u32(package0 + 8, 0);
    test_graph_write_u32(package0 + 12, 0x09);
    test_graph_write_u32(package0 + 16, 0);
    test_graph_write_u64(package1, 5 | ((UINT64)6 << 16) |
            ((UINT64)7 << 32) | ((UINT64)8 << 48));
    test_graph_write_u32(package1 + 8, 0);
    test_graph_write_u32(package1 + 12,
            0x0b | (WINE_APPX_GRAPH_BLOB_VERSION > 1 ? 0x10 : 0));
    test_graph_write_u32(package1 + 16, 1);
    test_graph_write_u64(package2, 9 | ((UINT64)10 << 16) |
            ((UINT64)11 << 32) | ((UINT64)12 << 48));
    test_graph_write_u32(package2 + 8, 0);
    test_graph_write_u32(package2 + 12, 0x0b);
    test_graph_write_u32(package2 + 16, 2);

    if (!wine_appx_graph_validate_blob(builder.data, builder.size))
    {
        ok(0, "constructed package graph is invalid.\n");
        goto failed;
    }
    *graph_size = builder.size;
    return builder.data;

failed:
    HeapFree(GetProcessHeap(), 0, builder.data);
    return NULL;
}

static NTSTATUS create_remote_package_graph_process(const void *graph,
        UINT32 graph_size, HANDLE *process, HANDLE *thread)
{
    ULONG_PTR attr_buffer[offsetof(PS_ATTRIBUTE_LIST, Attributes[1]) /
            sizeof(ULONG_PTR)];
    PS_ATTRIBUTE_LIST *attr = (PS_ATTRIBUTE_LIST *)attr_buffer;
    struct wine_appx_graph_attach attach;
    unsigned long long leases[3];
    RTL_USER_PROCESS_PARAMETERS *params;
    WCHAR module[MAX_PATH], nt_path[MAX_PATH + 4], command_line[MAX_PATH + 32];
    UNICODE_STRING image, command;
    PS_CREATE_INFO create_info;
    DWORD module_length;
    HANDLE lease;
    UINT32 i;
    NTSTATUS status;

    *process = *thread = NULL;
    module_length = GetModuleFileNameW(NULL, module, ARRAY_SIZE(module));
    if (!module_length || module_length >= ARRAY_SIZE(module))
        return STATUS_UNSUCCESSFUL;
    if ((lease = CreateFileW(module, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE)
        return RtlGetLastNtStatus();
    for (i = 0; i < ARRAY_SIZE(leases); ++i)
        leases[i] = (ULONG_PTR)lease;
    lstrcpyW(nt_path, L"\\??\\");
    lstrcatW(nt_path, module);
    swprintf(command_line, ARRAY_SIZE(command_line), L"\"%s\" version graph-remote-child",
            module);
    RtlInitUnicodeString(&image, nt_path);
    RtlInitUnicodeString(&command, command_line);
    status = RtlCreateProcessParametersEx(&params, &image, NULL, NULL, &command,
            NULL, NULL, NULL, NULL, NULL, PROCESS_PARAMS_FLAG_NORMALIZED);
    if (status)
    {
        CloseHandle(lease);
        return status;
    }

    attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
    attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
    attach.size = graph_size;
    attach.reserved = 0;
    attach.blob = (ULONG_PTR)graph;
    attach.leases = (ULONG_PTR)leases;
    attach.lease_count = ARRAY_SIZE(leases);
    attach.lease_reserved = 0;
    params->PackageDependencyData = &attach;

    memset(attr, 0, sizeof(attr_buffer));
    attr->TotalLength = offsetof(PS_ATTRIBUTE_LIST, Attributes[1]);
    attr->Attributes[0].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
    attr->Attributes[0].Size = image.Length;
    attr->Attributes[0].ValuePtr = image.Buffer;

    memset(&create_info, 0, sizeof(create_info));
    create_info.Size = sizeof(create_info);
    status = NtCreateUserProcess(process, thread, PROCESS_ALL_ACCESS,
            THREAD_ALL_ACCESS, NULL, NULL, 0,
            THREAD_CREATE_FLAGS_CREATE_SUSPENDED, params, &create_info, attr);
    RtlDestroyProcessParameters(params);
    CloseHandle(lease);
    return status;
}

static void test_current_package_graph(void)
{
    static const PackagePathType unavailable_path_types[] =
    {
        PackagePathType_Mutable,
        PackagePathType_MachineExternal,
        PackagePathType_UserExternal,
        PackagePathType_EffectiveExternal,
    };
    RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
    void *original_graph = params->PackageDependencyData;
    struct test_package_info *info;
    LARGE_INTEGER frequency, start, end;
    BYTE *graph = NULL, *corrupt_graph = NULL, *id_buffer = NULL, *info_buffer = NULL;
    BYTE zero_capacity_buffer[1];
    WCHAR string_buffer[256];
    PACKAGE_ID *id;
    HANDLE process = NULL, process_thread = NULL, restricted = NULL, self;
    void *noaccess = NULL;
    UINT32 graph_size, length, size, count, original_size, original_count;
    LONG ret = ERROR_SUCCESS;
    unsigned int i;

    if (strcmp(winetest_platform, "wine"))
    {
        skip("Wine package graph wire-format tests are not applicable on Windows.\n");
        return;
    }
    if (!pGetCurrentApplicationUserModelId || !pGetCurrentPackageFamilyName ||
        !pGetCurrentPackageFullName || !pGetCurrentPackageId ||
        !pGetCurrentPackageInfo || !pGetCurrentPackageInfo2 ||
        !pGetCurrentPackagePath || !pGetCurrentPackagePath2 ||
        !pGetPackageFamilyName || !pGetPackageFullName)
    {
        win_skip("Package graph query APIs are unavailable.\n");
        return;
    }

    params->PackageDependencyData = NULL;
    ret = pGetCurrentPackageFullName((UINT32 *)1, (WCHAR *)1);
    ok(ret == APPMODEL_ERROR_NO_PACKAGE, "Got unexpected no-package ret %ld.\n", ret);
    ret = pGetCurrentPackageFamilyName((UINT32 *)1, (WCHAR *)1);
    ok(ret == APPMODEL_ERROR_NO_PACKAGE, "Got unexpected no-package ret %ld.\n", ret);
    ret = pGetCurrentApplicationUserModelId((UINT32 *)1, (WCHAR *)1);
    ok(ret == APPMODEL_ERROR_NO_APPLICATION,
            "Got unexpected no-application ret %ld.\n", ret);
    ret = pGetCurrentPackageId((UINT32 *)1, (BYTE *)1);
    ok(ret == APPMODEL_ERROR_NO_PACKAGE, "Got unexpected no-package ret %ld.\n", ret);
    ret = pGetCurrentPackageInfo(~0u, (UINT32 *)1, (BYTE *)1, (UINT32 *)1);
    ok(ret == APPMODEL_ERROR_NO_PACKAGE, "Got unexpected no-package ret %ld.\n", ret);
    ret = pGetCurrentPackagePath((UINT32 *)1, (WCHAR *)1);
    ok(ret == APPMODEL_ERROR_NO_PACKAGE, "Got unexpected no-package ret %ld.\n", ret);
    ret = pGetCurrentPackageInfo2(~0u, (PackagePathType)~0u,
            (UINT32 *)1, (BYTE *)1, (UINT32 *)1);
    ok(ret == APPMODEL_ERROR_NO_PACKAGE,
            "Got unexpected v2 package-info no-package ret %ld.\n", ret);
    ret = pGetCurrentPackagePath2((PackagePathType)~0u,
            (UINT32 *)1, (WCHAR *)1);
    ok(ret == APPMODEL_ERROR_NO_PACKAGE,
            "Got unexpected v2 path no-package ret %ld.\n", ret);

    graph = build_test_package_graph(&graph_size);
    ok(!!graph, "Failed to build the test package graph.\n");
    if (!graph) goto done;
    params->PackageDependencyData = graph;

    length = 0;
    ret = pGetCurrentPackageFullName(&length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got full-name ret %ld.\n", ret);
    ok(length == lstrlenW(L"TestPackage_1.2.3.4_neutral__0abcdefghjkme") + 1,
            "Got full-name length %u.\n", length);
    original_size = length;
    length--;
    string_buffer[0] = 0xcccc;
    ret = pGetCurrentPackageFullName(&length, string_buffer);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got undersized full-name ret %ld.\n", ret);
    ok(length == original_size, "Got undersized full-name length %u.\n", length);
    ok(string_buffer[0] == 0xcccc, "Undersized call modified the buffer.\n");
    length = ARRAY_SIZE(string_buffer);
    ret = pGetCurrentPackageFullName(&length, string_buffer);
    ok(ret == ERROR_SUCCESS, "Got full-name ret %ld.\n", ret);
    ok(length == original_size, "Got oversized full-name length %u.\n", length);
    ok(!lstrcmpW(string_buffer, L"TestPackage_1.2.3.4_neutral__0abcdefghjkme"),
            "Got full name %s.\n", debugstr_w(string_buffer));

    length = ARRAY_SIZE(string_buffer);
    ret = pGetCurrentPackageFamilyName(&length, string_buffer);
    ok(ret == ERROR_SUCCESS, "Got family-name ret %ld.\n", ret);
    ok(!lstrcmpW(string_buffer, L"TestPackage_0abcdefghjkme"),
            "Got family name %s.\n", debugstr_w(string_buffer));
    length = ARRAY_SIZE(string_buffer);
    ret = pGetCurrentApplicationUserModelId(&length, string_buffer);
    ok(ret == ERROR_SUCCESS, "Got AUMID ret %ld.\n", ret);
    ok(!lstrcmpW(string_buffer, L"TestPackage_0abcdefghjkme!App"),
            "Got AUMID %s.\n", debugstr_w(string_buffer));
    length = ARRAY_SIZE(string_buffer);
    ret = pGetCurrentPackagePath(&length, string_buffer);
    ok(ret == ERROR_SUCCESS, "Got path ret %ld.\n", ret);
    ok(!lstrcmpW(string_buffer, L"C:\\Packages\\TestPackage"),
            "Got package path %s.\n", debugstr_w(string_buffer));
    length = ARRAY_SIZE(string_buffer);
    ret = pGetCurrentPackagePath2(PackagePathType_Install,
            &length, string_buffer);
    ok(ret == ERROR_SUCCESS, "Got v2 install path ret %ld.\n", ret);
    ok(!lstrcmpW(string_buffer, L"C:\\Packages\\TestPackage"),
            "Got v2 install path %s.\n", debugstr_w(string_buffer));
    length = ARRAY_SIZE(string_buffer);
    ret = pGetCurrentPackagePath2(PackagePathType_Effective,
            &length, string_buffer);
    ok(ret == ERROR_SUCCESS, "Got v2 effective path ret %ld.\n", ret);
    ok(!lstrcmpW(string_buffer, L"C:\\Packages\\TestPackage"),
            "Got v2 effective path %s.\n", debugstr_w(string_buffer));
    for (i = 0; i < ARRAY_SIZE(unavailable_path_types); i++)
    {
        length = 0x12345678;
        string_buffer[0] = 0xcccc;
        ret = pGetCurrentPackagePath2(unavailable_path_types[i],
                &length, string_buffer);
        ok(ret == ERROR_NOT_FOUND,
                "Path type %u returned %ld.\n",
                unavailable_path_types[i], ret);
        ok(length == 0x12345678,
                "Path type %u changed length to %u.\n",
                unavailable_path_types[i], length);
        ok(string_buffer[0] == 0xcccc,
                "Path type %u modified the output.\n",
                unavailable_path_types[i]);
    }
    length = 0x12345678;
    string_buffer[0] = 0xcccc;
    ret = pGetCurrentPackagePath2((PackagePathType)~0u,
            &length, string_buffer);
    ok(ret == ERROR_INVALID_PARAMETER,
            "Invalid path type returned %ld.\n", ret);
    ok(length == 0x12345678 && string_buffer[0] == 0xcccc,
            "Invalid path type modified the output.\n");

    size = 0;
    ret = pGetCurrentPackageId(&size, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got package-id probe ret %ld.\n", ret);
    ok(size > sizeof(*id), "Got package-id size %u.\n", size);
    original_size = size;
    zero_capacity_buffer[0] = 0xcc;
    size--;
    ret = pGetCurrentPackageId(&size, zero_capacity_buffer);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got undersized package-id ret %ld.\n", ret);
    ok(size == original_size, "Got undersized package-id size %u.\n", size);
    ok(zero_capacity_buffer[0] == 0xcc, "Undersized package-id call modified the buffer.\n");
    id_buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    ok(!!id_buffer, "Failed to allocate package-id buffer.\n");
    if (id_buffer)
    {
        original_size = size;
        ret = pGetCurrentPackageId(&size, id_buffer);
        ok(ret == ERROR_SUCCESS, "Got package-id ret %ld.\n", ret);
        ok(size == original_size, "Got package-id size %u.\n", size);
        id = (PACKAGE_ID *)id_buffer;
        ok(id->reserved == 0, "Got package-id reserved %#x.\n", id->reserved);
        ok(id->processorArchitecture == PROCESSOR_ARCHITECTURE_NEUTRAL,
                "Got package architecture %u.\n", id->processorArchitecture);
        ok(id->version.Version == 0x0001000200030004,
                "Got package version %s.\n", wine_dbgstr_longlong(id->version.Version));
        ok(!lstrcmpW(id->name, L"TestPackage"), "Got package name %s.\n",
                debugstr_w(id->name));
        ok(!lstrcmpW(id->publisher, L"CN=Test Publisher"), "Got publisher %s.\n",
                debugstr_w(id->publisher));
        ok(!lstrcmpW(id->resourceId, L""), "Got resource id %s.\n",
                debugstr_w(id->resourceId));
        ok(!lstrcmpW(id->publisherId, L"0abcdefghjkme"), "Got publisher id %s.\n",
                debugstr_w(id->publisherId));
    }

    size = count = 0;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_STATIC, &size, NULL, &count);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got package-info probe ret %ld.\n", ret);
    ok(count == 3, "Got package-info count %u.\n", count);
    ok(size >= 3 * sizeof(*info), "Got package-info size %u.\n", size);
    info_buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    ok(!!info_buffer, "Failed to allocate package-info buffer.\n");
    if (info_buffer)
    {
        original_size = size;
        count = 0;
        ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_STATIC, &size,
                info_buffer, &count);
        ok(ret == ERROR_SUCCESS, "Got package-info ret %ld.\n", ret);
        ok(size == original_size, "Got package-info size %u.\n", size);
        ok(count == 3, "Got package-info count %u.\n", count);
        info = (struct test_package_info *)info_buffer;
        ok(!info[0].reserved && !info[0].flags,
                "Got head flags %#x, reserved %#x.\n", info[0].flags, info[0].reserved);
        ok((info[1].flags & 0x00080001) == 0x00080001,
                "Got dependency flags %#x.\n", info[1].flags);
        ok(!lstrcmpW(info[0].path, L"C:\\Packages\\TestPackage"),
                "Got info path %s.\n", debugstr_w(info[0].path));
        ok(!lstrcmpW(info[0].package_full_name,
                L"TestPackage_1.2.3.4_neutral__0abcdefghjkme"),
                "Got info full name %s.\n", debugstr_w(info[0].package_full_name));
        ok(!lstrcmpW(info[0].package_family_name, L"TestPackage_0abcdefghjkme"),
                "Got info family name %s.\n", debugstr_w(info[0].package_family_name));
        ok(!lstrcmpW(info[1].package_id.name, L"TestFramework"),
                "Got dependency name %s.\n", debugstr_w(info[1].package_id.name));
        ok(!lstrcmpW(info[2].package_id.name, L"TestTransitive"),
                "Got transitive name %s.\n", debugstr_w(info[2].package_id.name));
        ok((BYTE *)info[0].path >= info_buffer + 3 * sizeof(*info) &&
                (BYTE *)info[0].path < info_buffer + size,
                "Got out-of-range path pointer %p.\n", info[0].path);
    }

    size = count = 0;
    ret = pGetCurrentPackageInfo2(TEST_PACKAGE_FILTER_STATIC,
            PackagePathType_Install, &size, NULL, &count);
    ok(ret == ERROR_INSUFFICIENT_BUFFER,
            "Got v2 install package-info probe ret %ld.\n", ret);
    ok(count == 3, "Got v2 install package-info count %u.\n", count);
    original_size = size;
    size = count = 0;
    ret = pGetCurrentPackageInfo2(TEST_PACKAGE_FILTER_STATIC,
            PackagePathType_Effective, &size, NULL, &count);
    ok(ret == ERROR_INSUFFICIENT_BUFFER,
            "Got v2 effective package-info probe ret %ld.\n", ret);
    ok(size == original_size && count == 3,
            "Got v2 effective size %u and count %u.\n", size, count);
    if (info_buffer && size <= original_size)
    {
        ret = pGetCurrentPackageInfo2(TEST_PACKAGE_FILTER_STATIC,
                PackagePathType_Effective, &size, info_buffer, &count);
        ok(ret == ERROR_SUCCESS,
                "Got v2 effective package-info ret %ld.\n", ret);
        info = (struct test_package_info *)info_buffer;
        ok(!lstrcmpW(info[0].path, L"C:\\Packages\\TestPackage"),
                "Got v2 effective info path %s.\n",
                debugstr_w(info[0].path));
    }
    for (i = 0; i < ARRAY_SIZE(unavailable_path_types); i++)
    {
        size = 0x12345678;
        count = 0x87654321;
        ret = pGetCurrentPackageInfo2(TEST_PACKAGE_FILTER_STATIC,
                unavailable_path_types[i], &size, info_buffer, &count);
        ok(ret == ERROR_NOT_FOUND,
                "Info path type %u returned %ld.\n",
                unavailable_path_types[i], ret);
        ok(size == 0x12345678 && count == 0x87654321,
                "Info path type %u modified outputs to %u and %u.\n",
                unavailable_path_types[i], size, count);
    }
    size = 0x12345678;
    count = 0x87654321;
    ret = pGetCurrentPackageInfo2(TEST_PACKAGE_FILTER_STATIC,
            (PackagePathType)~0u, &size, info_buffer, &count);
    ok(ret == ERROR_INVALID_PARAMETER,
            "Invalid info path type returned %ld.\n", ret);
    ok(size == 0x12345678 && count == 0x87654321,
            "Invalid info path type modified outputs to %u and %u.\n",
            size, count);

    zero_capacity_buffer[0] = 0xcc;
    size = 0;
    count = 0xdeadbeef;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_STATIC, &size,
            zero_capacity_buffer, &count);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got zero-capacity ret %ld.\n", ret);
    ok(size >= 3 * sizeof(*info), "Got zero-capacity required size %u.\n", size);
    ok(count == 3, "Got zero-capacity count %u.\n", count);
    ok(zero_capacity_buffer[0] == 0xcc, "Zero-capacity call modified the buffer.\n");

    size = 0;
    count = 0;
    ret = pGetCurrentPackageInfo(0, &size, NULL, &count);
    if (WINE_APPX_GRAPH_BLOB_VERSION == 1)
        ok(ret == ERROR_NOT_SUPPORTED, "Got v1 ALL_LOADED ret %ld.\n", ret);
    else
        ok(ret == ERROR_INSUFFICIENT_BUFFER && count == 2,
                "Got ALL_LOADED ret %ld, count %u.\n", ret, count);
    size = 0;
    count = 0;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_HEAD, &size, NULL, &count);
    ok(ret == ERROR_INSUFFICIENT_BUFFER && count == 1,
            "Got HEAD ret %ld, count %u.\n", ret, count);
    size = 0;
    count = 0;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_DIRECT, &size, NULL, &count);
    if (WINE_APPX_GRAPH_BLOB_VERSION == 1)
        ok(ret == ERROR_NOT_SUPPORTED, "Got v1 DIRECT ret %ld.\n", ret);
    else
        ok(ret == ERROR_INSUFFICIENT_BUFFER && count == 1,
                "Got DIRECT ret %ld, count %u.\n", ret, count);
    size = 0;
    count = 0;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_HEAD | TEST_PACKAGE_FILTER_DIRECT,
            &size, NULL, &count);
    if (WINE_APPX_GRAPH_BLOB_VERSION == 1)
        ok(ret == ERROR_NOT_SUPPORTED, "Got v1 HEAD|DIRECT ret %ld.\n", ret);
    else
        ok(ret == ERROR_INSUFFICIENT_BUFFER && count == 2,
                "Got HEAD|DIRECT ret %ld, count %u.\n", ret, count);
    size = 0xdeadbeef;
    count = 0xdeadbeef;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_RESOURCE, &size, NULL, &count);
    ok(ret == ERROR_SUCCESS && !size && !count,
            "Got RESOURCE ret %ld, size %u, count %u.\n", ret, size, count);
    size = 0;
    count = 0;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_STATIC, &size, NULL, &count);
    ok(ret == ERROR_INSUFFICIENT_BUFFER && count == 3,
            "Got STATIC ret %ld, count %u.\n", ret, count);
    size = 0xdeadbeef;
    count = 0xdeadbeef;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_DYNAMIC, &size, NULL, &count);
    ok(ret == ERROR_SUCCESS && !size && !count,
            "Got DYNAMIC ret %ld, size %u, count %u.\n", ret, size, count);
    size = 0;
    count = 0;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_STATIC |
            TEST_PACKAGE_FILTER_DYNAMIC, &size, NULL, &count);
    ok(ret == ERROR_INSUFFICIENT_BUFFER && count == 3,
            "Got STATIC|DYNAMIC ret %ld, count %u.\n", ret, count);
    original_size = 0x12345678;
    original_count = 0x87654321;
    size = original_size;
    count = original_count;
    ret = pGetCurrentPackageInfo(0x80000000, &size, NULL, &count);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unknown-filter ret %ld.\n", ret);
    ok(size == original_size && count == original_count,
            "Unknown filter modified outputs to %u, %u.\n", size, count);
    count = original_count;
    ret = pGetCurrentPackageInfo(0, NULL, NULL, &count);
    ok(ret == ERROR_INVALID_PARAMETER, "Got NULL size ret %ld.\n", ret);
    ok(count == original_count, "NULL size modified count to %u.\n", count);
    size = 0;
    ret = pGetCurrentPackageInfo(TEST_PACKAGE_FILTER_STATIC, &size, NULL, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER && size,
            "Got NULL count ret %ld, size %u.\n", ret, size);

    length = ARRAY_SIZE(string_buffer);
    ret = pGetPackageFullName(GetCurrentProcess(), &length, string_buffer);
    ok(ret == ERROR_SUCCESS, "Got self process full-name ret %ld.\n", ret);
    self = NULL;
    ok(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
            &self, PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0),
            "DuplicateHandle failed, error %lu.\n", GetLastError());
    if (self)
    {
        length = ARRAY_SIZE(string_buffer);
        ret = pGetPackageFamilyName(self, &length, string_buffer);
        /*
         * Non-pseudo handles use the immutable server snapshot.  This test
         * graph was installed only into the local PEB, so it must not leak
         * across the remote-process query boundary.
         */
        ok(ret == APPMODEL_ERROR_NO_PACKAGE,
                "Got duplicated-self family-name ret %ld.\n", ret);
        CloseHandle(self);
    }

    ret = create_remote_package_graph_process(graph, graph_size, &process,
            &process_thread);
    ok(!ret, "Failed to create packaged query process, status %#lx.\n", ret);
    if (!ret)
    {
        length = 0;
        ret = pGetPackageFullName(process, &length, NULL);
        ok(ret == ERROR_INSUFFICIENT_BUFFER,
                "Got remote full-name probe ret %ld.\n", ret);
        ok(length == lstrlenW(L"TestPackage_1.2.3.4_neutral__0abcdefghjkme") + 1,
                "Got remote full-name length %u.\n", length);
        length = ARRAY_SIZE(string_buffer);
        ret = pGetPackageFullName(process, &length, string_buffer);
        ok(ret == ERROR_SUCCESS, "Got remote full-name ret %ld.\n", ret);
        ok(!lstrcmpW(string_buffer,
                L"TestPackage_1.2.3.4_neutral__0abcdefghjkme"),
                "Got remote full name %s.\n", debugstr_w(string_buffer));
        length = ARRAY_SIZE(string_buffer);
        ret = pGetPackageFamilyName(process, &length, string_buffer);
        ok(ret == ERROR_SUCCESS, "Got remote family-name ret %ld.\n", ret);
        ok(!lstrcmpW(string_buffer, L"TestPackage_0abcdefghjkme"),
                "Got remote family name %s.\n", debugstr_w(string_buffer));

        ok(DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(),
                &restricted, SYNCHRONIZE, FALSE, 0),
                "Failed to create restricted process handle, error %lu.\n",
                GetLastError());
        if (restricted)
        {
            length = ARRAY_SIZE(string_buffer);
            ret = pGetPackageFullName(restricted, &length, string_buffer);
            ok(ret == ERROR_ACCESS_DENIED,
                    "Restricted remote query returned %ld.\n", ret);
            CloseHandle(restricted);
            restricted = NULL;
        }

        NtTerminateProcess(process, 0);
        ok(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0,
                "Timed out terminating packaged query process.\n");
        length = ARRAY_SIZE(string_buffer);
        ret = pGetPackageFamilyName(process, &length, string_buffer);
        ok(ret == ERROR_SUCCESS,
                "Exited remote family-name query returned %ld.\n", ret);
        CloseHandle(process_thread);
        process_thread = NULL;
        CloseHandle(process);
        process = NULL;
    }

    length = ARRAY_SIZE(string_buffer);
    ret = pGetPackageFullName((HANDLE)(ULONG_PTR)0xdeadbeef, &length,
            string_buffer);
    ok(ret == ERROR_INVALID_HANDLE, "Invalid remote handle returned %ld.\n", ret);

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (i = 0; i < 10000; ++i)
    {
        length = ARRAY_SIZE(string_buffer);
        ret = pGetCurrentPackageFullName(&length, string_buffer);
        if (ret) break;
    }
    QueryPerformanceCounter(&end);
    ok(i == 10000, "Repeated package query failed at iteration %u, ret %ld.\n", i, ret);
    trace("10000 cached package identity queries took %.3f ms.\n",
            (double)(end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart);
    ok(end.QuadPart - start.QuadPart < frequency.QuadPart * 2,
            "Cached package queries took over two seconds.\n");

    corrupt_graph = HeapAlloc(GetProcessHeap(), 0, graph_size);
    ok(!!corrupt_graph, "Failed to allocate corrupt graph.\n");
    if (corrupt_graph)
    {
        memcpy(corrupt_graph, graph, graph_size);
        test_graph_write_u32(corrupt_graph + TEST_GRAPH_HEADER_SIZE + 88, graph_size - 1);
        params->PackageDependencyData = corrupt_graph;
        length = ARRAY_SIZE(string_buffer);
        ret = pGetCurrentPackageFullName(&length, string_buffer);
        ok(ret == APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT,
                "Got corrupt graph ret %ld.\n", ret);
    }

    noaccess = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    ok(!!noaccess, "VirtualAlloc failed, error %lu.\n", GetLastError());
    if (noaccess)
    {
        params->PackageDependencyData = noaccess;
        length = ARRAY_SIZE(string_buffer);
        ret = pGetCurrentPackageFullName(&length, string_buffer);
        ok(ret == APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT,
                "Got inaccessible graph ret %ld.\n", ret);
    }

done:
    if (process)
    {
        NtTerminateProcess(process, STATUS_UNSUCCESSFUL);
        WaitForSingleObject(process, 5000);
    }
    if (restricted) CloseHandle(restricted);
    if (process_thread) CloseHandle(process_thread);
    if (process) CloseHandle(process);
    params->PackageDependencyData = original_graph;
    if (noaccess) VirtualFree(noaccess, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, info_buffer);
    HeapFree(GetProcessHeap(), 0, id_buffer);
    HeapFree(GetProcessHeap(), 0, corrupt_graph);
    HeapFree(GetProcessHeap(), 0, graph);
}

static const struct
{
    UINT32 code;
    const WCHAR *name;
    BOOL broken;
}
arch_data[] =
{
    {PROCESSOR_ARCHITECTURE_INTEL,   L"X86"},
    {PROCESSOR_ARCHITECTURE_ARM,     L"Arm"},
    {PROCESSOR_ARCHITECTURE_AMD64,   L"X64"},
    {PROCESSOR_ARCHITECTURE_NEUTRAL, L"Neutral"},
    {PROCESSOR_ARCHITECTURE_ARM64,   L"Arm64",   TRUE /* Before Win10. */},
    {PROCESSOR_ARCHITECTURE_IA32_ON_ARM64, L"X86A64", TRUE /* Older Win10. */},
    {PROCESSOR_ARCHITECTURE_UNKNOWN, L"Unknown", TRUE /* Before Win10 1709. */},
};

static const WCHAR *arch_string_from_code(UINT32 arch)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(arch_data); ++i)
        if (arch_data[i].code == arch)
            return arch_data[i].name;

    return NULL;
}

static unsigned int get_package_str_size(const WCHAR *str)
{
    return str ? (lstrlenW(str) + 1) * sizeof(*str) : 0;
}

static unsigned int get_package_id_size(const PACKAGE_ID *id)
{
    return sizeof(*id) + get_package_str_size(id->name)
            + get_package_str_size(id->publisher)
            + get_package_str_size(id->resourceId) + 14 * sizeof(WCHAR);
}

static void packagefullname_from_packageid(WCHAR *buffer, size_t count, const PACKAGE_ID *id)
{
    swprintf(buffer, count, L"%s_%u.%u.%u.%u_%s_%s_%s", id->name, id->version.Major,
            id->version.Minor, id->version.Build, id->version.Revision,
            arch_string_from_code(id->processorArchitecture), id->resourceId,
            id->publisherId);
}

static BOOL bytes_are_value(const void *buffer, SIZE_T size, BYTE value)
{
    const BYTE *bytes = buffer;
    SIZE_T i;

    for (i = 0; i < size; i++)
        if (bytes[i] != value) return FALSE;
    return TRUE;
}

static void test_package_identity_names(void)
{
    static const WCHAR publisher[] =
            L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US";
    static const WCHAR family[] = L"Microsoft.WindowsStore_8wekyb3d8bbwe";
    static const WCHAR full_name[] =
            L"Microsoft.WindowsStore_1.2.3.4_x64__8wekyb3d8bbwe";
    static const WCHAR missing_family[] = L"Wine.NoSuch.Contract_0000000000000";
    static const WCHAR missing_full_name[] =
            L"Wine.NoSuch.Contract_1.0.0.0_neutral__0000000000000";
    PACKAGE_ID id =
    {
        0, PROCESSOR_ARCHITECTURE_AMD64,
        {{.Major = 1, .Minor = 2, .Build = 3, .Revision = 4}},
        (WCHAR *)L"Microsoft.WindowsStore", (WCHAR *)publisher,
        NULL, NULL
    };
    WCHAR buffer[256], second[64];
    WCHAR *names[2] = {(WCHAR *)0xdeadbeef, (WCHAR *)0xdeadbeef};
    UINT32 length, second_length, required, count, buffer_length;
    LONG ret;

    if (!pPackageFamilyNameFromFullName || !pPackageFamilyNameFromId ||
        !pPackageFullNameFromId || !pPackageNameAndPublisherIdFromFamilyName)
    {
        win_skip("Package identity name APIs are unavailable.\n");
        return;
    }

    length = 0;
    ret = pPackageFamilyNameFromFullName(full_name, &length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    required = lstrlenW(family) + 1;
    ok(length == required, "Got length %u, expected %u.\n", length, required);

    memset(buffer, 0xcc, sizeof(buffer));
    length = required - 1;
    ret = pPackageFamilyNameFromFullName(full_name, &length, buffer);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    ok(length == required, "Got length %u, expected %u.\n", length, required);
    ok(bytes_are_value(buffer, sizeof(buffer), 0xcc), "Output was partially written.\n");

    length = ARRAY_SIZE(buffer);
    ret = pPackageFamilyNameFromFullName(full_name, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got unexpected ret %ld.\n", ret);
    ok(length == required, "Got length %u, expected %u.\n", length, required);
    ok(!lstrcmpW(buffer, family), "Got family %s.\n", debugstr_w(buffer));

    length = 0;
    ret = pPackageFamilyNameFromId(&id, &length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    ok(length == required, "Got length %u, expected %u.\n", length, required);
    length = ARRAY_SIZE(buffer);
    ret = pPackageFamilyNameFromId(&id, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got unexpected ret %ld.\n", ret);
    ok(!lstrcmpW(buffer, family), "Got family %s.\n", debugstr_w(buffer));

    length = 0;
    ret = pPackageFullNameFromId(&id, &length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    required = lstrlenW(full_name) + 1;
    ok(length == required, "Got length %u, expected %u.\n", length, required);
    length = ARRAY_SIZE(buffer);
    ret = pPackageFullNameFromId(&id, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got unexpected ret %ld.\n", ret);
    ok(!lstrcmpW(buffer, full_name), "Got full name %s.\n", debugstr_w(buffer));

    id.publisherId = (WCHAR *)L"0000000000000";
    memset(buffer, 0xcc, sizeof(buffer));
    length = ARRAY_SIZE(buffer);
    ret = pPackageFullNameFromId(&id, &length, buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);
    ok(length == ARRAY_SIZE(buffer), "Length changed to %u.\n", length);
    ok(bytes_are_value(buffer, sizeof(buffer), 0xcc), "Invalid output was written.\n");
    id.publisher = NULL;
    id.publisherId = (WCHAR *)L"8WEKYB3D8BBWE";
    id.processorArchitecture = PROCESSOR_ARCHITECTURE_IA32_ON_ARM64;
    id.resourceId = (WCHAR *)L"en-US";
    length = ARRAY_SIZE(buffer);
    ret = pPackageFullNameFromId(&id, &length, buffer);
    ok(ret == ERROR_SUCCESS || broken(ret == ERROR_INVALID_PARAMETER),
            "Got unexpected ret %ld.\n", ret);
    if (!ret)
        ok(!lstrcmpW(buffer,
                L"Microsoft.WindowsStore_1.2.3.4_x86a64_en-US_8WEKYB3D8BBWE"),
                "Got full name %s.\n", debugstr_w(buffer));

    length = second_length = 0;
    ret = pPackageNameAndPublisherIdFromFamilyName(family, &length, NULL,
            &second_length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    ok(length == lstrlenW(id.name) + 1, "Got name length %u.\n", length);
    ok(second_length == 14, "Got publisher length %u.\n", second_length);

    memset(buffer, 0xcc, sizeof(buffer));
    memset(second, 0xcc, sizeof(second));
    length--;
    ret = pPackageNameAndPublisherIdFromFamilyName(family, &length, buffer,
            &second_length, second);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    ok(bytes_are_value(buffer, sizeof(buffer), 0xcc), "Name was partially written.\n");
    ok(bytes_are_value(second, sizeof(second), 0xcc), "Publisher ID was partially written.\n");

    length = ARRAY_SIZE(buffer);
    second_length = ARRAY_SIZE(second);
    ret = pPackageNameAndPublisherIdFromFamilyName(family, &length, buffer,
            &second_length, second);
    ok(ret == ERROR_SUCCESS, "Got unexpected ret %ld.\n", ret);
    ok(!lstrcmpW(buffer, L"Microsoft.WindowsStore"), "Got name %s.\n", debugstr_w(buffer));
    ok(!lstrcmpW(second, L"8wekyb3d8bbwe"), "Got publisher ID %s.\n", debugstr_w(second));

    if (pGetPackagesByPackageFamily)
    {
        count = ARRAY_SIZE(names);
        buffer_length = ARRAY_SIZE(buffer);
        ret = pGetPackagesByPackageFamily(missing_family, &count, names,
                &buffer_length, buffer);
        ok(ret == ERROR_SUCCESS, "Got unexpected ret %ld.\n", ret);
        ok(!count, "Got package count %u.\n", count);
        ok(!buffer_length, "Got buffer length %u.\n", buffer_length);

        count = buffer_length = 0x12345678;
        ret = pGetPackagesByPackageFamily(L"invalid", &count, names,
                &buffer_length, buffer);
        ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);
        ok(count == 0x12345678, "Count changed to %u.\n", count);
        ok(buffer_length == 0x12345678, "Buffer length changed to %u.\n", buffer_length);
    }

    if (pGetPackagePathByFullName)
    {
        length = 0;
        ret = pGetPackagePathByFullName(missing_full_name, &length, NULL);
        ok(ret == ERROR_NOT_FOUND, "Got unexpected ret %ld.\n", ret);
        length = 0x12345678;
        ret = pGetPackagePathByFullName(L"invalid", &length, NULL);
        ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);
        ok(length == 0x12345678, "Length changed to %u.\n", length);
    }

    if (pGetPackagePathByFullName2)
    {
        length = 0;
        ret = pGetPackagePathByFullName2(missing_full_name,
                PackagePathType_Install, &length, NULL);
        ok(ret == ERROR_NOT_FOUND,
                "Got v2 install path ret %ld.\n", ret);
        ret = pGetPackagePathByFullName2(missing_full_name,
                PackagePathType_Effective, &length, NULL);
        ok(ret == ERROR_NOT_FOUND,
                "Got v2 effective path ret %ld.\n", ret);
        ret = pGetPackagePathByFullName2(missing_full_name,
                PackagePathType_Mutable, &length, NULL);
        ok(ret == ERROR_NOT_FOUND,
                "Got v2 mutable path ret %ld.\n", ret);

        length = 0x12345678;
        memset(buffer, 0xcc, sizeof(buffer));
        ret = pGetPackagePathByFullName2(missing_full_name,
                (PackagePathType)~0u, &length, buffer);
        ok(ret == ERROR_INVALID_PARAMETER,
                "Got invalid v2 path type ret %ld.\n", ret);
        ok(length == 0x12345678,
                "Invalid v2 path type changed length to %u.\n", length);
        ok(bytes_are_value(buffer, sizeof(buffer), 0xcc),
                "Invalid v2 path type partially wrote output.\n");
    }
}

static void test_PackageIdFromFullName(void)
{
    static const PACKAGE_ID test_package_id =
    {
        0, PROCESSOR_ARCHITECTURE_INTEL,
                {{.Major = 1, .Minor = 2, .Build = 3, .Revision = 4}},
                (WCHAR *)L"TestPackage", NULL,
                (WCHAR *)L"TestResourceId", (WCHAR *)L"0abcdefghjkme"
    };
    static const WCHAR * const invalid_full_names[] =
    {
        L"Ab_1.2.3.4_x86__0abcdefghjkme",
        L"CON.package_1.2.3.4_x86__0abcdefghjkme",
        L"Wine._1.2.3.4_x86__0abcdefghjkme",
        L"Wine.xn--bad_1.2.3.4_x86__0abcdefghjkme",
        L"Wine_bad_1.2.3.4_x86__0abcdefghjkme",
        L"Wine.Package_1.2.3_x86__0abcdefghjkme",
        L"Wine.Package_65536.2.3.4_x86__0abcdefghjkme",
        L"Wine.Package_1..3.4_x86__0abcdefghjkme",
        L"Wine.Package_1.2.3.4.5_x86__0abcdefghjkme",
        L"Wine.Package_1.2.3.4_unknown__0abcdefghjkme",
        L"Wine.Package_1.2.3.4_x86_abcdefghijklmnopqrstuvwxyz12345_0abcdefghjkme",
        L"Wine.Package_1.2.3.4_x86__0abcdefgijkme",
        L"Wine.Package_1.2.3.4_x86__0abcdefghjkm",
        L"Wine.Package_1.2.3.4_x86__0abcdefghjkme_suffix",
    };
    static const WCHAR missing_full_name[] =
            L"Wine.NoSuch.Contract_1.0.0.0_neutral__0000000000000";
    UINT32 size, expected_size;
    PACKAGE_ID test_id;
    WCHAR fullname[512];
    BYTE id_buffer[512];
    unsigned int i;
    PACKAGE_ID *id;
    LONG ret;

    if (!pPackageIdFromFullName)
    {
        win_skip("PackageIdFromFullName not available.\n");
        return;
    }

    packagefullname_from_packageid(fullname, ARRAY_SIZE(fullname), &test_package_id);

    id = (PACKAGE_ID *)id_buffer;

    memset(id_buffer, 0xcc, sizeof(id_buffer));
    expected_size = get_package_id_size(&test_package_id);
    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(fullname, 0, &size, id_buffer);
    ok(ret == ERROR_SUCCESS, "Got unexpected ret %lu.\n", ret);
    ok(size == expected_size, "Got unexpected length %u, expected %u.\n", size, expected_size);
    ok(!lstrcmpW(id->name, test_package_id.name), "Got unexpected name %s.\n", debugstr_w(id->name));
    ok(!lstrcmpW(id->resourceId, test_package_id.resourceId), "Got unexpected resourceId %s.\n",
            debugstr_w(id->resourceId));
    ok(!lstrcmpW(id->publisherId, test_package_id.publisherId), "Got unexpected publisherId %s.\n",
            debugstr_w(id->publisherId));
    ok(!id->publisher, "Got unexpected publisher %s.\n", debugstr_w(id->publisher));
    ok(id->processorArchitecture == PROCESSOR_ARCHITECTURE_INTEL, "Got unexpected processorArchitecture %u.\n",
            id->processorArchitecture);
    ok(id->version.Version == 0x0001000200030004, "Got unexpected Version %s.\n",
            wine_dbgstr_longlong(id->version.Version));
    ok((BYTE *)id->name == id_buffer + sizeof(*id), "Got unexpected name %p, buffer %p.\n", id->name, id_buffer);
    ok((BYTE *)id->resourceId == (BYTE *)id->name + (lstrlenW(id->name) + 1) * 2,
            "Got unexpected resourceId %p, buffer %p.\n", id->resourceId, id_buffer);
    ok((BYTE *)id->publisherId == (BYTE *)id->resourceId + (lstrlenW(id->resourceId) + 1) * 2,
            "Got unexpected publisherId %p, buffer %p.\n", id->resourceId, id_buffer);

    ret = pPackageIdFromFullName(fullname, 0, NULL, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(NULL, 0, &size, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);
    ok(size == sizeof(id_buffer), "Got unexpected size %u.\n", size);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(fullname, 0, &size, NULL);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);
    ok(size == sizeof(id_buffer), "Got unexpected size %u.\n", size);

    size = expected_size - 1;
    ret = pPackageIdFromFullName(fullname, 0, &size, NULL);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);
    ok(size == expected_size - 1, "Got unexpected size %u.\n", size);

    size = expected_size - 1;
    ret = pPackageIdFromFullName(fullname, 0, &size, id_buffer);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    ok(size == expected_size, "Got unexpected size %u.\n", size);

    size = 0;
    ret = pPackageIdFromFullName(fullname, 0, &size, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got unexpected ret %ld.\n", ret);
    ok(size == expected_size, "Got unexpected size %u.\n", size);

    for (i = 0; i < ARRAY_SIZE(arch_data); ++i)
    {
        test_id = test_package_id;
        test_id.processorArchitecture = arch_data[i].code;
        packagefullname_from_packageid(fullname, ARRAY_SIZE(fullname), &test_id);
        size = expected_size;
        ret = pPackageIdFromFullName(fullname, 0, &size, id_buffer);
        if (arch_data[i].code == PROCESSOR_ARCHITECTURE_UNKNOWN)
        {
            ok(ret == ERROR_INVALID_PARAMETER,
                    "Got unexpected ret %lu for unsupported arch %S.\n",
                    ret, arch_data[i].name);
            continue;
        }
        ok(ret == ERROR_SUCCESS || broken(arch_data[i].broken && ret == ERROR_INVALID_PARAMETER),
                "Got unexpected ret %lu.\n", ret);
        if (ret != ERROR_SUCCESS)
            continue;
        ok(size == expected_size, "Got unexpected length %u, expected %u.\n", size, expected_size);
        ok(id->processorArchitecture == arch_data[i].code, "Got unexpected processorArchitecture %u, arch %S.\n",
                id->processorArchitecture, arch_data[i].name);
    }

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(L"TestPackage_1.2.3.4_X86_TestResourceId_0abcdefghjkme", 0, &size, id_buffer);
    ok(ret == ERROR_SUCCESS, "Got unexpected ret %lu.\n", ret);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(L"TestPackage_1.2.3.4_X86_TestResourceId_abcdefghjkme", 0, &size, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %lu.\n", ret);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(L"TestPackage_1.2.3.4_X86_TestResourceId_0abcdefghjkmee", 0, &size, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %lu.\n", ret);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(L"TestPackage_1.2.3_X86_TestResourceId_0abcdefghjkme", 0, &size, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %lu.\n", ret);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(L"TestPackage_1.2.3.4_X86_TestResourceId_0abcdefghjkme_", 0, &size, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %lu.\n", ret);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(L"TestPackage_1.2.3.4_X86__0abcdefghjkme", 0, &size, id_buffer);
    ok(ret == ERROR_SUCCESS, "Got unexpected ret %lu.\n", ret);
    ok(!lstrcmpW(id->resourceId, L""), "Got unexpected resourceId %s.\n", debugstr_w(id->resourceId));

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(L"TestPackage_1.2.3.4_X86_0abcdefghjkme", 0, &size, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %lu.\n", ret);

    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(
            L"TestPackage_1.2.3.4_X86A64_TestResourceId_0abcdefghjkme",
            0, &size, id_buffer);
    ok(ret == ERROR_SUCCESS || broken(ret == ERROR_INVALID_PARAMETER),
            "Got unexpected ret %lu.\n", ret);
    if (!ret)
        ok(id->processorArchitecture == PROCESSOR_ARCHITECTURE_IA32_ON_ARM64,
                "Got architecture %u.\n", id->processorArchitecture);

    for (i = 0; i < ARRAY_SIZE(invalid_full_names); i++)
    {
        memset(id_buffer, 0xcc, sizeof(id_buffer));
        size = sizeof(id_buffer);
        ret = pPackageIdFromFullName(
                invalid_full_names[i], PACKAGE_INFORMATION_BASIC,
                &size, id_buffer);
        ok(ret == ERROR_INVALID_PARAMETER ||
                broken(wcsstr(invalid_full_names[i], L"_unknown_") &&
                       ret == ERROR_SUCCESS),
                "Name %s returned %ld.\n",
                debugstr_w(invalid_full_names[i]), ret);
        if (ret == ERROR_INVALID_PARAMETER)
        {
            ok(size == sizeof(id_buffer), "Name %s changed size to %u.\n",
                    debugstr_w(invalid_full_names[i]), size);
            ok(bytes_are_value(id_buffer, sizeof(id_buffer), 0xcc),
                    "Name %s partially wrote output.\n",
                    debugstr_w(invalid_full_names[i]));
        }
    }

    memset(id_buffer, 0xcc, sizeof(id_buffer));
    size = sizeof(id_buffer);
    ret = pPackageIdFromFullName(fullname, 1, &size, id_buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got unexpected ret %ld.\n", ret);
    ok(size == sizeof(id_buffer), "Size changed to %u.\n", size);
    ok(bytes_are_value(id_buffer, sizeof(id_buffer), 0xcc),
            "Unsupported flags partially wrote output.\n");

    size = 0;
    ret = pPackageIdFromFullName(missing_full_name, PACKAGE_INFORMATION_FULL,
            &size, NULL);
    ok(ret == ERROR_NOT_FOUND, "Got unexpected ret %ld.\n", ret);
}

#define TEST_VERSION_WIN7   1
#define TEST_VERSION_WIN8   2
#define TEST_VERSION_WIN8_1 4
#define TEST_VERSION_WIN10  8

static const struct
{
    unsigned int pe_version_major, pe_version_minor;
    unsigned int manifest_versions;
    unsigned int expected_major, expected_minor;
}
test_pe_os_version_tests[] =
{
    { 4, 0,                         0,  6, 2},
    { 4, 0,        TEST_VERSION_WIN10, 10, 0},
    { 6, 3,         TEST_VERSION_WIN8,  6, 2},
    {10, 0,                         0, 10, 0},
    { 6, 3,                         0,  6, 3},
    { 6, 4,                         0,  6, 3},
    { 9, 0,                         0,  6, 3},
    {11, 0,                         0, 10, 0},
    {10, 0,
            TEST_VERSION_WIN7 | TEST_VERSION_WIN8 | TEST_VERSION_WIN8_1,
                                        6, 3},
};

static void test_pe_os_version_child(unsigned int test)
{
    OSVERSIONINFOEXA info;
    BOOL ret;

    info.dwOSVersionInfoSize = sizeof(info);
    ret = GetVersionExA((OSVERSIONINFOA *)&info);
    ok(ret, "Got unexpected ret %#x, GetLastError() %lu.\n", ret, GetLastError());
    ok(info.dwMajorVersion == test_pe_os_version_tests[test].expected_major,
            "Test %u, expected major version %u, got %lu.\n", test, test_pe_os_version_tests[test].expected_major,
            info.dwMajorVersion);
    ok(info.dwMinorVersion == test_pe_os_version_tests[test].expected_minor,
            "Test %u, expected minor version %u, got %lu.\n", test, test_pe_os_version_tests[test].expected_minor,
            info.dwMinorVersion);
}

static void test_pe_os_version(void)
{
    static const char manifest_header[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<assembly manifestVersion=\"1.0\" xmlns=\"urn:schemas-microsoft-com:asm.v1\""
                    " xmlns:asmv3=\"urn:schemas-microsoft-com:asm.v3\">\n"
                "\t<compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\n"
                    "\t\t<application>\n";
    static const char manifest_footer[] =
                    "\t\t</application>\n"
                "\t</compatibility>\n"
            "</assembly>\n";
    static const char *version_guids[] =
    {
        "{35138b9a-5d96-4fbd-8e2d-a2440225f93a}",
        "{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}",
        "{1f676c76-80e1-4239-95bb-83d0f6d0da78}",
        "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}",
    };
    LONG hdr_offset, offset_major, offset_minor;
    char str[MAX_PATH], tmp_exe_name[9];
    RTL_OSVERSIONINFOEXW rtlinfo;
    STARTUPINFOA si = { 0 };
    PROCESS_INFORMATION pi;
    DWORD result, code;
    unsigned int i, j;
    HANDLE file;
    char **argv;
    DWORD size;
    BOOL ret;

    winetest_get_mainargs( &argv );

    if (!pRtlGetVersion)
    {
        win_skip("RtlGetVersion is not supported, skipping tests.\n");
        return;
    }

    rtlinfo.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOEXW);
    ok(!pRtlGetVersion(&rtlinfo), "RtlGetVersion failed.\n");
    if (rtlinfo.dwMajorVersion < 10)
    {
        skip("Too old Windows version %lu.%lu, skipping tests.\n", rtlinfo.dwMajorVersion, rtlinfo.dwMinorVersion);
        return;
    }

    file = CreateFileA(argv[0], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(file != INVALID_HANDLE_VALUE, "CreateFile failed, GetLastError() %lu.\n", GetLastError());
    SetFilePointer(file, 0x3c, NULL, FILE_BEGIN);
    ReadFile(file, &hdr_offset, sizeof(hdr_offset), &size, NULL);
    CloseHandle(file);

    offset_major = hdr_offset + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader)
            + FIELD_OFFSET(IMAGE_OPTIONAL_HEADER, MajorOperatingSystemVersion);
    offset_minor = hdr_offset + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader)
            + FIELD_OFFSET(IMAGE_OPTIONAL_HEADER, MinorOperatingSystemVersion);

    si.cb = sizeof(si);

    for (i = 0; i < ARRAY_SIZE(test_pe_os_version_tests); ++i)
    {
        sprintf(tmp_exe_name, "tmp%u.exe", i);
        ret = CopyFileA(argv[0], tmp_exe_name, FALSE);
        ok(ret, "Got unexpected ret %#x, GetLastError() %lu.\n", ret, GetLastError());

        file = CreateFileA(tmp_exe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        ok(file != INVALID_HANDLE_VALUE, "CreateFile failed, GetLastError() %lu.\n", GetLastError());

        SetFilePointer(file, offset_major, NULL, FILE_BEGIN);
        WriteFile(file, &test_pe_os_version_tests[i].pe_version_major,
                sizeof(test_pe_os_version_tests[i].pe_version_major), &size, NULL);
        SetFilePointer(file, offset_minor, NULL, FILE_BEGIN);
        WriteFile(file, &test_pe_os_version_tests[i].pe_version_minor,
                sizeof(test_pe_os_version_tests[i].pe_version_minor), &size, NULL);

        CloseHandle(file);

        sprintf(str, "%s.manifest", tmp_exe_name);
        file = CreateFileA(str, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok(file != INVALID_HANDLE_VALUE, "CreateFile failed, GetLastError() %lu.\n", GetLastError());

        WriteFile(file, manifest_header, strlen(manifest_header), &size, NULL);
        for (j = 0; j < ARRAY_SIZE(version_guids); ++j)
        {
            if (test_pe_os_version_tests[i].manifest_versions & (1 << j))
            {
                sprintf(str, "\t\t\t<supportedOS Id=\"%s\"/>\n", version_guids[j]);
                WriteFile(file, str, strlen(str), &size, NULL);
            }
        }
        WriteFile(file, manifest_footer, strlen(manifest_footer), &size, NULL);

        CloseHandle(file);

        sprintf(str, "%s version pe_os_version %u", tmp_exe_name, i);

        ret = CreateProcessA(NULL, str, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        ok(ret, "Got unexpected ret %#x, GetLastError() %lu.\n", ret, GetLastError());
        CloseHandle(pi.hThread);
        result = WaitForSingleObject(pi.hProcess, 10000);
        ok(result == WAIT_OBJECT_0, "Got unexpected result %#lx.\n", result);

        ret = GetExitCodeProcess(pi.hProcess, &code);
        ok(ret, "Got unexpected ret %#x, GetLastError() %lu.\n", ret, GetLastError());
        ok(!code, "Test %u failed.\n", i);

        CloseHandle(pi.hProcess);

        DeleteFileA(tmp_exe_name);
        sprintf(str, "%s.manifest", tmp_exe_name);
        DeleteFileA(str);
    }
}

START_TEST(version)
{
    char **argv;
    int argc;

    argc = winetest_get_mainargs( &argv );

    init_function_pointers();

    if (argc >= 4)
    {
        if (!strcmp(argv[2], "pe_os_version"))
        {
            unsigned int test;

            test = atoi(argv[3]);
            test_pe_os_version_child(test);
        }
        return;
    }

    test_GetProductInfo();
    test_GetVersionEx();
    test_VerifyVersionInfo();
    test_pe_os_version();
    test_SystemFirmwareTable();
    test_current_package_graph();
    test_package_identity_names();
    test_PackageIdFromFullName();
}
