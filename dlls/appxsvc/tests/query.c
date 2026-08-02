/*
 * AppX installed package query tests
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#define appx_deployment_query test_appx_deployment_query
#define appx_catalog_snapshot_free test_appx_catalog_snapshot_free
#define appx_catalog_snapshot_get_package_count \
    test_appx_catalog_snapshot_get_package_count
#define appx_catalog_snapshot_get_package \
    test_appx_catalog_snapshot_get_package
#define wine_appx_get_package_path_by_full_name \
    test_wine_appx_get_package_path_by_full_name
#define wine_appx_get_packages_by_family \
    test_wine_appx_get_packages_by_family
#define wine_appx_get_package_publisher_by_full_name \
    test_wine_appx_get_package_publisher_by_full_name

#include "../query.c"

#undef appx_deployment_query
#undef appx_catalog_snapshot_free
#undef appx_catalog_snapshot_get_package_count
#undef appx_catalog_snapshot_get_package
#undef wine_appx_get_package_path_by_full_name
#undef wine_appx_get_packages_by_family
#undef wine_appx_get_package_publisher_by_full_name

#include "wine/test.h"

struct appx_catalog_snapshot
{
    const struct appx_catalog_package *packages;
    UINT32 count;
};

static const struct appx_catalog_package packages[] =
{
    {
        .name = L"Wine.Query",
        .publisher = L"CN=Wine Query",
        .resource_id = L"",
        .publisher_id = L"0abcdefghjkme",
        .full_name = L"Wine.Query_1.0.0.0_x64__0abcdefghjkme",
        .family_name = L"Wine.Query_0abcdefghjkme",
        .payload_path = L"payloads\\0123456789abcdef0123456789abcdef",
        .flags = APPX_CATALOG_PACKAGE_ACTIVE | APPX_CATALOG_PACKAGE_SIGNED,
    },
    {
        .name = L"Wine.Query",
        .publisher = L"CN=Wine Query",
        .resource_id = L"en-US",
        .publisher_id = L"0abcdefghjkme",
        .full_name = L"Wine.Query_1.0.0.0_neutral_en-US_0abcdefghjkme",
        .family_name = L"Wine.Query_0abcdefghjkme",
        .payload_path = L"payloads\\1123456789abcdef0123456789abcdef",
        .flags = APPX_CATALOG_PACKAGE_ACTIVE | APPX_CATALOG_PACKAGE_SIGNED |
                 APPX_CATALOG_PACKAGE_RESOURCE,
    },
    {
        .name = L"Wine.Query",
        .publisher = L"CN=Old Wine Query",
        .resource_id = L"",
        .publisher_id = L"0abcdefghjkme",
        .full_name = L"Wine.Query_0.9.0.0_x64__0abcdefghjkme",
        .family_name = L"Wine.Query_0abcdefghjkme",
        .payload_path = L"payloads\\2123456789abcdef0123456789abcdef",
        .flags = APPX_CATALOG_PACKAGE_SIGNED,
    },
};

static struct appx_catalog_snapshot all_snapshot =
{
    packages, ARRAY_SIZE(packages)
};
static struct appx_catalog_snapshot one_snapshot;
static HRESULT query_hr = S_OK;
static LONG snapshot_free_count;

HRESULT WINAPI test_appx_deployment_query(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_CATALOG_SNAPSHOT **snapshot )
{
    UINT32 i;

    ok(options != NULL, "Options are NULL.\n");
    ok(options->size == sizeof(*options), "Got options size %u.\n",
            options->size);
    ok(options->version == APPX_DEPLOYMENT_OPTIONS_VERSION,
            "Got options version %u.\n", options->version);
    ok(!lstrcmpW(options->store_root, default_store_root),
            "Got store root %s.\n", wine_dbgstr_w(options->store_root));
    *snapshot = NULL;
    if (FAILED(query_hr)) return query_hr;
    if (!full_name)
    {
        *snapshot = &all_snapshot;
        return S_OK;
    }
    for (i = 0; i < ARRAY_SIZE(packages); i++)
        if (!lstrcmpiW(full_name, packages[i].full_name))
        {
            one_snapshot.packages = packages + i;
            one_snapshot.count = 1;
            *snapshot = &one_snapshot;
            return S_OK;
        }
    return HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND);
}

void WINAPI test_appx_catalog_snapshot_free(APPX_CATALOG_SNAPSHOT *snapshot)
{
    if (snapshot) InterlockedIncrement(&snapshot_free_count);
}

UINT32 WINAPI test_appx_catalog_snapshot_get_package_count(
    const APPX_CATALOG_SNAPSHOT *snapshot)
{
    const struct appx_catalog_snapshot *object =
            (const struct appx_catalog_snapshot *)snapshot;
    return object ? object->count : 0;
}

const struct appx_catalog_package * WINAPI
test_appx_catalog_snapshot_get_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, UINT32 index)
{
    const struct appx_catalog_snapshot *object =
            (const struct appx_catalog_snapshot *)snapshot;
    return object && index < object->count ? object->packages + index : NULL;
}

static BOOL bytes_are_value(const void *buffer, SIZE_T size, BYTE value)
{
    const BYTE *bytes = buffer;
    SIZE_T i;

    for (i = 0; i < size; i++)
        if (bytes[i] != value) return FALSE;
    return TRUE;
}

static void test_errors(void)
{
    WCHAR buffer[128];
    WCHAR *names[2];
    UINT32 length, count;
    LONG ret;

    length = 0x12345678;
    ret = test_wine_appx_get_package_path_by_full_name(NULL, &length, buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got ret %ld.\n", ret);
    ok(length == 0x12345678, "Length changed to %u.\n", length);
    ret = test_wine_appx_get_package_publisher_by_full_name(
            packages[0].full_name, NULL, buffer);
    ok(ret == ERROR_INVALID_PARAMETER, "Got ret %ld.\n", ret);

    query_hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    length = 0x12345678;
    ret = test_wine_appx_get_package_path_by_full_name(
            packages[0].full_name, &length, buffer);
    ok(ret == ERROR_NOT_FOUND, "Got ret %ld.\n", ret);
    ok(length == 0x12345678, "Length changed to %u.\n", length);
    count = length = 0x12345678;
    ret = test_wine_appx_get_packages_by_family(
            packages[0].family_name, &count, names, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got ret %ld.\n", ret);
    ok(!count && !length, "Got count %u and length %u.\n", count, length);

    query_hr = APPX_E_INVALID_PACKAGING_LAYOUT;
    count = length = 0x12345678;
    ret = test_wine_appx_get_packages_by_family(
            packages[0].family_name, &count, names, &length, buffer);
    ok(ret == ERROR_BAD_FORMAT, "Got ret %ld.\n", ret);
    ok(count == 0x12345678 && length == 0x12345678,
            "Outputs changed to %u and %u.\n", count, length);
    ret = test_wine_appx_get_package_publisher_by_full_name(
            packages[0].full_name, &length, buffer);
    ok(ret == ERROR_BAD_FORMAT, "Got ret %ld.\n", ret);
    ok(length == 0x12345678, "Length changed to %u.\n", length);
    query_hr = S_OK;
}

static void test_path_and_publisher(void)
{
    WCHAR expected[WINE_APPX_MAX_PATH_CHARS], buffer[256];
    UINT32 length, required;
    LONG ret;

    lstrcpyW(expected, default_store_root);
    lstrcatW(expected, L"\\");
    lstrcatW(expected, packages[0].payload_path);
    required = lstrlenW(expected) + 1;

    length = 0;
    ret = test_wine_appx_get_package_path_by_full_name(
            packages[0].full_name, &length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got ret %ld.\n", ret);
    ok(length == required, "Got length %u, expected %u.\n", length, required);
    memset(buffer, 0xcc, sizeof(buffer));
    length--;
    ret = test_wine_appx_get_package_path_by_full_name(
            packages[0].full_name, &length, buffer);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got ret %ld.\n", ret);
    ok(length == required, "Got length %u, expected %u.\n", length, required);
    ok(bytes_are_value(buffer, sizeof(buffer), 0xcc),
            "Path was partially written.\n");
    length = ARRAY_SIZE(buffer);
    ret = test_wine_appx_get_package_path_by_full_name(
            packages[0].full_name, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got ret %ld.\n", ret);
    ok(!lstrcmpW(buffer, expected), "Got path %s.\n", wine_dbgstr_w(buffer));

    length = 0;
    ret = test_wine_appx_get_package_publisher_by_full_name(
            packages[0].full_name, &length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got ret %ld.\n", ret);
    required = lstrlenW(packages[0].publisher) + 1;
    ok(length == required, "Got length %u, expected %u.\n", length, required);
    length = ARRAY_SIZE(buffer);
    ret = test_wine_appx_get_package_publisher_by_full_name(
            packages[0].full_name, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got ret %ld.\n", ret);
    ok(!lstrcmpW(buffer, packages[0].publisher),
            "Got publisher %s.\n", wine_dbgstr_w(buffer));

    length = 0;
    ret = test_wine_appx_get_package_path_by_full_name(
            L"Wine.Query_9.0.0.0_x64__0abcdefghjkme", &length, NULL);
    ok(ret == ERROR_NOT_FOUND, "Got ret %ld.\n", ret);
    ret = test_wine_appx_get_package_path_by_full_name(
            packages[2].full_name, &length, NULL);
    ok(ret == ERROR_NOT_FOUND, "Got ret %ld.\n", ret);
}

static void test_family(void)
{
    WCHAR buffer[256];
    WCHAR *names[3];
    UINT32 count, length, required;
    LONG ret;

    required = lstrlenW(packages[0].full_name) + 1 +
               lstrlenW(packages[1].full_name) + 1;
    count = length = 0;
    ret = test_wine_appx_get_packages_by_family(
            L"wine.query_0ABCDEFGHJKME", &count, NULL, &length, NULL);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got ret %ld.\n", ret);
    ok(count == 2, "Got count %u.\n", count);
    ok(length == required, "Got length %u, expected %u.\n", length, required);

    memset(names, 0xcc, sizeof(names));
    memset(buffer, 0xcc, sizeof(buffer));
    count = 1;
    length = ARRAY_SIZE(buffer);
    ret = test_wine_appx_get_packages_by_family(
            packages[0].family_name, &count, names, &length, buffer);
    ok(ret == ERROR_INSUFFICIENT_BUFFER, "Got ret %ld.\n", ret);
    ok(count == 2 && length == required, "Got count %u, length %u.\n",
            count, length);
    ok(bytes_are_value(names, sizeof(names), 0xcc),
            "Pointer array was partially written.\n");
    ok(bytes_are_value(buffer, sizeof(buffer), 0xcc),
            "String buffer was partially written.\n");

    count = ARRAY_SIZE(names);
    length = ARRAY_SIZE(buffer);
    ret = test_wine_appx_get_packages_by_family(
            packages[0].family_name, &count, names, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got ret %ld.\n", ret);
    ok(count == 2 && length == required, "Got count %u, length %u.\n",
            count, length);
    ok(names[0] == buffer, "Got first pointer %p, buffer %p.\n",
            names[0], buffer);
    ok(names[1] == buffer + lstrlenW(packages[0].full_name) + 1,
            "Got second pointer %p.\n", names[1]);
    ok(!lstrcmpW(names[0], packages[0].full_name),
            "Got first name %s.\n", wine_dbgstr_w(names[0]));
    ok(!lstrcmpW(names[1], packages[1].full_name),
            "Got second name %s.\n", wine_dbgstr_w(names[1]));

    count = length = 1;
    ret = test_wine_appx_get_packages_by_family(
            L"Wine.Other_0abcdefghjkme", &count, names, &length, buffer);
    ok(ret == ERROR_SUCCESS, "Got ret %ld.\n", ret);
    ok(!count && !length, "Got count %u, length %u.\n", count, length);
}

struct query_thread_context
{
    LONG failed;
};

static DWORD WINAPI query_thread_proc(void *parameter)
{
    struct query_thread_context *context = parameter;
    WCHAR buffer[256];
    WCHAR *names[2];
    UINT32 count, length, i;
    LONG ret;

    for (i = 0; i < 100; i++)
    {
        count = ARRAY_SIZE(names);
        length = ARRAY_SIZE(buffer);
        ret = test_wine_appx_get_packages_by_family(
                packages[0].family_name, &count, names, &length, buffer);
        if (ret || count != 2 || names[0] != buffer ||
            lstrcmpW(names[0], packages[0].full_name) ||
            lstrcmpW(names[1], packages[1].full_name))
        {
            InterlockedExchange(&context->failed, TRUE);
            break;
        }
    }
    return 0;
}

static void test_concurrent_queries(void)
{
    struct query_thread_context context = {0};
    HANDLE threads[4];
    DWORD result;
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(threads); i++)
    {
        threads[i] = CreateThread(NULL, 0, query_thread_proc, &context, 0, NULL);
        ok(threads[i] != NULL, "Failed to create thread %u, error %lu.\n",
                i, GetLastError());
        if (!threads[i]) break;
    }
    if (i)
    {
        result = WaitForMultipleObjects(i, threads, TRUE, 10000);
        ok(result == WAIT_OBJECT_0, "Got wait result %#lx.\n", result);
        while (i) CloseHandle(threads[--i]);
    }
    ok(!context.failed, "A concurrent package query failed.\n");
}

START_TEST(query)
{
    test_errors();
    test_path_and_publisher();
    test_family();
    test_concurrent_queries();
    ok(snapshot_free_count == 410, "Got snapshot free count %ld.\n",
            snapshot_free_count);
}
