/*
 * Unit test suite for cryptnet.dll
 *
 * Copyright 2007 Juan Lang
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
#define CERT_REVOCATION_PARA_HAS_EXTRA_FIELDS

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <winsock2.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <wininet.h>
#include <wincrypt.h>
#include "wine/test.h"
#include "revocation_fixtures.h"

static const BYTE bigCert[] = {
0x30,0x78,0x02,0x01,0x01,0x30,0x02,0x06,0x00,0x30,0x14,0x31,0x12,0x30,0x10,
0x06,0x03,0x55,0x04,0x03,0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,0x61,0x6e,
0x67,0x30,0x22,0x18,0x0f,0x31,0x36,0x30,0x31,0x30,0x31,0x30,0x31,0x30,0x30,
0x30,0x30,0x30,0x30,0x5a,0x18,0x0f,0x31,0x36,0x30,0x31,0x30,0x31,0x30,0x31,
0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x30,0x14,0x31,0x12,0x30,0x10,0x06,0x03,
0x55,0x04,0x03,0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,0x61,0x6e,0x67,0x30,
0x07,0x30,0x02,0x06,0x00,0x03,0x01,0x00,0xa3,0x16,0x30,0x14,0x30,0x12,0x06,
0x03,0x55,0x1d,0x13,0x01,0x01,0xff,0x04,0x08,0x30,0x06,0x01,0x01,0xff,0x02,
0x01,0x01};
static const BYTE certWithIssuingDistPoint[] = {
0x30,0x81,0x99,0xa0,0x03,0x02,0x01,0x02,0x02,0x01,0x01,0x30,0x0d,0x06,0x09,
0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,0x00,0x30,0x14,0x31,0x12,
0x30,0x10,0x06,0x03,0x55,0x04,0x03,0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,
0x61,0x6e,0x67,0x30,0x22,0x18,0x0f,0x31,0x36,0x30,0x31,0x30,0x31,0x30,0x31,
0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x18,0x0f,0x31,0x36,0x30,0x31,0x30,0x31,
0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x30,0x14,0x31,0x12,0x30,0x10,
0x06,0x03,0x55,0x04,0x03,0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,0x61,0x6e,
0x67,0x30,0x07,0x30,0x02,0x06,0x00,0x03,0x01,0x00,0xa3,0x27,0x30,0x25,0x30,
0x23,0x06,0x03,0x55,0x1d,0x1c,0x01,0x01,0xff,0x04,0x19,0x30,0x17,0xa0,0x15,
0xa0,0x13,0x86,0x11,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x77,0x69,0x6e,0x65,
0x68,0x71,0x2e,0x6f,0x72,0x67, };
static const BYTE certWithCRLDistPoint[] = {
0x30,0x81,0x9b,0xa0,0x03,0x02,0x01,0x02,0x02,0x01,0x01,0x30,0x0d,0x06,0x09,
0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,0x00,0x30,0x14,0x31,0x12,
0x30,0x10,0x06,0x03,0x55,0x04,0x03,0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,
0x61,0x6e,0x67,0x30,0x22,0x18,0x0f,0x31,0x36,0x30,0x31,0x30,0x31,0x30,0x31,
0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x18,0x0f,0x31,0x36,0x30,0x31,0x30,0x31,
0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x30,0x14,0x31,0x12,0x30,0x10,
0x06,0x03,0x55,0x04,0x03,0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,0x61,0x6e,
0x67,0x30,0x07,0x30,0x02,0x06,0x00,0x03,0x01,0x00,0xa3,0x29,0x30,0x27,0x30,
0x25,0x06,0x03,0x55,0x1d,0x1f,0x01,0x01,0xff,0x04,0x1b,0x30,0x19,0x30,0x17,
0xa0,0x15,0xa0,0x13,0x86,0x11,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x77,0x69,
0x6e,0x65,0x68,0x71,0x2e,0x6f,0x72,0x67, };
static const BYTE certWithAIAWithCAIssuers[] = {
0x30,0x82,0x01,0x3c,0xa0,0x03,0x02,0x01,0x02,0x02,0x01,0x01,0x30,0x0b,0x06,
0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x30,0x14,0x31,0x12,0x30,
0x10,0x06,0x03,0x55,0x04,0x03,0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,0x61,
0x6e,0x67,0x30,0x1e,0x17,0x0d,0x30,0x39,0x31,0x30,0x32,0x38,0x30,0x30,0x30,
0x30,0x30,0x30,0x5a,0x17,0x0d,0x32,0x30,0x31,0x31,0x32,0x37,0x30,0x30,0x30,
0x30,0x30,0x30,0x5a,0x30,0x14,0x31,0x12,0x30,0x10,0x06,0x03,0x55,0x04,0x03,
0x13,0x09,0x4a,0x75,0x61,0x6e,0x20,0x4c,0x61,0x6e,0x67,0x30,0x81,0xa5,0x30,
0x0b,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x03,0x81,0x95,
0x00,0x06,0x02,0x00,0x00,0x00,0x24,0x00,0x00,0x52,0x53,0x41,0x31,0x00,0x04,
0x00,0x00,0x01,0x00,0x01,0x00,0x2f,0xb2,0x8c,0xff,0x6c,0xf1,0xb1,0x61,0x9c,
0x3a,0x8f,0x5e,0x35,0x2f,0x1f,0xd5,0xcf,0x2a,0xf6,0x9e,0x37,0xe8,0x89,0xa2,
0xb1,0x1c,0xc0,0x1c,0xb6,0x72,0x45,0x97,0xe5,0x88,0x3d,0xfe,0xa6,0x27,0xea,
0xd6,0x07,0x0f,0xcd,0xba,0x49,0x06,0x16,0xdb,0xad,0x06,0x76,0x39,0x4c,0x15,
0xdf,0xe2,0x07,0xc5,0x99,0x1b,0x98,0x4b,0xc3,0x8e,0x89,0x12,0x95,0x9e,0x3b,
0xb9,0x59,0xfe,0x91,0x33,0xc1,0x1f,0xce,0x8f,0xab,0x93,0x25,0x01,0x3e,0xde,
0xf1,0x58,0x3b,0xe7,0x7a,0x03,0x14,0x07,0x09,0x0a,0x21,0x2d,0x12,0x11,0x08,
0x78,0x07,0x9e,0x34,0xc3,0xc5,0xde,0xb2,0xd8,0xd7,0x86,0x0d,0x0d,0xcd,0x81,
0xa4,0x2d,0x7c,0x82,0x50,0xca,0x2a,0xc2,0x99,0xe5,0xf3,0xca,0x7e,0xad,0xa3,
0x31,0x30,0x2f,0x30,0x2d,0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x01,0x01,
0x04,0x21,0x30,0x1f,0x30,0x1d,0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x30,
0x02,0x86,0x11,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x77,0x69,0x6e,0x65,0x68,
0x71,0x2e,0x6f,0x72,0x67 };

struct http_server
{
    HANDLE thread;
    struct http_server_context *context;
};

struct http_server_context
{
    SOCKET listener;
    SOCKET client;
    char *headers;
    BYTE *payload;
    DWORD payload_size;
    DWORD delay;
};

static BOOL socket_send_all(SOCKET socket, const void *buffer, DWORD size)
{
    const char *ptr = buffer;
    int count;

    while (size)
    {
        count = send(socket, ptr, min(size, 0x4000), 0);
        if (count <= 0)
            return FALSE;
        ptr += count;
        size -= count;
    }
    return TRUE;
}

static DWORD WINAPI http_server_thread(void *arg)
{
    struct http_server_context *server = arg;
    char request[2048];
    DWORD offset;
    int count;

    server->client = accept(server->listener, NULL, NULL);
    closesocket(server->listener);
    server->listener = INVALID_SOCKET;
    if (server->client == INVALID_SOCKET)
        return 1;

    count = recv(server->client, request, sizeof(request), 0);
    if (count <= 0)
        goto done;
    if (server->delay)
        Sleep(server->delay);
    if (!socket_send_all(server->client, server->headers,
            strlen(server->headers)))
        goto done;
    for (offset = 0; offset < server->payload_size; offset += count)
    {
        count = min(server->payload_size - offset, 137);
        if (!socket_send_all(server->client, server->payload + offset, count))
            break;
    }

done:
    shutdown(server->client, SD_BOTH);
    closesocket(server->client);
    server->client = INVALID_SOCKET;
    return 0;
}

static BOOL start_http_server(struct http_server *server, const char *headers,
        const BYTE *payload, DWORD payload_size, DWORD delay, USHORT *port)
{
    struct http_server_context *context;
    struct sockaddr_in address;
    int address_size = sizeof(address);

    memset(server, 0, sizeof(*server));
    if (!(context = calloc(1, sizeof(*context))))
        return FALSE;
    context->listener = INVALID_SOCKET;
    context->client = INVALID_SOCKET;
    if (!(context->headers = _strdup(headers)) ||
            (payload_size && !(context->payload = malloc(payload_size))))
    {
        free(context->headers);
        free(context);
        return FALSE;
    }
    if (payload_size) memcpy(context->payload, payload, payload_size);
    context->payload_size = payload_size;
    context->delay = delay;
    context->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (context->listener == INVALID_SOCKET)
    {
        free(context->payload);
        free(context->headers);
        free(context);
        return FALSE;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(context->listener, (struct sockaddr *)&address, sizeof(address)) ||
            listen(context->listener, 1) || getsockname(context->listener,
                (struct sockaddr *)&address, &address_size))
    {
        closesocket(context->listener);
        free(context->payload);
        free(context->headers);
        free(context);
        return FALSE;
    }
    *port = ntohs(address.sin_port);
    server->context = context;
    server->thread = CreateThread(NULL, 0, http_server_thread, context, 0, NULL);
    if (!server->thread)
    {
        closesocket(context->listener);
        free(context->payload);
        free(context->headers);
        free(context);
        server->context = NULL;
        return FALSE;
    }
    return TRUE;
}

static void stop_http_server(struct http_server *server)
{
    DWORD wait;

    if (!server->thread) return;
    wait = WaitForSingleObject(server->thread, 10000);
    if (wait == WAIT_TIMEOUT)
    {
        ok(0, "HTTP server thread exceeded its deadline\n");
        if (TerminateThread(server->thread, 1))
            wait = WaitForSingleObject(server->thread, 5000);
    }
    ok(wait == WAIT_OBJECT_0, "HTTP server thread did not terminate, wait %#lx\n",
            wait);
    if (wait == WAIT_OBJECT_0 && server->context)
    {
        if (server->context->client != INVALID_SOCKET)
            closesocket(server->context->client);
        if (server->context->listener != INVALID_SOCKET)
            closesocket(server->context->listener);
        free(server->context->payload);
        free(server->context->headers);
        free(server->context);
        server->context = NULL;
    }
    CloseHandle(server->thread);
    server->thread = NULL;
}

static BOOL url_cache_entry_exists(const char *url)
{
    DWORD size = 0;

    SetLastError(ERROR_SUCCESS);
    return GetUrlCacheEntryInfoA(url, NULL, &size) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER;
}

static void compareUrlArray(const CRYPT_URL_ARRAY *expected,
 const CRYPT_URL_ARRAY *got)
{
    ok(expected->cUrl == got->cUrl, "Expected %ld URLs, got %ld\n",
     expected->cUrl, got->cUrl);
    if (expected->cUrl == got->cUrl)
    {
        DWORD i;

        for (i = 0; i < got->cUrl; i++)
            ok(!lstrcmpiW(expected->rgwszUrl[i], got->rgwszUrl[i]),
             "%ld: unexpected URL\n", i);
    }
}


static void test_getObjectUrl(void)
{
    static WCHAR url[] = L"http://winehq.org";
    BOOL ret;
    DWORD urlArraySize = 0, infoSize = 0;
    PCCERT_CONTEXT cert;

    SetLastError(0xdeadbeef);
    ret = CryptGetObjectUrl(NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL);
    ok(!ret && GetLastError() == ERROR_FILE_NOT_FOUND,
     "Expected ERROR_FILE_NOT_FOUND, got %ld\n", GetLastError());
    /* Crash
    ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, NULL, 0, NULL, NULL,
     NULL, NULL, NULL);
    ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, NULL, 0, NULL, NULL,
     NULL, &infoSize, NULL);
    ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, NULL, 0, NULL,
     &urlArraySize, NULL, &infoSize, NULL);
     */
    /* A cert with no CRL dist point extension fails.. */
    cert = CertCreateCertificateContext(X509_ASN_ENCODING, bigCert,
     sizeof(bigCert));
    SetLastError(0xdeadbeef);
    ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, (void *)cert, 0, NULL,
     NULL, NULL, NULL, NULL);
    ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
     "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
    CertFreeCertificateContext(cert);

    cert = CertCreateCertificateContext(X509_ASN_ENCODING,
     certWithIssuingDistPoint, sizeof(certWithIssuingDistPoint));
    if (cert)
    {
        /* This cert has no AIA extension, so expect this to fail */
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, (void *)cert, 0,
         NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, (void *)cert,
         CRYPT_GET_URL_FROM_PROPERTY, NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, (void *)cert,
         CRYPT_GET_URL_FROM_EXTENSION, NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        /* It does have an issuing dist point extension, but that's not what
         * this is looking for (it wants a CRL dist points extension)
         */
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, 0, NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, CRYPT_GET_URL_FROM_PROPERTY, NULL, NULL, NULL, NULL,
         NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, CRYPT_GET_URL_FROM_EXTENSION, NULL, NULL, NULL, NULL,
         NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        CertFreeCertificateContext(cert);
    }
    cert = CertCreateCertificateContext(X509_ASN_ENCODING,
     certWithCRLDistPoint, sizeof(certWithCRLDistPoint));
    if (cert)
    {
        PCRYPT_URL_ARRAY urlArray;

        /* This cert has no AIA extension, so expect this to fail */
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, (void *)cert, 0,
         NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, (void *)cert,
         CRYPT_GET_URL_FROM_PROPERTY, NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER, (void *)cert,
         CRYPT_GET_URL_FROM_EXTENSION, NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        /* It does have a CRL dist points extension */
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, 0, NULL, NULL, NULL, NULL, NULL);
        ok(!ret && GetLastError() == E_INVALIDARG,
         "Expected E_INVALIDARG, got %08lx\n", GetLastError());
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, 0, NULL, NULL, NULL, &infoSize, NULL);
        ok(!ret && GetLastError() == E_INVALIDARG,
         "Expected E_INVALIDARG, got %08lx\n", GetLastError());
        /* Can get it without specifying the location: */
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, 0, NULL, &urlArraySize, NULL, NULL, NULL);
        ok(ret, "CryptGetObjectUrl failed: %08lx\n", GetLastError());
        urlArray = HeapAlloc(GetProcessHeap(), 0, urlArraySize);
        if (urlArray)
        {
            ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
             (void *)cert, 0, urlArray, &urlArraySize, NULL, NULL, NULL);
            ok(ret, "CryptGetObjectUrl failed: %08lx\n", GetLastError());
            if (ret)
            {
                LPWSTR pUrl = url;
                CRYPT_URL_ARRAY expectedUrl = { 1, &pUrl };

                compareUrlArray(&expectedUrl, urlArray);
            }
            HeapFree(GetProcessHeap(), 0, urlArray);
        }
        /* or by specifying it's an extension: */
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, CRYPT_GET_URL_FROM_EXTENSION, NULL, &urlArraySize, NULL,
         NULL, NULL);
        ok(ret, "CryptGetObjectUrl failed: %08lx\n", GetLastError());
        urlArray = HeapAlloc(GetProcessHeap(), 0, urlArraySize);
        if (urlArray)
        {
            ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
             (void *)cert, CRYPT_GET_URL_FROM_EXTENSION, urlArray,
             &urlArraySize, NULL, NULL, NULL);
            ok(ret, "CryptGetObjectUrl failed: %08lx\n", GetLastError());
            if (ret)
            {
                LPWSTR pUrl = url;
                CRYPT_URL_ARRAY expectedUrl = { 1, &pUrl };

                compareUrlArray(&expectedUrl, urlArray);
            }
            HeapFree(GetProcessHeap(), 0, urlArray);
        }
        /* but it isn't contained in a property: */
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, CRYPT_GET_URL_FROM_PROPERTY, NULL, &urlArraySize, NULL,
         NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "Expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        CertFreeCertificateContext(cert);
    }
    cert = CertCreateCertificateContext(X509_ASN_ENCODING,
     certWithAIAWithCAIssuers, sizeof(certWithAIAWithCAIssuers));
    if (cert)
    {
        PCRYPT_URL_ARRAY urlArray;

        /* This has an AIA extension with the CA Issuers set, so expect it
         * to succeed:
         */
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER,
         (void *)cert, 0, NULL, &urlArraySize, NULL, NULL, NULL);
        ok(ret, "CryptGetObjectUrl failed: %08lx\n", GetLastError());
        if (ret)
        {
            urlArray = HeapAlloc(GetProcessHeap(), 0, urlArraySize);
            if (urlArray)
            {
                ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_ISSUER,
                 (void *)cert, CRYPT_GET_URL_FROM_EXTENSION, urlArray,
                 &urlArraySize, NULL, NULL, NULL);
                ok(ret, "CryptGetObjectUrl failed: %08lx\n", GetLastError());
                if (ret)
                {
                    LPWSTR pUrl = url;
                    CRYPT_URL_ARRAY expectedUrl = { 1, &pUrl };

                    compareUrlArray(&expectedUrl, urlArray);
                }
                HeapFree(GetProcessHeap(), 0, urlArray);
            }
        }
        /* It doesn't have a CRL dist points extension, so this should fail */
        SetLastError(0xdeadbeef);
        ret = CryptGetObjectUrl(URL_OID_CERTIFICATE_CRL_DIST_POINT,
         (void *)cert, 0, NULL, &urlArraySize, NULL, NULL, NULL);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND,
         "expected CRYPT_E_NOT_FOUND, got %08lx\n", GetLastError());
        CertFreeCertificateContext(cert);
    }
}

static void make_tmp_file(LPSTR path)
{
    static char curr[MAX_PATH] = { 0 };
    char temp[MAX_PATH];
    DWORD dwNumberOfBytesWritten;
    HANDLE hf;

    if (!*curr)
        GetCurrentDirectoryA(MAX_PATH, curr);
    GetTempFileNameA(curr, "net", 0, temp);
    lstrcpyA(path, temp);
    hf = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
     FILE_ATTRIBUTE_NORMAL, NULL);
    WriteFile(hf, certWithCRLDistPoint, sizeof(certWithCRLDistPoint),
     &dwNumberOfBytesWritten, NULL);
    CloseHandle(hf);
}

static void test_retrieveObjectByUrl(void)
{
    BOOL ret;
    char tmpfile[MAX_PATH], url[MAX_PATH + 8];
    CRYPT_BLOB_ARRAY *pBlobArray;
    PCCERT_CONTEXT cert;
    PCCRL_CONTEXT crl;
    HCERTSTORE store;
    CRYPT_RETRIEVE_AUX_INFO aux = { 0 };
    FILETIME ft = { 0 };

    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL);
    ok(!ret && (GetLastError() == ERROR_INVALID_PARAMETER ||
                GetLastError() == E_INVALIDARG),
       "got 0x%lx/%lu (expected ERROR_INVALID_PARAMETER or E_INVALIDARG)\n",
       GetLastError(), GetLastError());

    make_tmp_file(tmpfile);
    sprintf(url, "file://%s", tmpfile);

    pBlobArray = (CRYPT_BLOB_ARRAY *)0xdeadbeef;
    ret = CryptRetrieveObjectByUrlA(url, NULL, 0, 0, (void **)&pBlobArray,
     NULL, NULL, NULL, NULL);
    if (!ret)
    {
        /* File URL support was apparently removed in Vista/Windows 2008 */
        win_skip("File URLs not supported\n");
        return;
    }
    ok(ret, "CryptRetrieveObjectByUrlA failed: %ld\n", GetLastError());
    ok(pBlobArray && pBlobArray != (CRYPT_BLOB_ARRAY *)0xdeadbeef,
     "Expected a valid pointer\n");
    if (pBlobArray && pBlobArray != (CRYPT_BLOB_ARRAY *)0xdeadbeef)
    {
        ok(pBlobArray->cBlob == 1, "Expected 1 blob, got %ld\n",
         pBlobArray->cBlob);
        ok(pBlobArray->rgBlob[0].cbData == sizeof(certWithCRLDistPoint),
         "Unexpected size %ld\n", pBlobArray->rgBlob[0].cbData);
        CryptMemFree(pBlobArray);
    }
    cert = (PCCERT_CONTEXT)0xdeadbeef;
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CERTIFICATE, 0, 0,
     (void **)&cert, NULL, NULL, NULL, NULL);
    ok(ret, "CryptRetrieveObjectByUrlA failed: %ld\n", GetLastError());
    ok(cert && cert != (PCCERT_CONTEXT)0xdeadbeef, "Expected a cert\n");
    if (cert && cert != (PCCERT_CONTEXT)0xdeadbeef)
        CertFreeCertificateContext(cert);
    crl = (PCCRL_CONTEXT)0xdeadbeef;
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CRL, 0, 0, (void **)&crl,
     NULL, NULL, NULL, NULL);
    /* w2k3,XP, newer w2k: CRYPT_E_NO_MATCH, older w2k: CRYPT_E_ASN1_BADTAG
     * or OSS_DATA_ERROR.
     */
    ok(!ret && (GetLastError() == CRYPT_E_NO_MATCH ||
                broken(GetLastError() == CRYPT_E_ASN1_BADTAG ||
                       GetLastError() == OSS_DATA_ERROR)),
        "got 0x%lx/%lu (expected CRYPT_E_NO_MATCH)\n", GetLastError(), GetLastError());

    /* only newer versions of cryptnet do the cleanup */
    if(!ret && GetLastError() != CRYPT_E_ASN1_BADTAG &&
               GetLastError() != OSS_DATA_ERROR) {
        ok(crl == NULL, "Expected CRL to be NULL\n");
    }

    if (crl && crl != (PCCRL_CONTEXT)0xdeadbeef)
        CertFreeCRLContext(crl);
    store = (HCERTSTORE)0xdeadbeef;
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CAPI2_ANY, 0, 0,
     &store, NULL, NULL, NULL, NULL);
    ok(ret, "CryptRetrieveObjectByUrlA failed: %ld\n", GetLastError());
    if (store && store != (HCERTSTORE)0xdeadbeef)
    {
        DWORD certs = 0;

        cert = NULL;
        do {
            cert = CertEnumCertificatesInStore(store, cert);
            if (cert)
                certs++;
        } while (cert);
        ok(certs == 1, "Expected 1 cert, got %ld\n", certs);
        CertCloseStore(store, 0);
    }
    /* Are file URLs cached? */
    cert = (PCCERT_CONTEXT)0xdeadbeef;
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CERTIFICATE,
     CRYPT_CACHE_ONLY_RETRIEVAL, 0, (void **)&cert, NULL, NULL, NULL, NULL);
    ok(ret, "CryptRetrieveObjectByUrlA failed: %08lx\n", GetLastError());
    if (cert && cert != (PCCERT_CONTEXT)0xdeadbeef)
        CertFreeCertificateContext(cert);

    cert = (PCCERT_CONTEXT)0xdeadbeef;
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CERTIFICATE, 0, 0,
     (void **)&cert, NULL, NULL, NULL, &aux);
    /* w2k: failure with E_INVALIDARG */
    ok(ret || broken(GetLastError() == E_INVALIDARG),
       "got %u with 0x%lx/%lu (expected '!=0' or '0' with E_INVALIDARG)\n",
       ret, GetLastError(), GetLastError());
    if (cert && cert != (PCCERT_CONTEXT)0xdeadbeef)
        CertFreeCertificateContext(cert);

    cert = (PCCERT_CONTEXT)0xdeadbeef;
    aux.cbSize = sizeof(aux);
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CERTIFICATE, 0, 0,
     (void **)&cert, NULL, NULL, NULL, &aux);
    /* w2k: failure with E_INVALIDARG */
    ok(ret || broken(GetLastError() == E_INVALIDARG),
       "got %u with 0x%lx/%lu (expected '!=0' or '0' with E_INVALIDARG)\n",
       ret, GetLastError(), GetLastError());
    if (!ret) {
        /* no more tests useful */
        DeleteFileA(tmpfile);
        skip("no usable CertificateContext\n");
        return;
    }
    CertFreeCertificateContext(cert);

    aux.pLastSyncTime = &ft;
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CERTIFICATE, 0, 0,
     (void **)&cert, NULL, NULL, NULL, &aux);
    ok(ret, "CryptRetrieveObjectByUrlA failed: %08lx\n", GetLastError());
    CertFreeCertificateContext(cert);
    ok(ft.dwLowDateTime || ft.dwHighDateTime,
     "Expected last sync time to be set\n");

    aux.dwMaxUrlRetrievalByteCount = sizeof(certWithCRLDistPoint) - 1;
    cert = NULL;
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CERTIFICATE,
            CRYPT_DONT_CACHE_RESULT, 0, (void **)&cert, NULL, NULL, NULL,
            &aux);
    ok(!ret, "unexpectedly decoded an object above the caller's size limit\n");
    if (winetest_platform_is_wine)
        ok(GetLastError() == ERROR_FILE_TOO_LARGE,
                "got error %#lx, expected ERROR_FILE_TOO_LARGE\n", GetLastError());
    if (cert)
        CertFreeCertificateContext(cert);

    DeleteFileA(tmpfile);
    /* Okay, after being deleted, are file URLs still cached? */
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CERTIFICATE,
     CRYPT_CACHE_ONLY_RETRIEVAL, 0, (void **)&cert, NULL, NULL, NULL, NULL);
    ok(!ret && (GetLastError() == ERROR_FILE_NOT_FOUND ||
     GetLastError() == ERROR_PATH_NOT_FOUND),
     "Expected ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND, got %ld\n",
     GetLastError());
}

static void test_large_crl_retrieval(void)
{
    LARGE_INTEGER file_size;
    PCCRL_CONTEXT crl = NULL;
    char tmpfile[MAX_PATH], url[MAX_PATH + 8];
    HANDLE file;
    DWORD error;
    BOOL ret;

    make_tmp_file(tmpfile);
    file = CreateFileA(tmpfile, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
    ok(file != INVALID_HANDLE_VALUE, "failed to open temporary file, error %lu\n",
            GetLastError());
    if (file == INVALID_HANDLE_VALUE)
    {
        DeleteFileA(tmpfile);
        return;
    }

    file_size.QuadPart = 32 * 1024 * 1024 + 1;
    ret = SetFilePointerEx(file, file_size, NULL, FILE_BEGIN);
    ok(ret, "failed to extend temporary file, error %lu\n", GetLastError());
    if (ret)
    {
        ret = SetEndOfFile(file);
        ok(ret, "failed to set temporary file size, error %lu\n", GetLastError());
    }
    CloseHandle(file);
    if (!ret)
    {
        DeleteFileA(tmpfile);
        return;
    }

    sprintf(url, "file://%s", tmpfile);
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, CONTEXT_OID_CRL,
            CRYPT_DONT_CACHE_RESULT, 0, (void **)&crl, NULL, NULL, NULL, NULL);
    error = GetLastError();
    ok(!ret, "unexpectedly decoded an oversized CRL\n");
    if (winetest_platform_is_wine)
        ok(error == ERROR_FILE_TOO_LARGE, "got error %#lx, expected ERROR_FILE_TOO_LARGE\n",
                error);
    if (crl)
        CertFreeCRLContext(crl);
    DeleteFileA(tmpfile);
}

static void test_http_retrieval_limits(void)
{
    static const char success_headers[] =
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
    static const char sized_headers[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 8192\r\nConnection: close\r\n\r\n";
    static const char missing_headers[] =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    BYTE payload[8192];
    CRYPT_BLOB_ARRAY *blobs;
    CRYPT_RETRIEVE_AUX_INFO aux = {sizeof(aux)};
    struct http_server server;
    WSADATA wsa_data;
    char url[128];
    DWORD error, i, start, elapsed;
    USHORT port;
    BOOL ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
    {
        win_skip("Winsock is unavailable\n");
        return;
    }
    for (i = 0; i < ARRAY_SIZE(payload); ++i)
        payload[i] = i * 37 + 11;

    if (!start_http_server(&server, success_headers, payload,
            sizeof(payload), 0, &port))
    {
        win_skip("failed to start local HTTP server\n");
        WSACleanup();
        return;
    }
    sprintf(url, "http://127.0.0.1:%u/object", port);
    aux.dwMaxUrlRetrievalByteCount = sizeof(payload);
    blobs = NULL;
    ret = CryptRetrieveObjectByUrlA(url, NULL,
            CRYPT_WIRE_ONLY_RETRIEVAL | CRYPT_DONT_CACHE_RESULT, 3000,
            (void **)&blobs, NULL, NULL, NULL, &aux);
    ok(ret, "HTTP retrieval failed, error %#lx\n", GetLastError());
    if (ret)
    {
        ok(blobs->cBlob == 1, "got %lu blobs\n", blobs->cBlob);
        if (blobs->cBlob == 1)
        {
            ok(blobs->rgBlob[0].cbData == sizeof(payload), "got %lu bytes\n",
                    blobs->rgBlob[0].cbData);
            ok(!memcmp(blobs->rgBlob[0].pbData, payload, sizeof(payload)),
                    "retrieved payload does not match\n");
        }
        CryptMemFree(blobs);
    }
    ok(!url_cache_entry_exists(url),
            "CRYPT_DONT_CACHE_RESULT created a URL cache entry\n");
    stop_http_server(&server);

    if (!start_http_server(&server, success_headers, payload,
            sizeof(payload), 0, &port))
    {
        win_skip("failed to start local HTTP server\n");
        WSACleanup();
        return;
    }
    sprintf(url, "http://127.0.0.1:%u/stream", port);
    aux.dwMaxUrlRetrievalByteCount = sizeof(payload) / 2;
    blobs = NULL;
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, NULL,
            CRYPT_WIRE_ONLY_RETRIEVAL | CRYPT_DONT_CACHE_RESULT, 3000,
            (void **)&blobs, NULL, NULL, NULL, &aux);
    error = GetLastError();
    ok(!ret, "streamed a response above the caller's size limit\n");
    ok(error == ERROR_FILE_TOO_LARGE, "got error %#lx\n", error);
    if (blobs) CryptMemFree(blobs);
    stop_http_server(&server);

    if (!start_http_server(&server, sized_headers, payload,
            sizeof(payload), 0, &port))
    {
        win_skip("failed to start local HTTP server\n");
        WSACleanup();
        return;
    }
    sprintf(url, "http://127.0.0.1:%u/object", port);
    aux.dwMaxUrlRetrievalByteCount = sizeof(payload) / 2;
    blobs = NULL;
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, NULL,
            CRYPT_WIRE_ONLY_RETRIEVAL | CRYPT_DONT_CACHE_RESULT, 3000,
            (void **)&blobs, NULL, NULL, NULL, &aux);
    error = GetLastError();
    ok(!ret, "retrieved a response above the caller's size limit\n");
    ok(error == ERROR_FILE_TOO_LARGE, "got error %#lx\n", error);
    if (blobs) CryptMemFree(blobs);
    stop_http_server(&server);

    if (!start_http_server(&server, missing_headers, NULL, 0, 0, &port))
    {
        win_skip("failed to start local HTTP server\n");
        WSACleanup();
        return;
    }
    sprintf(url, "http://127.0.0.1:%u/missing", port);
    aux.dwMaxUrlRetrievalByteCount = sizeof(payload);
    blobs = NULL;
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, NULL,
            CRYPT_WIRE_ONLY_RETRIEVAL | CRYPT_DONT_CACHE_RESULT, 3000,
            (void **)&blobs, NULL, NULL, NULL, &aux);
    error = GetLastError();
    ok(!ret, "accepted an HTTP error response\n");
    ok(error == ERROR_HTTP_INVALID_SERVER_RESPONSE, "got error %#lx\n", error);
    if (blobs) CryptMemFree(blobs);
    stop_http_server(&server);

    if (!start_http_server(&server, success_headers, payload,
            sizeof(payload), 350, &port))
    {
        win_skip("failed to start local HTTP server\n");
        WSACleanup();
        return;
    }
    sprintf(url, "http://127.0.0.1:%u/slow", port);
    blobs = NULL;
    start = GetTickCount();
    SetLastError(0xdeadbeef);
    ret = CryptRetrieveObjectByUrlA(url, NULL,
            CRYPT_WIRE_ONLY_RETRIEVAL | CRYPT_DONT_CACHE_RESULT, 100,
            (void **)&blobs, NULL, NULL, NULL, &aux);
    elapsed = GetTickCount() - start;
    error = GetLastError();
    ok(!ret, "slow HTTP retrieval unexpectedly succeeded\n");
    ok(error == ERROR_TIMEOUT || error == ERROR_INTERNET_TIMEOUT,
            "got error %#lx\n", error);
    ok(elapsed < 1500, "retrieval exceeded its cumulative deadline (%lu ms)\n",
            elapsed);
    if (blobs) CryptMemFree(blobs);
    stop_http_server(&server);

    WSACleanup();
}

static const BYTE rootWithKeySignAndCRLSign[] = {
0x30,0x82,0x01,0xdf,0x30,0x82,0x01,0x4c,0xa0,0x03,0x02,0x01,0x02,0x02,0x10,
0x5b,0xc7,0x0b,0x27,0x99,0xbb,0x2e,0x99,0x47,0x9d,0x45,0x4e,0x7c,0x1a,0xca,
0xe8,0x30,0x09,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1d,0x05,0x00,0x30,0x10,0x31,
0x0e,0x30,0x0c,0x06,0x03,0x55,0x04,0x03,0x13,0x05,0x43,0x65,0x72,0x74,0x31,
0x30,0x1e,0x17,0x0d,0x30,0x37,0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,
0x30,0x5a,0x17,0x0d,0x30,0x37,0x31,0x32,0x33,0x31,0x32,0x33,0x35,0x39,0x35,
0x39,0x5a,0x30,0x10,0x31,0x0e,0x30,0x0c,0x06,0x03,0x55,0x04,0x03,0x13,0x05,
0x43,0x65,0x72,0x74,0x31,0x30,0x81,0x9f,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,
0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00,0x03,0x81,0x8d,0x00,0x30,0x81,0x89,
0x02,0x81,0x81,0x00,0xad,0x7e,0xca,0xf3,0xe5,0x99,0xc2,0x2a,0xca,0x50,0x82,
0x7c,0x2d,0xa4,0x81,0xcd,0x0d,0x0d,0x86,0xd7,0xd8,0xb2,0xde,0xc5,0xc3,0x34,
0x9e,0x07,0x78,0x08,0x11,0x12,0x2d,0x21,0x0a,0x09,0x07,0x14,0x03,0x7a,0xe7,
0x3b,0x58,0xf1,0xde,0x3e,0x01,0x25,0x93,0xab,0x8f,0xce,0x1f,0xc1,0x33,0x91,
0xfe,0x59,0xb9,0x3b,0x9e,0x95,0x12,0x89,0x8e,0xc3,0x4b,0x98,0x1b,0x99,0xc5,
0x07,0xe2,0xdf,0x15,0x4c,0x39,0x76,0x06,0xad,0xdb,0x16,0x06,0x49,0xba,0xcd,
0x0f,0x07,0xd6,0xea,0x27,0xa6,0xfe,0x3d,0x88,0xe5,0x97,0x45,0x72,0xb6,0x1c,
0xc0,0x1c,0xb1,0xa2,0x89,0xe8,0x37,0x9e,0xf6,0x2a,0xcf,0xd5,0x1f,0x2f,0x35,
0x5e,0x8f,0x3a,0x9c,0x61,0xb1,0xf1,0x6c,0xff,0x8c,0xb2,0x2f,0x02,0x03,0x01,
0x00,0x01,0xa3,0x42,0x30,0x40,0x30,0x0e,0x06,0x03,0x55,0x1d,0x0f,0x01,0x01,
0xff,0x04,0x04,0x03,0x02,0x00,0x06,0x30,0x0f,0x06,0x03,0x55,0x1d,0x13,0x01,
0x01,0xff,0x04,0x05,0x30,0x03,0x01,0x01,0xff,0x30,0x1d,0x06,0x03,0x55,0x1d,
0x0e,0x04,0x16,0x04,0x14,0x14,0x8c,0x16,0xbb,0xbe,0x70,0xa2,0x28,0x89,0xa0,
0x58,0xff,0x98,0xbd,0xa8,0x24,0x2b,0x8a,0xe9,0x9a,0x30,0x09,0x06,0x05,0x2b,
0x0e,0x03,0x02,0x1d,0x05,0x00,0x03,0x81,0x81,0x00,0x74,0xcb,0x21,0xfd,0x2d,
0x25,0xdc,0xa5,0xaa,0xa1,0x26,0xdc,0x8b,0x40,0x11,0x64,0xae,0x5c,0x71,0x3c,
0x28,0xbc,0xf9,0xb3,0xcb,0xa5,0x94,0xb2,0x8d,0x4c,0x23,0x2b,0x9b,0xde,0x2c,
0x4c,0x30,0x04,0xc6,0x88,0x10,0x2f,0x53,0xfd,0x6c,0x82,0xf1,0x13,0xfb,0xda,
0x27,0x75,0x25,0x48,0xe4,0x72,0x09,0x2a,0xee,0xb4,0x1e,0xc9,0x55,0xf5,0xf7,
0x82,0x91,0xd8,0x4b,0xe4,0x3a,0xfe,0x97,0x87,0xdf,0xfb,0x15,0x5a,0x12,0x3e,
0x12,0xe6,0xad,0x40,0x0b,0xcf,0xee,0x1a,0x44,0xe0,0x83,0xb2,0x67,0x94,0xd4,
0x2e,0x7c,0xf2,0x06,0x9d,0xb3,0x3b,0x7e,0x2f,0xda,0x25,0x66,0x7e,0xa7,0x1f,
0x45,0xd4,0xf5,0xe3,0xdf,0x2a,0xf1,0x18,0x28,0x20,0xb5,0xf8,0xf5,0x8d,0x7a,
0x2e,0x84,0xee };
static const BYTE revokedCert[] = {
0x30,0x82,0x01,0xb9,0x30,0x82,0x01,0x22,0xa0,0x03,0x02,0x01,0x02,0x02,0x01,
0x01,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,
0x00,0x30,0x10,0x31,0x0e,0x30,0x0c,0x06,0x03,0x55,0x04,0x03,0x13,0x05,0x43,
0x65,0x72,0x74,0x31,0x30,0x1e,0x17,0x0d,0x30,0x37,0x30,0x35,0x30,0x31,0x30,
0x30,0x30,0x30,0x30,0x30,0x5a,0x17,0x0d,0x30,0x37,0x31,0x30,0x30,0x31,0x30,
0x30,0x30,0x30,0x30,0x30,0x5a,0x30,0x10,0x31,0x0e,0x30,0x0c,0x06,0x03,0x55,
0x04,0x03,0x13,0x05,0x43,0x65,0x72,0x74,0x32,0x30,0x81,0x9f,0x30,0x0d,0x06,
0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00,0x03,0x81,0x8d,
0x00,0x30,0x81,0x89,0x02,0x81,0x81,0x00,0xb8,0x52,0xda,0xc5,0x4b,0x3f,0xe5,
0x33,0x0e,0x67,0x5f,0x48,0x21,0xdc,0x7e,0xef,0x37,0x33,0xba,0xff,0xb4,0xc6,
0xdc,0xb6,0x17,0x8e,0x20,0x55,0x07,0x12,0xd2,0x7b,0x3c,0xce,0x30,0xc5,0xa7,
0x48,0x9f,0x6e,0xfe,0xb8,0xbe,0xdb,0x9f,0x9b,0x17,0x60,0x16,0xde,0xc6,0x8b,
0x47,0xd1,0x57,0x71,0x3c,0x93,0xfc,0xbd,0xec,0x44,0x32,0x3b,0xb9,0xcf,0x6b,
0x05,0x72,0xa7,0x87,0x8e,0x7e,0xd4,0x9a,0x87,0x1c,0x2f,0xb7,0x82,0x40,0xfc,
0x6a,0x80,0x83,0x68,0x28,0xce,0x84,0xf4,0x0b,0x2e,0x44,0xcb,0x53,0xac,0x85,
0x85,0xb5,0x46,0x36,0x98,0x3c,0x10,0x02,0xaa,0x02,0xbc,0x8b,0xa2,0x23,0xb2,
0xd3,0x51,0x9a,0x22,0x4a,0xe3,0xaa,0x4e,0x7c,0xda,0x38,0xcf,0x49,0x98,0x72,
0xa3,0x02,0x03,0x01,0x00,0x01,0xa3,0x23,0x30,0x21,0x30,0x1f,0x06,0x03,0x55,
0x1d,0x23,0x04,0x18,0x30,0x18,0x80,0x14,0x14,0x8c,0x16,0xbb,0xbe,0x70,0xa2,
0x28,0x89,0xa0,0x58,0xff,0x98,0xbd,0xa8,0x24,0x2b,0x8a,0xe9,0x9a,0x30,0x0d,
0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,0x00,0x03,0x81,
0x81,0x00,0x8a,0x49,0xa9,0x86,0x5e,0xc9,0x33,0x7e,0xfd,0xab,0x64,0x1f,0x6d,
0x00,0xd7,0x9b,0xec,0xd1,0x5b,0x38,0xcc,0xd6,0xf3,0xf2,0xb4,0x75,0x70,0x00,
0x82,0x9d,0x37,0x58,0xe1,0xcd,0x2c,0x61,0xb3,0x28,0xe7,0x8a,0x00,0xbe,0x6e,
0xca,0xe8,0x55,0xd5,0xad,0x3a,0xea,0xaf,0x13,0x20,0x1c,0x44,0xfc,0xb4,0xf9,
0x29,0x2b,0xdc,0x8a,0x2d,0x1b,0x27,0x9e,0xb9,0x3b,0x4a,0x71,0x9d,0x47,0x7d,
0xf7,0x92,0x6b,0x21,0x7f,0xfa,0x88,0x79,0x94,0x33,0xf6,0xdd,0x92,0x04,0x92,
0xd6,0x5e,0x0a,0x74,0xf2,0x85,0xa6,0xd5,0x3c,0x28,0xc0,0x89,0x5d,0xda,0xf3,
0xa6,0x01,0xc2,0xe9,0xa3,0xc1,0xb7,0x21,0x08,0xba,0x18,0x07,0x45,0xeb,0x77,
0x7d,0xcd,0xc6,0xe7,0x2a,0x7b,0x46,0xd2,0x3d,0xb5 };
static const BYTE unRevokedCert[] = {
0x30,0x82,0x01,0xa2,0x30,0x82,0x01,0x0d,0xa0,0x03,0x02,0x01,0x02,0x02,0x01,
0x02,0x30,0x0b,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x30,
0x10,0x31,0x0e,0x30,0x0c,0x06,0x03,0x55,0x04,0x03,0x13,0x05,0x43,0x65,0x72,
0x74,0x31,0x30,0x1e,0x17,0x0d,0x30,0x37,0x30,0x35,0x30,0x31,0x30,0x30,0x30,
0x30,0x30,0x30,0x5a,0x17,0x0d,0x30,0x37,0x31,0x30,0x30,0x31,0x30,0x30,0x30,
0x30,0x30,0x30,0x5a,0x30,0x24,0x31,0x22,0x30,0x0e,0x06,0x03,0x55,0x04,0x03,
0x13,0x07,0x66,0x6f,0x6f,0x2e,0x63,0x6f,0x6d,0x30,0x10,0x06,0x03,0x55,0x04,
0x03,0x13,0x09,0x2a,0x2e,0x66,0x6f,0x6f,0x2e,0x63,0x6f,0x6d,0x30,0x81,0x9d,
0x30,0x0b,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x03,0x81,
0x8d,0x00,0x30,0x81,0x89,0x02,0x81,0x81,0x00,0xb8,0x52,0xda,0xc5,0x4b,0x3f,
0xe5,0x33,0x0e,0x67,0x5f,0x48,0x21,0xdc,0x7e,0xef,0x37,0x33,0xba,0xff,0xb4,
0xc6,0xdc,0xb6,0x17,0x8e,0x20,0x55,0x07,0x12,0xd2,0x7b,0x3c,0xce,0x30,0xc5,
0xa7,0x48,0x9f,0x6e,0xfe,0xb8,0xbe,0xdb,0x9f,0x9b,0x17,0x60,0x16,0xde,0xc6,
0x8b,0x47,0xd1,0x57,0x71,0x3c,0x93,0xfc,0xbd,0xec,0x44,0x32,0x3b,0xb9,0xcf,
0x6b,0x05,0x72,0xa7,0x87,0x8e,0x7e,0xd4,0x9a,0x87,0x1c,0x2f,0xb7,0x82,0x40,
0xfc,0x6a,0x80,0x83,0x68,0x28,0xce,0x84,0xf4,0x0b,0x2e,0x44,0xcb,0x53,0xac,
0x85,0x85,0xb5,0x46,0x36,0x98,0x3c,0x10,0x02,0xaa,0x02,0xbc,0x8b,0xa2,0x23,
0xb2,0xd3,0x51,0x9a,0x22,0x4a,0xe3,0xaa,0x4e,0x7c,0xda,0x38,0xcf,0x49,0x98,
0x72,0xa3,0x02,0x03,0x01,0x00,0x01,0x30,0x0b,0x06,0x09,0x2a,0x86,0x48,0x86,
0xf7,0x0d,0x01,0x01,0x05,0x03,0x81,0x81,0x00,0x9f,0x11,0x8a,0x0a,0x6e,0xb0,
0x73,0xcc,0x48,0xf1,0x92,0xca,0xaf,0x9a,0x3d,0xb9,0xcf,0xbe,0x84,0xd0,0xa8,
0x34,0x25,0x27,0x9d,0x28,0x68,0xc5,0x35,0x2b,0x84,0xff,0xdb,0xd0,0x1f,0x0d,
0xd7,0xd6,0x8c,0x1b,0x33,0x52,0x7d,0x19,0xd0,0xc2,0xf3,0x63,0xd6,0x55,0x45,
0xf9,0x46,0xa0,0xb7,0xb3,0x94,0xbb,0x25,0x9b,0x29,0x76,0x7c,0x11,0xc7,0x7b,
0xcc,0xcb,0x99,0x3c,0xae,0xe7,0x16,0xb5,0xa7,0x6a,0x1f,0x75,0x4a,0x58,0x65,
0xb1,0x5b,0x91,0x29,0x20,0x81,0x51,0x64,0x05,0x24,0xa5,0x77,0xb7,0x8e,0xc8,
0x32,0x0f,0x0d,0x4f,0xf9,0x78,0x0f,0xc4,0xef,0xd6,0x25,0x5a,0xa4,0x9b,0x07,
0x17,0xea,0x56,0xe2,0x7b,0x61,0x1c,0x2d,0x40,0x38,0x9a,0x24,0x64,0x4b,0x6d,
0x08,0x96 };
static const BYTE rootSignedCRLWithBadAKI[] = {
0x30,0x82,0x01,0x1f,0x30,0x81,0x89,0x02,0x01,0x01,0x30,0x0d,0x06,0x09,0x2a,
0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,0x00,0x30,0x10,0x31,0x0e,0x30,
0x0c,0x06,0x03,0x55,0x04,0x03,0x13,0x05,0x43,0x65,0x72,0x74,0x31,0x17,0x0d,
0x30,0x37,0x30,0x39,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x17,0x0d,
0x30,0x37,0x31,0x32,0x33,0x31,0x32,0x33,0x35,0x39,0x35,0x39,0x5a,0x30,0x14,
0x30,0x12,0x02,0x01,0x01,0x17,0x0d,0x30,0x37,0x30,0x39,0x30,0x31,0x30,0x30,
0x30,0x30,0x30,0x30,0x5a,0xa0,0x2f,0x30,0x2d,0x30,0x0a,0x06,0x03,0x55,0x1d,
0x14,0x04,0x03,0x02,0x01,0x01,0x30,0x1f,0x06,0x03,0x55,0x1d,0x23,0x04,0x18,
0x30,0x18,0x80,0x14,0x14,0x8c,0x16,0xbb,0xbe,0x70,0xa2,0x28,0x89,0xa0,0x58,
0xff,0x98,0xbd,0xa8,0x24,0x2b,0x8a,0xe9,0x9a,0x30,0x0d,0x06,0x09,0x2a,0x86,
0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,0x00,0x03,0x81,0x81,0x00,0xa3,0xcf,
0x17,0x5d,0x7a,0x08,0xab,0x11,0x1a,0xbd,0x5c,0xde,0x9a,0x22,0x92,0x38,0xe6,
0x96,0xcc,0xb1,0xc5,0x42,0x86,0xa6,0xae,0xad,0xa3,0x1a,0x2b,0xa0,0xb0,0x65,
0xaa,0x9c,0xd7,0x2d,0x44,0x8c,0xae,0x61,0xc7,0x30,0x17,0x89,0x84,0x3b,0x4a,
0x8f,0x17,0x08,0x06,0x37,0x1c,0xf7,0x2d,0x4e,0x47,0x07,0x61,0x50,0xd9,0x06,
0xd1,0x46,0xed,0x0a,0xbb,0xc3,0x9b,0x36,0x0b,0xa7,0x27,0x2f,0x2b,0x55,0xce,
0x2a,0xa5,0x60,0xc6,0x53,0x28,0xe8,0xee,0xad,0x0e,0x2b,0xe8,0xd7,0x5f,0xc9,
0xa5,0xed,0xf9,0x77,0xb0,0x3c,0x81,0xcf,0xcc,0x49,0xb2,0x1a,0xc3,0xfd,0x34,
0xd5,0xbc,0xb0,0xd5,0xa5,0x9c,0x1b,0x72,0xc3,0x0f,0xa3,0xe3,0x3c,0xf0,0xc3,
0x91,0xe8,0x93,0x4f,0xd4,0x2f };
static const BYTE rootSignedCRL[] = {
0x30,0x81,0xe6,0x30,0x53,0x30,0x0b,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,
0x01,0x01,0x05,0x30,0x10,0x31,0x0e,0x30,0x0c,0x06,0x03,0x55,0x04,0x03,0x13,
0x05,0x43,0x65,0x72,0x74,0x31,0x17,0x0d,0x30,0x37,0x30,0x35,0x30,0x31,0x30,
0x30,0x30,0x30,0x30,0x30,0x5a,0x17,0x0d,0x30,0x37,0x31,0x32,0x33,0x31,0x32,
0x33,0x35,0x39,0x35,0x39,0x5a,0x30,0x14,0x30,0x12,0x02,0x01,0x01,0x17,0x0d,
0x30,0x37,0x31,0x30,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x30,0x0b,
0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x03,0x81,0x81,0x00,
0x94,0x84,0x0a,0xad,0x63,0xe3,0x05,0xc1,0xd8,0x94,0x44,0xeb,0x30,0x03,0xa1,
0xb4,0x7b,0x09,0x2f,0xf6,0xef,0x0f,0xe5,0x58,0x70,0x67,0xac,0x32,0x91,0xc0,
0x9d,0xf1,0x2b,0xf4,0xb3,0xcf,0xdd,0x1d,0x74,0x7b,0x6f,0x59,0x36,0x73,0xca,
0xcd,0x9c,0xb6,0xd9,0x35,0x39,0x45,0x8d,0xfd,0xf9,0x65,0xf3,0x42,0x2c,0x2c,
0xa6,0xfc,0xd2,0x23,0x6c,0x73,0x28,0x21,0x47,0x22,0x88,0x36,0x7d,0xd8,0xf0,
0xd0,0xca,0x11,0x20,0x50,0x6b,0x1e,0xb1,0x16,0x16,0xde,0xa6,0xc1,0x8d,0x18,
0xf1,0x42,0x22,0x1d,0x95,0x11,0xd7,0xa9,0x8f,0x90,0xe5,0x2f,0x71,0x52,0x47,
0xe0,0x45,0xb1,0x5a,0x2c,0x72,0x8a,0x25,0xca,0xd6,0x96,0xa2,0x7b,0x83,0x4c,
0xa3,0x24,0x7e,0xdd,0x45,0xa1,0x38,0xf8 };

static BOOL (WINAPI *pCertVerifyRevocation)(DWORD, DWORD, DWORD, void **, DWORD,
 PCERT_REVOCATION_PARA, PCERT_REVOCATION_STATUS);

/* Wednesday, Oct 1, 2007 */
static SYSTEMTIME oct2007 = { 2007, 10, 1, 1, 0, 0, 0, 0 };
/* Tuesday, May 1, 2007 */
static SYSTEMTIME may2007 = { 2007, 5, 2, 1, 0, 0, 0, 0 };

struct online_revocation_result
{
    BOOL ret;
    DWORD error;
    CERT_REVOCATION_STATUS status;
};

static BYTE *decode_base64_fixture(const char *fixture, DWORD *size)
{
    BYTE *data;

    *size = 0;
    if (!CryptStringToBinaryA(fixture, 0, CRYPT_STRING_BASE64, NULL, size,
            NULL, NULL))
    {
        ok(0, "failed to query fixture size, error %#lx\n", GetLastError());
        return NULL;
    }
    if (!(data = malloc(*size)))
    {
        ok(0, "failed to allocate %lu fixture bytes\n", *size);
        return NULL;
    }
    if (!CryptStringToBinaryA(fixture, 0, CRYPT_STRING_BASE64, data, size,
            NULL, NULL))
    {
        ok(0, "failed to decode fixture, error %#lx\n", GetLastError());
        free(data);
        return NULL;
    }
    return data;
}

static void delete_test_revocation_status(const CERT_CONTEXT *cert,
        const CERT_CONTEXT *issuer)
{
    static const WCHAR suffix[] =
            L"\\AppData\\LocalLow\\Microsoft\\CryptnetUrlCache\\Content\\";
    BYTE hash[20], *input;
    WCHAR path[MAX_PATH];
    DWORD hash_size = sizeof(hash), input_size, path_len, i;

    if (!winetest_platform_is_wine)
        return;
    input_size = cert->cbCertEncoded + sizeof(issuer->cbCertEncoded) +
            issuer->cbCertEncoded;
    if (!(input = malloc(input_size)))
        return;
    memcpy(input, cert->pbCertEncoded, cert->cbCertEncoded);
    memcpy(input + cert->cbCertEncoded, &issuer->cbCertEncoded,
            sizeof(issuer->cbCertEncoded));
    memcpy(input + cert->cbCertEncoded + sizeof(issuer->cbCertEncoded),
            issuer->pbCertEncoded, issuer->cbCertEncoded);
    if (!CryptHashCertificate(0, CALG_SHA1, 0, input, input_size, hash,
            &hash_size))
    {
        free(input);
        return;
    }
    free(input);

    path_len = GetEnvironmentVariableW(L"USERPROFILE", path, ARRAY_SIZE(path));
    if (!path_len || path_len >= ARRAY_SIZE(path) || path_len +
            ARRAY_SIZE(suffix) - 1 + ARRAY_SIZE(hash) * 2 >= ARRAY_SIZE(path))
        return;
    wcscpy(path + path_len, suffix);
    path_len = wcslen(path);
    for (i = 0; i < hash_size; ++i)
    {
        swprintf(path + path_len, 3, L"%02x", hash[i]);
        path_len += 2;
    }
    DeleteFileW(path);
}

static BOOL verify_with_online_crl(const CERT_CONTEXT *cert,
        const CERT_CONTEXT *issuer, const BYTE *crl_data, DWORD crl_size,
        FILETIME *time, struct online_revocation_result *result)
{
    CERT_ALT_NAME_ENTRY alternate_name = {CERT_ALT_NAME_URL};
    CRL_DIST_POINTS_INFO points = {0};
    CERT_EXTENSION *extensions = NULL;
    CRL_DIST_POINT point = {0};
    CRYPT_DATA_BLOB encoded = {0};
    CERT_REVOCATION_PARA params = {sizeof(params)};
    CERT_CONTEXT context;
    CERT_INFO info;
    struct http_server server;
    char headers[160], url[128];
    BYTE *encoded_cert = NULL;
    WCHAR urlW[128];
    void *contexts[1];
    ULONGLONG nonce;
    static LONG sequence;
    LONG id;
    USHORT port;
    BOOL success = FALSE;

    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: application/pkix-crl\r\n"
            "Content-Length: %lu\r\nConnection: close\r\n\r\n", crl_size);
    if (!start_http_server(&server, headers, crl_data, crl_size, 0, &port))
    {
        ok(0, "failed to start local CRL server\n");
        return FALSE;
    }

    id = InterlockedIncrement(&sequence);
    sprintf(url, "http://127.0.0.1:%u/crl-%ld", port, id);
    MultiByteToWideChar(CP_ACP, 0, url, -1, urlW, ARRAY_SIZE(urlW));
    alternate_name.pwszURL = urlW;
    point.DistPointName.dwDistPointNameChoice = CRL_DIST_POINT_FULL_NAME;
    point.DistPointName.FullName.cAltEntry = 1;
    point.DistPointName.FullName.rgAltEntry = &alternate_name;
    points.cDistPoint = 1;
    points.rgDistPoint = &point;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_CRL_DIST_POINTS, &points,
            CRYPT_ENCODE_ALLOC_FLAG, NULL, &encoded.pbData, &encoded.cbData))
    {
        ok(0, "failed to encode CRL distribution point, error %#lx\n",
                GetLastError());
        goto done;
    }

    extensions = malloc((cert->pCertInfo->cExtension + 1) * sizeof(*extensions));
    if (!extensions)
    {
        ok(0, "failed to allocate certificate extensions\n");
        goto done;
    }
    extensions[0].pszObjId = (char *)szOID_CRL_DIST_POINTS;
    extensions[0].fCritical = FALSE;
    extensions[0].Value = encoded;
    if (cert->pCertInfo->cExtension)
        memcpy(extensions + 1, cert->pCertInfo->rgExtension,
                cert->pCertInfo->cExtension * sizeof(*extensions));

    info = *cert->pCertInfo;
    info.cExtension++;
    info.rgExtension = extensions;
    context = *cert;
    context.pCertInfo = &info;
    if (!(encoded_cert = malloc(cert->cbCertEncoded + sizeof(nonce))))
    {
        ok(0, "failed to allocate unique certificate encoding\n");
        goto done;
    }
    memcpy(encoded_cert, cert->pbCertEncoded, cert->cbCertEncoded);
    nonce = GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 32) ^ id;
    memcpy(encoded_cert + cert->cbCertEncoded, &nonce, sizeof(nonce));
    context.pbCertEncoded = encoded_cert;
    context.cbCertEncoded += sizeof(nonce);
    contexts[0] = &context;
    params.pIssuerCert = issuer;
    params.pftTimeToUse = time;
    params.dwUrlRetrievalTimeout = 3000;
    memset(&result->status, 0, sizeof(result->status));
    result->status.cbSize = sizeof(result->status);
    SetLastError(0xdeadbeef);
    result->ret = pCertVerifyRevocation(X509_ASN_ENCODING,
            CERT_CONTEXT_REVOCATION_TYPE, 1, contexts,
            CERT_VERIFY_REV_ACCUMULATIVE_TIMEOUT_FLAG, &params,
            &result->status);
    result->error = GetLastError();
    delete_test_revocation_status(&context, issuer);
    success = TRUE;

done:
    DeleteUrlCacheEntryA(url);
    free(encoded_cert);
    free(extensions);
    LocalFree(encoded.pbData);
    stop_http_server(&server);
    return success;
}

static BOOL verify_with_ocsp_responses(const CERT_CONTEXT *cert,
        const CERT_CONTEXT *issuer, const char *const *headers,
        const BYTE *const *responses, const DWORD *response_sizes,
        DWORD response_count, FILETIME *time,
        struct online_revocation_result *result)
{
    CERT_AUTHORITY_INFO_ACCESS access_info = {0};
    CERT_ACCESS_DESCRIPTION accesses[2];
    CERT_EXTENSION *extensions = NULL;
    CRYPT_DATA_BLOB encoded = {0};
    CERT_REVOCATION_PARA params = {sizeof(params)};
    CERT_CONTEXT context;
    CERT_INFO info;
    struct http_server servers[2] = {{0}};
    BYTE *encoded_cert = NULL;
    char urls[2][128] = {{0}};
    WCHAR urlsW[2][128];
    void *contexts[1];
    ULONGLONG nonce;
    static LONG sequence;
    LONG id;
    USHORT ports[2];
    DWORD i;
    BOOL success = FALSE;

    if (!response_count || response_count > ARRAY_SIZE(servers))
    {
        ok(0, "invalid OCSP response count %lu\n", response_count);
        return FALSE;
    }
    memset(accesses, 0, sizeof(accesses));
    id = InterlockedIncrement(&sequence);
    for (i = 0; i < response_count; ++i)
    {
        if (!start_http_server(&servers[i], headers[i], responses[i],
                response_sizes[i], 0, &ports[i]))
        {
            ok(0, "failed to start local OCSP server %lu\n", i);
            goto done;
        }
        sprintf(urls[i], "http://127.0.0.1:%u/ocsp-%ld-%lu", ports[i],
                id, i);
        MultiByteToWideChar(CP_ACP, 0, urls[i], -1, urlsW[i],
                ARRAY_SIZE(urlsW[i]));
        accesses[i].pszAccessMethod = (char *)szOID_PKIX_OCSP;
        accesses[i].AccessLocation.dwAltNameChoice = CERT_ALT_NAME_URL;
        accesses[i].AccessLocation.pwszURL = urlsW[i];
    }
    access_info.cAccDescr = response_count;
    access_info.rgAccDescr = accesses;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_AUTHORITY_INFO_ACCESS,
            &access_info, CRYPT_ENCODE_ALLOC_FLAG, NULL, &encoded.pbData,
            &encoded.cbData))
    {
        ok(0, "failed to encode authority information access, error %#lx\n",
                GetLastError());
        goto done;
    }

    extensions = malloc((cert->pCertInfo->cExtension + 1) * sizeof(*extensions));
    if (!extensions)
    {
        ok(0, "failed to allocate certificate extensions\n");
        goto done;
    }
    extensions[0].pszObjId = (char *)szOID_AUTHORITY_INFO_ACCESS;
    extensions[0].fCritical = FALSE;
    extensions[0].Value = encoded;
    if (cert->pCertInfo->cExtension)
        memcpy(extensions + 1, cert->pCertInfo->rgExtension,
                cert->pCertInfo->cExtension * sizeof(*extensions));
    info = *cert->pCertInfo;
    info.cExtension++;
    info.rgExtension = extensions;
    context = *cert;
    context.pCertInfo = &info;
    if (!(encoded_cert = malloc(cert->cbCertEncoded + sizeof(nonce))))
    {
        ok(0, "failed to allocate unique certificate encoding\n");
        goto done;
    }
    memcpy(encoded_cert, cert->pbCertEncoded, cert->cbCertEncoded);
    nonce = GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 32) ^ id;
    memcpy(encoded_cert + cert->cbCertEncoded, &nonce, sizeof(nonce));
    context.pbCertEncoded = encoded_cert;
    context.cbCertEncoded += sizeof(nonce);
    contexts[0] = &context;
    params.pIssuerCert = issuer;
    params.pftTimeToUse = time;
    params.dwUrlRetrievalTimeout = 3000;
    memset(&result->status, 0, sizeof(result->status));
    result->status.cbSize = sizeof(result->status);
    SetLastError(0xdeadbeef);
    result->ret = pCertVerifyRevocation(X509_ASN_ENCODING,
            CERT_CONTEXT_REVOCATION_TYPE, 1, contexts,
            CERT_VERIFY_REV_ACCUMULATIVE_TIMEOUT_FLAG, &params,
            &result->status);
    result->error = GetLastError();
    delete_test_revocation_status(&context, issuer);
    success = TRUE;

done:
    for (i = 0; i < response_count; ++i)
    {
        if (urls[i][0]) DeleteUrlCacheEntryA(urls[i]);
        stop_http_server(&servers[i]);
    }
    free(encoded_cert);
    free(extensions);
    LocalFree(encoded.pbData);
    return success;
}

static BOOL verify_with_ocsp_response(const CERT_CONTEXT *cert,
        const CERT_CONTEXT *issuer, const char *headers,
        const BYTE *response, DWORD response_size, FILETIME *time,
        struct online_revocation_result *result)
{
    const BYTE *responses[] = {response};
    const char *response_headers[] = {headers};
    DWORD response_sizes[] = {response_size};

    return verify_with_ocsp_responses(cert, issuer, response_headers,
            responses, response_sizes, 1, time, result);
}

static void test_ocsp_response_limit(void)
{
    static const char oversized_headers[] =
            "HTTP/1.1 200 OK\r\nContent-Type: application/ocsp-response\r\n"
            "Content-Length: 1048577\r\nConnection: close\r\n\r\n";
    struct online_revocation_result result;
    const CERT_CONTEXT *issuer, *cert;
    WSADATA wsa_data;
    BOOL ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
    {
        win_skip("Winsock is unavailable\n");
        return;
    }
    pCertVerifyRevocation = (void *)GetProcAddress(GetModuleHandleA("cryptnet.dll"),
            "CertDllVerifyRevocation");
    issuer = CertCreateCertificateContext(X509_ASN_ENCODING,
            rootWithKeySignAndCRLSign, sizeof(rootWithKeySignAndCRLSign));
    cert = CertCreateCertificateContext(X509_ASN_ENCODING, revokedCert,
            sizeof(revokedCert));
    ok(pCertVerifyRevocation && issuer && cert, "failed to prepare OCSP test\n");
    if (pCertVerifyRevocation && issuer && cert)
    {
        ret = verify_with_ocsp_response(cert, issuer, oversized_headers,
                NULL, 0, NULL, &result);
        if (ret)
        {
            ok(!result.ret, "accepted an oversized OCSP response\n");
            ok(result.error == ERROR_FILE_TOO_LARGE, "got error %#lx\n",
                    result.error);
            ok(result.status.dwError == ERROR_FILE_TOO_LARGE,
                    "got status %#lx\n", result.status.dwError);
        }
    }
    if (cert) CertFreeCertificateContext(cert);
    if (issuer) CertFreeCertificateContext(issuer);
    WSACleanup();
}

static BOOL verify_ocsp_fixture(const CERT_CONTEXT *cert,
        const CERT_CONTEXT *issuer, const char *fixture, FILETIME *time,
        struct online_revocation_result *result)
{
    BYTE *response;
    char headers[160];
    DWORD response_size;
    BOOL ret;

    if (!(response = decode_base64_fixture(fixture, &response_size)))
        return FALSE;
    sprintf(headers, "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/ocsp-response\r\n"
            "Content-Length: %lu\r\nConnection: close\r\n\r\n",
            response_size);
    ret = verify_with_ocsp_response(cert, issuer, headers, response,
            response_size, time, result);
    free(response);
    return ret;
}

static void test_ocsp_response_validation(void)
{
    static SYSTEMTIME august3_2026 = {2026, 8, 0, 3, 0, 0, 0, 0};
    static SYSTEMTIME august11_2026 = {2026, 8, 0, 11, 0, 0, 0, 0};
    struct online_revocation_result result;
    const CERT_CONTEXT *issuer = NULL, *leaf = NULL, *revoked = NULL;
    BYTE *issuer_data = NULL, *leaf_data = NULL, *revoked_data = NULL;
    BYTE *tampered = NULL;
    DWORD issuer_size, leaf_size, revoked_size, tampered_size;
    FILETIME before_revocation, after_revocation;
    char headers[160];
    WSADATA wsa_data;
    BOOL ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
    {
        win_skip("Winsock is unavailable\n");
        return;
    }
    pCertVerifyRevocation = (void *)GetProcAddress(GetModuleHandleA("cryptnet.dll"),
            "CertDllVerifyRevocation");
    issuer_data = decode_base64_fixture(test_ca_base64, &issuer_size);
    leaf_data = decode_base64_fixture(test_leaf_base64, &leaf_size);
    revoked_data = decode_base64_fixture(test_future_revoked_base64,
            &revoked_size);
    if (issuer_data)
        issuer = CertCreateCertificateContext(X509_ASN_ENCODING, issuer_data,
                issuer_size);
    if (leaf_data)
        leaf = CertCreateCertificateContext(X509_ASN_ENCODING, leaf_data,
                leaf_size);
    if (revoked_data)
        revoked = CertCreateCertificateContext(X509_ASN_ENCODING,
                revoked_data, revoked_size);
    ok(pCertVerifyRevocation && issuer && leaf && revoked,
            "failed to prepare OCSP response tests\n");
    if (!pCertVerifyRevocation || !issuer || !leaf || !revoked)
        goto done;
    ok(SystemTimeToFileTime(&august3_2026, &before_revocation),
            "failed to create historical verification time\n");
    ok(SystemTimeToFileTime(&august11_2026, &after_revocation),
            "failed to create post-revocation verification time\n");

    ret = verify_ocsp_fixture(leaf, issuer, test_direct_good_ocsp_base64,
            &before_revocation, &result);
    if (ret)
    {
        ok(result.ret, "issuer-signed OCSP response failed, error %#lx\n",
                result.error);
        ok(!result.status.dwError, "got status %#lx\n",
                result.status.dwError);
    }

    ret = verify_ocsp_fixture(leaf, issuer, test_delegated_good_ocsp_base64,
            &before_revocation, &result);
    if (ret)
    {
        ok(result.ret, "delegated OCSP response failed, error %#lx\n",
                result.error);
        ok(!result.status.dwError, "got status %#lx\n",
                result.status.dwError);
    }

    ret = verify_ocsp_fixture(leaf, issuer,
            test_delegated_no_next_ocsp_base64, &before_revocation, &result);
    if (ret)
    {
        ok(result.ret, "OCSP response without nextUpdate failed, error %#lx\n",
                result.error);
        ok(!result.status.dwError, "got status %#lx\n",
                result.status.dwError);
    }

    ret = verify_ocsp_fixture(leaf, issuer,
            test_delegated_bad_eku_ocsp_base64, &before_revocation, &result);
    if (ret)
    {
        ok(!result.ret, "accepted an unauthorized delegated responder\n");
        ok(result.error == CRYPT_E_NO_REVOCATION_CHECK,
                "got error %#lx\n", result.error);
        ok(result.status.dwError == CRYPT_E_NO_REVOCATION_CHECK,
                "got status %#lx\n", result.status.dwError);
    }

    {
        BYTE *bad_response, *good_response;
        DWORD response_sizes[2];
        char first_headers[160], second_headers[160];
        const char *response_headers[2] = {first_headers, second_headers};
        const BYTE *responses[2];

        bad_response = decode_base64_fixture(
                test_delegated_bad_eku_ocsp_base64, response_sizes);
        good_response = decode_base64_fixture(test_direct_good_ocsp_base64,
                response_sizes + 1);
        if (bad_response && good_response)
        {
            sprintf(first_headers, "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/ocsp-response\r\n"
                    "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                    response_sizes[0]);
            sprintf(second_headers, "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/ocsp-response\r\n"
                    "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                    response_sizes[1]);
            responses[0] = bad_response;
            responses[1] = good_response;
            ret = verify_with_ocsp_responses(leaf, issuer,
                    response_headers, responses, response_sizes, 2,
                    &before_revocation, &result);
            if (ret)
            {
                ok(result.ret, "OCSP responder failover failed, error %#lx\n",
                        result.error);
                ok(!result.status.dwError, "got status %#lx\n",
                        result.status.dwError);
            }
        }
        free(good_response);
        free(bad_response);
    }

    ret = verify_ocsp_fixture(revoked, issuer,
            test_delegated_future_revoked_ocsp_base64, &before_revocation,
            &result);
    if (ret)
    {
        ok(result.ret, "certificate failed before its revocation date, error %#lx\n",
                result.error);
        ok(!result.status.dwError, "got status %#lx\n",
                result.status.dwError);
        ok(!result.status.dwReason, "got reason %lu\n",
                result.status.dwReason);
    }

    ret = verify_ocsp_fixture(revoked, issuer,
            test_delegated_future_revoked_ocsp_base64, &after_revocation,
            &result);
    if (ret)
    {
        ok(!result.ret, "revoked certificate unexpectedly succeeded\n");
        ok(result.error == CRYPT_E_REVOKED, "got error %#lx\n",
                result.error);
        ok(result.status.dwError == CRYPT_E_REVOKED, "got status %#lx\n",
                result.status.dwError);
        ok(result.status.dwReason == CRL_REASON_KEY_COMPROMISE,
                "got reason %lu\n", result.status.dwReason);
    }

    tampered = decode_base64_fixture(test_direct_good_ocsp_base64,
            &tampered_size);
    if (tampered)
    {
        tampered[tampered_size - 1] ^= 0x80;
        sprintf(headers, "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/ocsp-response\r\n"
                "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                tampered_size);
        ret = verify_with_ocsp_response(leaf, issuer, headers, tampered,
                tampered_size, &before_revocation, &result);
        if (ret)
        {
            ok(!result.ret, "accepted an OCSP response with a modified signature\n");
            ok(result.error == NTE_BAD_SIGNATURE, "got error %#lx\n",
                    result.error);
            ok(result.status.dwError == NTE_BAD_SIGNATURE,
                    "got status %#lx\n", result.status.dwError);
        }
    }

done:
    free(tampered);
    if (revoked) CertFreeCertificateContext(revoked);
    if (leaf) CertFreeCertificateContext(leaf);
    if (issuer) CertFreeCertificateContext(issuer);
    free(revoked_data);
    free(leaf_data);
    free(issuer_data);
    WSACleanup();
}

static void test_online_crl_verification(void)
{
    struct online_revocation_result result;
    const CERT_CONTEXT *issuer, *revoked, *unrevoked;
    BYTE tampered_crl[sizeof(rootSignedCRL)];
    FILETIME time;
    WSADATA wsa_data;
    BOOL ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
    {
        win_skip("Winsock is unavailable\n");
        return;
    }
    pCertVerifyRevocation = (void *)GetProcAddress(GetModuleHandleA("cryptnet.dll"),
            "CertDllVerifyRevocation");
    ok(!!pCertVerifyRevocation, "CertDllVerifyRevocation is unavailable\n");
    if (!pCertVerifyRevocation)
        goto done;

    issuer = CertCreateCertificateContext(X509_ASN_ENCODING,
            rootWithKeySignAndCRLSign, sizeof(rootWithKeySignAndCRLSign));
    revoked = CertCreateCertificateContext(X509_ASN_ENCODING, revokedCert,
            sizeof(revokedCert));
    unrevoked = CertCreateCertificateContext(X509_ASN_ENCODING, unRevokedCert,
            sizeof(unRevokedCert));
    ok(issuer && revoked && unrevoked, "failed to create test certificates\n");
    if (!issuer || !revoked || !unrevoked)
        goto free_certs;
    SystemTimeToFileTime(&oct2007, &time);

    memcpy(tampered_crl, rootSignedCRL, sizeof(tampered_crl));
    tampered_crl[sizeof(tampered_crl) - 1] ^= 0x80;
    ret = verify_with_online_crl(revoked, issuer, tampered_crl,
            sizeof(tampered_crl), &time, &result);
    if (ret)
    {
        ok(!result.ret, "accepted a CRL with a modified signature\n");
        ok(result.error == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n",
                result.error);
        ok(result.status.dwError == CRYPT_E_NO_REVOCATION_CHECK,
                "got status %#lx\n", result.status.dwError);
    }

    ret = verify_with_online_crl(revoked, issuer, rootSignedCRL,
            sizeof(rootSignedCRL), NULL, &result);
    if (ret)
    {
        ok(!result.ret, "accepted an expired CRL\n");
        ok(result.error == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n",
                result.error);
        ok(result.status.dwError == CRYPT_E_REVOCATION_OFFLINE,
                "got status %#lx\n", result.status.dwError);
    }

    SystemTimeToFileTime(&may2007, &time);
    ret = verify_with_online_crl(revoked, issuer, rootSignedCRL,
            sizeof(rootSignedCRL), &time, &result);
    if (ret)
    {
        ok(result.ret, "certificate failed before its CRL revocation date, error %#lx\n",
                result.error);
        ok(!result.status.dwError, "got status %#lx\n",
                result.status.dwError);
        ok(!result.status.dwReason, "got reason %lu\n",
                result.status.dwReason);
    }

    SystemTimeToFileTime(&oct2007, &time);
    ret = verify_with_online_crl(revoked, issuer, rootSignedCRL,
            sizeof(rootSignedCRL), &time, &result);
    if (ret)
    {
        ok(!result.ret, "revoked certificate unexpectedly succeeded\n");
        ok(result.error == CRYPT_E_REVOKED, "got error %#lx\n", result.error);
        ok(result.status.dwError == CRYPT_E_REVOKED, "got status %#lx\n",
                result.status.dwError);
    }

    ret = verify_with_online_crl(unrevoked, issuer, rootSignedCRL,
            sizeof(rootSignedCRL), &time, &result);
    if (ret)
    {
        ok(result.ret, "unrevoked certificate failed, error %#lx\n", result.error);
        ok(!result.status.dwError, "got status %#lx\n", result.status.dwError);
    }

free_certs:
    if (unrevoked) CertFreeCertificateContext(unrevoked);
    if (revoked) CertFreeCertificateContext(revoked);
    if (issuer) CertFreeCertificateContext(issuer);
done:
    WSACleanup();
}

static BOOL verify_crl_fixture(const CERT_CONTEXT *cert,
        const CERT_CONTEXT *issuer, const char *fixture, FILETIME *time,
        struct online_revocation_result *result)
{
    BYTE *crl;
    DWORD crl_size;
    BOOL ret;

    if (!(crl = decode_base64_fixture(fixture, &crl_size)))
        return FALSE;
    ret = verify_with_online_crl(cert, issuer, crl, crl_size, time, result);
    free(crl);
    return ret;
}

static void test_crl_distribution_point_failover(const CERT_CONTEXT *cert,
        const CERT_CONTEXT *issuer, FILETIME *time)
{
    CERT_ALT_NAME_ENTRY alternate_names[2];
    CRL_DIST_POINTS_INFO points_info = {0};
    CERT_EXTENSION *extensions = NULL;
    CRL_DIST_POINT points[2];
    CRYPT_DATA_BLOB encoded = {0};
    CERT_REVOCATION_PARA params = {sizeof(params)};
    struct online_revocation_result result;
    struct http_server servers[2] = {{0}};
    BYTE *crls[2] = {NULL, NULL}, *encoded_cert = NULL;
    const char *fixtures[2] = {test_delta_crl_base64,
            test_complete_crl_base64};
    DWORD crl_sizes[2], i;
    char headers[2][160], urls[2][128] = {{0}};
    WCHAR urlsW[2][128];
    CERT_CONTEXT context;
    CERT_INFO info;
    void *contexts[1];
    ULONGLONG nonce;
    static LONG sequence;
    LONG id;
    USHORT ports[2];
    BOOL ret;

    memset(alternate_names, 0, sizeof(alternate_names));
    memset(points, 0, sizeof(points));
    for (i = 0; i < ARRAY_SIZE(crls); ++i)
    {
        if (!(crls[i] = decode_base64_fixture(fixtures[i], &crl_sizes[i])))
            goto done;
        sprintf(headers[i], "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/pkix-crl\r\n"
                "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                crl_sizes[i]);
        if (!start_http_server(&servers[i], headers[i], crls[i],
                crl_sizes[i], 0, &ports[i]))
        {
            ok(0, "failed to start CRL server %lu\n", i);
            goto done;
        }
    }

    id = InterlockedIncrement(&sequence);
    for (i = 0; i < ARRAY_SIZE(crls); ++i)
    {
        sprintf(urls[i], "http://127.0.0.1:%u/failover-%ld-%lu",
                ports[i], id, i);
        MultiByteToWideChar(CP_ACP, 0, urls[i], -1, urlsW[i],
                ARRAY_SIZE(urlsW[i]));
        alternate_names[i].dwAltNameChoice = CERT_ALT_NAME_URL;
        alternate_names[i].pwszURL = urlsW[i];
        points[i].DistPointName.dwDistPointNameChoice =
                CRL_DIST_POINT_FULL_NAME;
        points[i].DistPointName.FullName.cAltEntry = 1;
        points[i].DistPointName.FullName.rgAltEntry =
                alternate_names + i;
    }
    points_info.cDistPoint = ARRAY_SIZE(points);
    points_info.rgDistPoint = points;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_CRL_DIST_POINTS,
            &points_info, CRYPT_ENCODE_ALLOC_FLAG, NULL, &encoded.pbData,
            &encoded.cbData))
    {
        ok(0, "failed to encode CRL distribution points, error %#lx\n",
                GetLastError());
        goto done;
    }

    extensions = malloc((cert->pCertInfo->cExtension + 1) *
            sizeof(*extensions));
    if (!extensions)
    {
        ok(0, "failed to allocate certificate extensions\n");
        goto done;
    }
    extensions[0].pszObjId = (char *)szOID_CRL_DIST_POINTS;
    extensions[0].fCritical = FALSE;
    extensions[0].Value = encoded;
    if (cert->pCertInfo->cExtension)
        memcpy(extensions + 1, cert->pCertInfo->rgExtension,
                cert->pCertInfo->cExtension * sizeof(*extensions));
    info = *cert->pCertInfo;
    info.cExtension++;
    info.rgExtension = extensions;
    context = *cert;
    context.pCertInfo = &info;
    if (!(encoded_cert = malloc(cert->cbCertEncoded + sizeof(nonce))))
    {
        ok(0, "failed to allocate unique certificate encoding\n");
        goto done;
    }
    memcpy(encoded_cert, cert->pbCertEncoded, cert->cbCertEncoded);
    nonce = GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 32) ^ id;
    memcpy(encoded_cert + cert->cbCertEncoded, &nonce, sizeof(nonce));
    context.pbCertEncoded = encoded_cert;
    context.cbCertEncoded += sizeof(nonce);
    contexts[0] = &context;
    params.pIssuerCert = issuer;
    params.pftTimeToUse = time;
    params.dwUrlRetrievalTimeout = 3000;
    memset(&result.status, 0, sizeof(result.status));
    result.status.cbSize = sizeof(result.status);
    SetLastError(0xdeadbeef);
    result.ret = pCertVerifyRevocation(X509_ASN_ENCODING,
            CERT_CONTEXT_REVOCATION_TYPE, 1, contexts,
            CERT_VERIFY_REV_ACCUMULATIVE_TIMEOUT_FLAG, &params,
            &result.status);
    result.error = GetLastError();
    ok(result.ret, "CRL distribution point failover failed, error %#lx\n",
            result.error);
    ok(!result.status.dwError, "got status %#lx\n",
            result.status.dwError);
    delete_test_revocation_status(&context, issuer);

    for (i = 0; i < ARRAY_SIZE(servers); ++i)
        stop_http_server(&servers[i]);
    ok(!url_cache_entry_exists(urls[0]),
            "unvalidated CRL was written to the URL cache\n");
    ok(url_cache_entry_exists(urls[1]),
            "validated CRL was not written to the URL cache\n");

    memset(&result.status, 0, sizeof(result.status));
    result.status.cbSize = sizeof(result.status);
    SetLastError(0xdeadbeef);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING,
            CERT_CONTEXT_REVOCATION_TYPE, 1, contexts,
            CERT_VERIFY_CACHE_ONLY_BASED_REVOCATION |
            CERT_VERIFY_REV_ACCUMULATIVE_TIMEOUT_FLAG, &params,
            &result.status);
    result.error = GetLastError();
    ok(ret, "cached validated CRL failed, error %#lx\n", result.error);
    ok(!result.status.dwError, "got cached status %#lx\n",
            result.status.dwError);
    delete_test_revocation_status(&context, issuer);

done:
    for (i = 0; i < ARRAY_SIZE(servers); ++i)
    {
        if (urls[i][0]) DeleteUrlCacheEntryA(urls[i]);
        stop_http_server(&servers[i]);
        free(crls[i]);
    }
    free(encoded_cert);
    free(extensions);
    LocalFree(encoded.pbData);
}

static void test_crl_scope_validation(void)
{
    static SYSTEMTIME august3_2026 = {2026, 8, 0, 3, 0, 0, 0, 0};
    struct online_revocation_result result;
    const CERT_CONTEXT *issuer = NULL, *leaf = NULL;
    BYTE *issuer_data = NULL, *leaf_data = NULL;
    DWORD issuer_size, leaf_size;
    FILETIME time;
    WSADATA wsa_data;
    BOOL ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
    {
        win_skip("Winsock is unavailable\n");
        return;
    }
    pCertVerifyRevocation = (void *)GetProcAddress(GetModuleHandleA("cryptnet.dll"),
            "CertDllVerifyRevocation");
    issuer_data = decode_base64_fixture(test_ca_base64, &issuer_size);
    leaf_data = decode_base64_fixture(test_leaf_base64, &leaf_size);
    if (issuer_data)
        issuer = CertCreateCertificateContext(X509_ASN_ENCODING, issuer_data,
                issuer_size);
    if (leaf_data)
        leaf = CertCreateCertificateContext(X509_ASN_ENCODING, leaf_data,
                leaf_size);
    ok(pCertVerifyRevocation && issuer && leaf,
            "failed to prepare CRL scope tests\n");
    if (!pCertVerifyRevocation || !issuer || !leaf)
        goto done;
    ok(SystemTimeToFileTime(&august3_2026, &time),
            "failed to create CRL verification time\n");

    ret = verify_crl_fixture(leaf, issuer, test_complete_crl_base64, &time,
            &result);
    if (ret)
    {
        ok(result.ret, "complete CRL failed, error %#lx\n", result.error);
        ok(!result.status.dwError, "got status %#lx\n",
                result.status.dwError);
    }

    ret = verify_crl_fixture(leaf, issuer, test_delta_crl_base64, &time,
            &result);
    if (ret)
    {
        ok(!result.ret, "accepted a delta CRL without its base CRL\n");
        ok(result.error == CRYPT_E_NO_REVOCATION_CHECK,
                "got error %#lx\n", result.error);
        ok(result.status.dwError == CRYPT_E_NO_REVOCATION_CHECK,
                "got status %#lx\n", result.status.dwError);
    }

    ret = verify_crl_fixture(leaf, issuer, test_reasons_crl_base64, &time,
            &result);
    if (ret)
    {
        ok(!result.ret, "accepted a reason-partitioned CRL as complete\n");
        ok(result.error == CRYPT_E_NO_REVOCATION_CHECK,
                "got error %#lx\n", result.error);
        ok(result.status.dwError == CRYPT_E_NO_REVOCATION_CHECK,
                "got status %#lx\n", result.status.dwError);
    }

    ret = verify_crl_fixture(leaf, issuer,
            test_unknown_critical_crl_base64, &time, &result);
    if (ret)
    {
        ok(!result.ret, "accepted a CRL with an unknown critical extension\n");
        ok(result.error == CRYPT_E_NO_REVOCATION_CHECK,
                "got error %#lx\n", result.error);
        ok(result.status.dwError == CRYPT_E_NO_REVOCATION_CHECK,
                "got status %#lx\n", result.status.dwError);
    }

    test_crl_distribution_point_failover(leaf, issuer, &time);

done:
    if (leaf) CertFreeCertificateContext(leaf);
    if (issuer) CertFreeCertificateContext(issuer);
    free(leaf_data);
    free(issuer_data);
    WSACleanup();
}

static void test_verifyRevocation(void)
{
    CERT_REVOCATION_STATUS status;
    CERT_REVOCATION_PARA params = {sizeof(params)};
    const CERT_CONTEXT *certs[2];
    FILETIME time;
    BOOL ret;

    pCertVerifyRevocation = (void *)GetProcAddress(GetModuleHandleA("cryptnet.dll"), "CertDllVerifyRevocation");

    if (0)
    {
        /* Crash */
        pCertVerifyRevocation(0, 0, 0, NULL, 0, NULL, NULL);
    }

    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(0, 0, 0, NULL, 0, NULL, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == E_INVALIDARG, "got error %#lx\n", GetLastError());
    todo_wine ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    todo_wine ok(status.dwError == E_INVALIDARG, "got error %#lx\n", status.dwError);
    todo_wine ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, 0, 0, NULL, 0, NULL, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == E_INVALIDARG, "got error %#lx\n", GetLastError());
    todo_wine ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    todo_wine ok(status.dwError == E_INVALIDARG, "got error %#lx\n", status.dwError);
    todo_wine ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(0, CERT_CONTEXT_REVOCATION_TYPE, 0, NULL, 0, NULL, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == E_INVALIDARG, "got error %#lx\n", GetLastError());
    todo_wine ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    todo_wine ok(status.dwError == E_INVALIDARG, "got error %#lx\n", status.dwError);
    todo_wine ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    certs[0] = CertCreateCertificateContext(X509_ASN_ENCODING, bigCert, sizeof(bigCert));

    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(0, CERT_CONTEXT_REVOCATION_TYPE, 1, (void **)certs, 0, NULL, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    CertFreeCertificateContext(certs[0]);

    certs[0] = CertCreateCertificateContext(X509_ASN_ENCODING, rootWithKeySignAndCRLSign, sizeof(rootWithKeySignAndCRLSign));
    certs[1] = CertCreateCertificateContext(X509_ASN_ENCODING, revokedCert, sizeof(revokedCert));

    /* The root cert itself can't be checked for revocation */
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(0, CERT_CONTEXT_REVOCATION_TYPE, 1, (void **)&certs[0], 0, NULL, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    /* Neither can the end cert */
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(0, CERT_CONTEXT_REVOCATION_TYPE, 1, (void **)&certs[1], 0, NULL, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    /* Both certs together can't, either (they're not CRLs) */
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(0, CERT_CONTEXT_REVOCATION_TYPE, 2, (void **)certs, 0, NULL, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    /* Test with an invalid CRL */

    params.hCrlStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    ret = CertAddEncodedCRLToStore(params.hCrlStore, X509_ASN_ENCODING, rootSignedCRLWithBadAKI,
            sizeof(rootSignedCRLWithBadAKI), CERT_STORE_ADD_ALWAYS, NULL);
    ok(ret, "failed to add CRL, error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            2, (void **)certs, 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    /* Specifying CERT_VERIFY_REV_CHAIN_FLAG doesn't change things either */
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            2, (void **)certs, CERT_VERIFY_REV_CHAIN_FLAG, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    /* Again, specifying the issuer cert: no change */
    params.pIssuerCert = certs[0];
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    /* Specifying the time to check: still no change */
    SystemTimeToFileTime(&oct2007, &time);
    params.pftTimeToUse = &time;

    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    CertCloseStore(params.hCrlStore, 0);

    /* Test again with a valid CRL.  This time, the cert should be revoked when
     * the time is after the validity period of the CRL, or considered
     * "revocation offline" when the checked time precedes the validity
     * period of the CRL.
     */
    params.hCrlStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    ret = CertAddEncodedCRLToStore(params.hCrlStore, X509_ASN_ENCODING,
            rootSignedCRL, sizeof(rootSignedCRL), CERT_STORE_ADD_ALWAYS, NULL);
    ok(ret, "failed to add CRL, error %lu\n", GetLastError());

    params.pftTimeToUse = NULL;

    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_REVOKED, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_REVOKED, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    SystemTimeToFileTime(&oct2007, &time);
    params.pftTimeToUse = &time;
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_REVOKED, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_REVOKED, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    SystemTimeToFileTime(&may2007, &time);
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    CertFreeCertificateContext(certs[1]);

    /* Test again with a valid CRL and an un-revoked cert.  No matter the
     * time checked, it's reported as revocation offline.
     */
    certs[1] = CertCreateCertificateContext(X509_ASN_ENCODING, unRevokedCert, sizeof(unRevokedCert));

    params.pftTimeToUse = NULL;
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    SystemTimeToFileTime(&oct2007, &time);
    params.pftTimeToUse = &time;
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    SystemTimeToFileTime(&may2007, &time);
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_REVOCATION_OFFLINE, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    params.pftTimeToUse = NULL;

    /* Test with the wrong encoding type. */
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(0, CERT_CONTEXT_REVOCATION_TYPE,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    todo_wine ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    todo_wine ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    /* Test with the wrong context type. */
    SetLastError(0xdeadbeef);
    memset(&status, 0xcc, sizeof(status));
    status.cbSize = sizeof(status);
    ret = pCertVerifyRevocation(X509_ASN_ENCODING, 0xdeadbeef,
            1, (void **)&certs[1], 0, &params, &status);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", GetLastError());
    ok(!status.dwIndex, "got index %lu\n", status.dwIndex);
    ok(status.dwError == CRYPT_E_NO_REVOCATION_CHECK, "got error %#lx\n", status.dwError);
    ok(!status.dwReason, "got reason %lu\n", status.dwReason);

    CertCloseStore(params.hCrlStore, 0);
    CertFreeCertificateContext(certs[1]);
    CertFreeCertificateContext(certs[0]);
}

START_TEST(cryptnet)
{
    test_getObjectUrl();
    test_retrieveObjectByUrl();
    test_large_crl_retrieval();
    test_http_retrieval_limits();
    test_verifyRevocation();
    test_ocsp_response_limit();
    test_ocsp_response_validation();
    test_online_crl_verification();
    test_crl_scope_validation();
}
