/*
 * Copyright 2012 Hans Leidekker for CodeWeavers
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

#define COBJMACROS

#include <stdio.h>
#include "windows.h"
#include "objidl.h"
#include "wbemcli.h"
#include "wine/test.h"

#define EXPECT_REF(obj,ref) _expect_ref((IUnknown*)obj, ref, __LINE__)
static void _expect_ref(IUnknown* obj, ULONG ref, int line)
{
    ULONG rc;
    IUnknown_AddRef(obj);
    rc = IUnknown_Release(obj);
    ok_(__FILE__,line)(rc == ref, "expected refcount %lu, got %lu\n", ref, rc);
}

static void test_IClientSecurity(void)
{
    HRESULT hr;
    IWbemLocator *locator;
    IWbemServices *services;
    IClientSecurity *security;
    BSTR path = SysAllocString( L"ROOT\\CIMV2" );
    DWORD authn_svc, authz_svc, authn_level, imp_level, capabilities;
    OLECHAR *principal;
    void *auth_info;
    ULONG refs;

    hr = CoCreateInstance( &CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator );
    if (hr != S_OK)
    {
        win_skip("can't create instance of WbemLocator\n");
        return;
    }
    ok( hr == S_OK, "failed to create IWbemLocator interface %#lx\n", hr );

    refs = IWbemLocator_Release( locator );
    ok( refs == 0, "unexpected refcount %lu\n", refs );

    hr = CoCreateInstance( &CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator );
    ok( hr == S_OK, "failed to create IWbemLocator interface %#lx\n", hr );

    hr = IWbemLocator_ConnectServer( locator, path, NULL, NULL, NULL, 0, NULL, NULL, &services );
    ok( hr == S_OK, "failed to get IWbemServices interface %#lx\n", hr );

    refs = IWbemServices_Release( services );
    ok( refs == 0, "unexpected refcount %lu\n", refs );

    hr = IWbemLocator_ConnectServer( locator, path, NULL, NULL, NULL, 0, NULL, NULL, &services );
    ok( hr == S_OK, "failed to get IWbemServices interface %#lx\n", hr );

    hr = IWbemServices_QueryInterface( services, &IID_IClientSecurity, (void **)&security );
    ok( hr == S_OK, "failed to query IClientSecurity interface %#lx\n", hr );
    ok( (void *)services != (void *)security, "expected pointers to be different\n" );

    refs = IClientSecurity_Release( security );
    ok( refs == 1, "unexpected refcount %lu\n", refs );

    refs = IWbemServices_Release( services );
    ok( refs == 0, "unexpected refcount %lu\n", refs );

    hr = IWbemLocator_ConnectServer( locator, path, NULL, NULL, NULL, 0, NULL, NULL, &services );
    ok( hr == S_OK, "failed to get IWbemServices interface %#lx\n", hr );

    hr = IWbemServices_QueryInterface( services, &IID_IClientSecurity, (void **)&security );
    ok( hr == S_OK, "failed to query IClientSecurity interface %#lx\n", hr );
    ok( (void *)services != (void *)security, "expected pointers to be different\n" );

    hr = IClientSecurity_SetBlanket( security, (IUnknown *)services, RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0 );
    ok( hr == S_OK, "SetBlanket failed %#lx\n", hr );

    authn_svc = authz_svc = authn_level = imp_level = capabilities = 0xdeadbeef;
    principal = (OLECHAR *)0xdeadbeef;
    auth_info = (void *)0xdeadbeef;
    hr = IClientSecurity_QueryBlanket( security, (IUnknown *)services, &authn_svc, &authz_svc,
            &principal, &authn_level, &imp_level, &auth_info, &capabilities );
    ok( hr == S_OK, "QueryBlanket failed %#lx\n", hr );
    ok( authn_svc == RPC_C_AUTHN_WINNT, "got authentication service %lu\n", authn_svc );
    ok( authz_svc == RPC_C_AUTHZ_NONE, "got authorization service %lu\n", authz_svc );
    ok( !principal, "got principal %s\n", wine_dbgstr_w(principal) );
    ok( authn_level == RPC_C_AUTHN_LEVEL_CALL, "got authentication level %lu\n", authn_level );
    ok( imp_level == RPC_C_IMP_LEVEL_IMPERSONATE, "got impersonation level %lu\n", imp_level );
    ok( !auth_info, "got authentication info %p\n", auth_info );
    ok( !capabilities, "got capabilities %#lx\n", capabilities );
    CoTaskMemFree( principal );

    hr = IClientSecurity_QueryBlanket( security, (IUnknown *)locator, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL );
    ok( hr == E_NOINTERFACE, "QueryBlanket for another proxy returned %#lx\n", hr );
    hr = IClientSecurity_SetBlanket( security, (IUnknown *)services, 0xdeadbeef,
            RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0 );
    ok( hr == E_INVALIDARG, "SetBlanket with invalid authentication service returned %#lx\n", hr );

    refs = IWbemServices_Release( services );
    ok( refs == 1, "unexpected refcount %lu\n", refs );

    refs = IClientSecurity_Release( security );
    ok( refs == 0, "unexpected refcount %lu\n", refs );

    IWbemLocator_Release( locator );
    SysFreeString( path );
}

static void check_client_security_blanket( const char *name, IClientSecurity *security, IUnknown *proxy,
        DWORD expected_authn_level, const WCHAR *expected_principal )
{
    DWORD authn_svc, authz_svc, authn_level, imp_level, capabilities;
    OLECHAR *principal = (OLECHAR *)0xdeadbeef;
    void *auth_info = (void *)0xdeadbeef;
    const char *principal_debug;
    HRESULT hr;

    authn_svc = authz_svc = authn_level = imp_level = capabilities = 0xdeadbeef;
    hr = IClientSecurity_QueryBlanket( security, proxy, &authn_svc, &authz_svc, &principal,
            &authn_level, &imp_level, &auth_info, &capabilities );
    ok( hr == S_OK, "%s: QueryBlanket failed %#lx\n", name, hr );
    if (FAILED(hr)) return;
    principal_debug = principal == (OLECHAR *)0xdeadbeef ? "<unchanged>" : wine_dbgstr_w(principal);
    ok( authn_svc == RPC_C_AUTHN_WINNT, "%s: got authentication service %lu\n", name, authn_svc );
    ok( authz_svc == RPC_C_AUTHZ_NONE, "%s: got authorization service %lu\n", name, authz_svc );
    if (expected_principal)
        ok( principal != (OLECHAR *)0xdeadbeef && principal && !lstrcmpW(principal, expected_principal),
                "%s: got principal %s\n",
                name, principal_debug );
    else
        ok( !principal, "%s: got principal %s\n", name, principal_debug );
    ok( authn_level == expected_authn_level, "%s: got authentication level %lu\n", name, authn_level );
    ok( imp_level == RPC_C_IMP_LEVEL_IMPERSONATE, "%s: got impersonation level %lu\n", name, imp_level );
    ok( !auth_info, "%s: got authentication info %p\n", name, auth_info );
    ok( !capabilities, "%s: got capabilities %#lx\n", name, capabilities );
    if (principal != (OLECHAR *)0xdeadbeef) CoTaskMemFree( principal );
}

static void test_IClientSecurity_objects(void)
{
    static const WCHAR class_principal[] = L"wbemprox/test";
    IEnumWbemClassObject *enumerator;
    IClientSecurity *enumerator_security, *class_security;
    IWbemClassObject *class_object;
    IWbemLocator *locator;
    IWbemServices *services;
    BSTR path, language, query;
    DWORD authn_svc, authz_svc;
    ULONG count, refs;
    HRESULT hr;

    hr = CoCreateInstance( &CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator );
    if (hr != S_OK)
    {
        win_skip("can't create instance of WbemLocator\n");
        return;
    }

    path = SysAllocString( L"ROOT\\CIMV2" );
    hr = IWbemLocator_ConnectServer( locator, path, NULL, NULL, NULL, 0, NULL, NULL, &services );
    ok( hr == S_OK, "failed to get IWbemServices interface %#lx\n", hr );
    SysFreeString( path );
    if (FAILED(hr))
    {
        IWbemLocator_Release( locator );
        return;
    }

    language = SysAllocString( L"WQL" );
    query = SysAllocString( L"SELECT * FROM Win32_OperatingSystem" );
    hr = IWbemServices_ExecQuery( services, language, query, 0, NULL, &enumerator );
    ok( hr == S_OK, "ExecQuery failed %#lx\n", hr );
    SysFreeString( query );
    SysFreeString( language );
    if (FAILED(hr))
    {
        IWbemServices_Release( services );
        IWbemLocator_Release( locator );
        return;
    }

    hr = IEnumWbemClassObject_QueryInterface( enumerator, &IID_IClientSecurity,
            (void **)&enumerator_security );
    ok( hr == S_OK, "enumerator QueryInterface failed %#lx\n", hr );
    if (FAILED(hr))
    {
        IEnumWbemClassObject_Release( enumerator );
        IWbemServices_Release( services );
        IWbemLocator_Release( locator );
        return;
    }

    hr = IClientSecurity_SetBlanket( enumerator_security, (IUnknown *)enumerator, RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0 );
    ok( hr == S_OK, "enumerator SetBlanket failed %#lx\n", hr );
    check_client_security_blanket( "enumerator", enumerator_security, (IUnknown *)enumerator,
            RPC_C_AUTHN_LEVEL_CALL, NULL );

    hr = IClientSecurity_SetBlanket( enumerator_security, (IUnknown *)enumerator, RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_NONE, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0 );
    ok( hr == E_INVALIDARG, "invalid authentication combination returned %#lx\n", hr );

    count = 0;
    class_object = NULL;
    hr = IEnumWbemClassObject_Next( enumerator, WBEM_INFINITE, 1, &class_object, &count );
    ok( hr == S_OK, "Next failed %#lx\n", hr );
    ok( count == 1, "got object count %lu\n", count );
    if (hr != S_OK || !count)
    {
        IClientSecurity_Release( enumerator_security );
        IEnumWbemClassObject_Release( enumerator );
        IWbemServices_Release( services );
        IWbemLocator_Release( locator );
        return;
    }

    hr = IWbemClassObject_QueryInterface( class_object, &IID_IClientSecurity, (void **)&class_security );
    ok( hr == S_OK, "class object QueryInterface failed %#lx\n", hr );
    if (FAILED(hr))
    {
        IWbemClassObject_Release( class_object );
        IClientSecurity_Release( enumerator_security );
        IEnumWbemClassObject_Release( enumerator );
        IWbemServices_Release( services );
        IWbemLocator_Release( locator );
        return;
    }

    hr = IClientSecurity_SetBlanket( class_security, (IUnknown *)class_object, RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, (OLECHAR *)class_principal, RPC_C_AUTHN_LEVEL_PKT_INTEGRITY,
            RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0 );
    ok( hr == S_OK, "class object SetBlanket failed %#lx\n", hr );
    check_client_security_blanket( "class object", class_security, (IUnknown *)class_object,
            RPC_C_AUTHN_LEVEL_PKT_INTEGRITY, class_principal );

    /* The class and its enumerator are different proxies and keep independent blankets. */
    check_client_security_blanket( "enumerator after class update", enumerator_security,
            (IUnknown *)enumerator, RPC_C_AUTHN_LEVEL_CALL, NULL );

    hr = IClientSecurity_SetBlanket( class_security, (IUnknown *)class_object, RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL,
            EOAC_STATIC_CLOAKING | EOAC_DYNAMIC_CLOAKING );
    ok( hr == E_INVALIDARG, "conflicting cloaking flags returned %#lx\n", hr );

    hr = IClientSecurity_SetBlanket( class_security, (IUnknown *)class_object, RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
            (void *)0xdeadbeef, EOAC_DYNAMIC_CLOAKING );
    ok( hr == E_INVALIDARG, "explicit credentials with cloaking returned %#lx\n", hr );

    hr = IClientSecurity_QueryBlanket( class_security, (IUnknown *)enumerator, &authn_svc, &authz_svc, NULL,
            NULL, NULL, NULL, NULL );
    ok( hr == E_NOINTERFACE, "QueryBlanket for another proxy returned %#lx\n", hr );
    check_client_security_blanket( "class object after invalid updates", class_security,
            (IUnknown *)class_object, RPC_C_AUTHN_LEVEL_PKT_INTEGRITY, class_principal );

    refs = IWbemClassObject_Release( class_object );
    ok( refs == 1, "unexpected class object refcount %lu\n", refs );
    refs = IClientSecurity_Release( class_security );
    ok( refs == 0, "unexpected class security refcount %lu\n", refs );
    refs = IEnumWbemClassObject_Release( enumerator );
    ok( refs == 1, "unexpected enumerator refcount %lu\n", refs );
    refs = IClientSecurity_Release( enumerator_security );
    ok( refs == 0, "unexpected enumerator security refcount %lu\n", refs );

    IWbemServices_Release( services );
    IWbemLocator_Release( locator );
}

static void test_IWbemLocator(void)
{
    static const struct
    {
        const WCHAR *path;
        HRESULT      result;
        BOOL         todo;
        HRESULT      result_broken;
    }
    test[] =
    {
        { L"", WBEM_E_INVALID_NAMESPACE },
        { L"\\", WBEM_E_INVALID_NAMESPACE },
        { L"\\\\", WBEM_E_INVALID_NAMESPACE },
        { L"\\\\.", WBEM_E_INVALID_NAMESPACE },
        { L"\\\\.\\", WBEM_E_INVALID_NAMESPACE, FALSE, WBEM_E_INVALID_PARAMETER },
        { L"\\ROOT", WBEM_E_INVALID_NAMESPACE },
        { L"\\\\ROOT", __HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE), TRUE },
        { L"\\\\.ROOT", __HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE), TRUE },
        { L"\\\\.\\NONE", WBEM_E_INVALID_NAMESPACE },
        { L"\\\\.\\ROOT", S_OK },
        { L"\\\\\\.\\ROOT", WBEM_E_INVALID_PARAMETER },
        { L"\\/.\\ROOT", S_OK, FALSE, WBEM_E_INVALID_PARAMETER },
        { L"//.\\ROOT", S_OK },
        { L"\\\\./ROOT", S_OK },
        { L"//./ROOT", S_OK },
        { L"NONE", WBEM_E_INVALID_NAMESPACE },
        { L"ROOT", S_OK },
        { L"ROOT\\NONE", WBEM_E_INVALID_NAMESPACE },
        { L"ROOT\\CIMV2", S_OK },
        { L"ROOT\\\\CIMV2", WBEM_E_INVALID_NAMESPACE, FALSE, WBEM_E_INVALID_PARAMETER },
        { L"ROOT\\CIMV2\\", WBEM_E_INVALID_NAMESPACE, FALSE, WBEM_E_INVALID_PARAMETER },
        { L"ROOT/CIMV2", S_OK },
        { L"root\\default", S_OK },
        { L"root\\cimv0", WBEM_E_INVALID_NAMESPACE },
        { L"root\\cimv1", WBEM_E_INVALID_NAMESPACE },
        { L"\\\\localhost\\ROOT", S_OK },
        { L"\\\\LOCALHOST\\ROOT", S_OK }
    };
    IWbemLocator *locator;
    IWbemServices *services;
    IWbemContext *context;
    unsigned int i;
    HRESULT hr;
    BSTR resource;

    hr = CoCreateInstance( &CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator );
    if (hr != S_OK)
    {
        win_skip("can't create instance of WbemLocator\n");
        return;
    }
    ok( hr == S_OK, "failed to create IWbemLocator interface %#lx\n", hr );

    for (i = 0; i < ARRAY_SIZE( test ); i++)
    {
        resource = SysAllocString( test[i].path );
        hr = IWbemLocator_ConnectServer( locator, resource, NULL, NULL, NULL, 0, NULL, NULL, &services );
        todo_wine_if (test[i].todo)
            ok( hr == test[i].result || broken(hr == test[i].result_broken),
                "%u: expected %#lx got %#lx\n", i, test[i].result, hr );
        SysFreeString( resource );
        if (hr == S_OK) IWbemServices_Release( services );
    }

    hr = CoCreateInstance( &CLSID_WbemContext, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemContext, (void **)&context );
    ok(hr == S_OK, "failed to create context object, hr %#lx\n", hr);

    EXPECT_REF(context, 1);
    resource = SysAllocString( L"root\\default" );
    hr = IWbemLocator_ConnectServer( locator, resource, NULL, NULL, NULL, 0, NULL, context, &services );
    ok(hr == S_OK, "failed to connect, hr %#lx\n", hr);
    SysFreeString( resource );
    EXPECT_REF(context, 1);
    IWbemServices_Release( services );
    IWbemContext_Release( context );
    IWbemLocator_Release( locator );
}

static void test_IWbemContext(void)
{
    IWbemContext *context;
    VARIANT var;
    HRESULT hr;
    BSTR str;

    hr = CoCreateInstance( &CLSID_WbemContext, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemContext, (void **)&context );
    ok(hr == S_OK, "failed to create context object, hr %#lx\n", hr);

    hr = IWbemContext_SetValue(context, L"name", 0, NULL);
    ok(hr == WBEM_E_INVALID_PARAMETER, "unexpected hr %#lx\n", hr);

    V_VT(&var) = VT_I4;
    V_I4(&var) = 12;
    hr = IWbemContext_SetValue(context, NULL, 0, &var);
    ok(hr == WBEM_E_INVALID_PARAMETER, "unexpected hr %#lx\n", hr);

    hr = IWbemContext_SetValue(context, L"name", 0, &var);
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);

    hr = IWbemContext_GetValue(context, NULL, 0, &var);
    ok(hr == WBEM_E_INVALID_PARAMETER, "unexpected hr %#lx\n", hr);

    hr = IWbemContext_GetValue(context, L"name", 0, NULL);
    ok(hr == WBEM_E_INVALID_PARAMETER, "unexpected hr %#lx\n", hr);

    hr = IWbemContext_GetValue(context, L"noname", 0, &var);
    ok(hr == WBEM_E_NOT_FOUND, "unexpected hr %#lx\n", hr);

    V_VT(&var) = VT_EMPTY;
    hr = IWbemContext_GetValue(context, L"NAME", 0, &var);
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);
    ok(V_VT(&var) == VT_I4, "unexpected value type\n");

    V_VT(&var) = VT_I4;
    V_I4(&var) = 13;
    hr = IWbemContext_SetValue(context, L"name2", 0, &var);
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);

    hr = IWbemContext_Next(context, 0, &str, &var);
    todo_wine
    ok(hr == WBEM_E_UNEXPECTED, "unexpected hr %#lx\n", hr);

    hr = IWbemContext_BeginEnumeration(context, 0);
    todo_wine
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);

    str = NULL;
    hr = IWbemContext_Next(context, 0, &str, &var);
todo_wine {
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);
    ok(!lstrcmpW(str, L"name"), "unexpected name %s\n", wine_dbgstr_w(str));
    SysFreeString(str);
}
    hr = IWbemContext_EndEnumeration(context);
    todo_wine
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);

    /* Overwrite */
    V_VT(&var) = VT_I4;
    V_I4(&var) = 14;
    hr = IWbemContext_SetValue(context, L"name", 0, &var);
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);

    V_VT(&var) = VT_EMPTY;
    hr = IWbemContext_GetValue(context, L"name", 0, &var);
    ok(hr == S_OK, "unexpected hr %#lx\n", hr);
    ok(V_VT(&var) == VT_I4, "unexpected value type\n");
    ok(V_I4(&var) == 14, "unexpected value\n");

    IWbemContext_Release( context );
}

static void test_namespaces(void)
{
    static const struct
    {
        const WCHAR *path;
        BOOL broken;
    }
    tests[] =
    {
        {L"ROOT\\CIMV2"},
        {L"ROOT\\Microsoft\\Windows\\Storage", TRUE /* Before Win8. */},
        {L"ROOT\\StandardCimv2", TRUE /* Before Win8. */},
        {L"ROOT\\WMI"},
    };
    IWbemLocator *locator;
    IWbemServices *services;
    unsigned int i;
    BSTR resource;
    HRESULT hr;

    hr = CoCreateInstance( &CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator );
    if (hr != S_OK)
    {
        win_skip( "can't create instance of WbemLocator.\n" );
        return;
    }

    for (i = 0; i < ARRAY_SIZE( tests ); i++)
    {
        resource = SysAllocString( tests[i].path );
        hr = IWbemLocator_ConnectServer( locator, resource, NULL, NULL, NULL, 0, NULL, NULL, &services );
        ok( hr == S_OK || broken( tests[i].broken && hr == WBEM_E_INVALID_NAMESPACE ), "%u: got %#lx\n", i, hr );
        SysFreeString( resource );
        if (hr == S_OK)
            IWbemServices_Release( services );
    }

    IWbemLocator_Release( locator );
}

START_TEST(services)
{
    CoInitialize( NULL );
    test_IClientSecurity();
    test_IClientSecurity_objects();
    test_IWbemLocator();
    test_IWbemContext();
    test_namespaces();
    CoUninitialize();
}
