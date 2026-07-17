#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

static int fail(int code)
{
    fprintf(stderr, "WinHTTP failure %d (GetLastError=%lu)\n", code, GetLastError());
    return code;
}

int main(void)
{
    HINTERNET session, connection, request;
    DWORD status = 0, status_size = sizeof(status);

    session = WinHttpOpen(L"Switchyard runtime TLS smoke test",
                          WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return fail(10);

    connection = WinHttpConnect(session, L"example.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) return fail(11);

    request = WinHttpOpenRequest(connection, L"GET", L"/", NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (!request) return fail(12);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return fail(13);
    if (!WinHttpReceiveResponse(request, NULL)) return fail(14);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) return fail(15);

    printf("HTTP %lu\n", status);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return status == 200 ? 0 : 16;
}
