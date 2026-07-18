/*
 * Copyright 2004-2007 Juan Lang
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
#include <assert.h>
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "winreg.h"
#include "winuser.h"
#include "wine/debug.h"
#include "crypt32_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

typedef struct _WINE_HASH_TO_DELETE
{
    BYTE        hash[20];
    struct list entry;
} WINE_HASH_TO_DELETE;

typedef struct _WINE_REGSTORE_NOTIFY
{
    struct list entry;
    HANDLE      event;
    HANDLE      signal_event;
    HANDLE      change_event;
    HANDLE      wait;
    HKEY        key;
    BOOL        close_signal_event;
} WINE_REGSTORE_NOTIFY;

typedef struct _WINE_REGSTOREINFO
{
    DWORD            dwOpenFlags;
    HCERTSTORE       memStore;
    HKEY             key;
    BOOL             dirty;
    CRITICAL_SECTION cs;
    struct list      certsToDelete;
    struct list      crlsToDelete;
    struct list      ctlsToDelete;
    struct list      notifications;
    HANDLE           key_modified_event;
} WINE_REGSTOREINFO;

void CRYPT_HashToStr(const BYTE *hash, LPWSTR asciiHash)
{
    DWORD i;

    assert(hash);
    assert(asciiHash);

    for (i = 0; i < 20; i++)
        wsprintfW(asciiHash + i * 2, L"%02X", hash[i]);
}

void CRYPT_RegReadSerializedFromReg(HKEY key, DWORD contextType, HCERTSTORE store, DWORD disposition)
{
    LONG rc;
    DWORD index = 0;
    WCHAR subKeyName[MAX_PATH];

    do {
        DWORD size = ARRAY_SIZE(subKeyName);

        rc = RegEnumKeyExW(key, index++, subKeyName, &size, NULL, NULL, NULL,
         NULL);
        if (!rc)
        {
            HKEY subKey;

            rc = RegOpenKeyExW(key, subKeyName, 0, KEY_READ, &subKey);
            if (!rc)
            {
                LPBYTE buf = NULL;

                size = 0;
                rc = RegQueryValueExW(subKey, L"Blob", NULL, NULL, NULL, &size);
                if (!rc)
                    buf = CryptMemAlloc(size);
                if (buf)
                {
                    rc = RegQueryValueExW(subKey, L"Blob", NULL, NULL, buf,
                     &size);
                    if (!rc)
                    {
                        const void *context;
                        DWORD addedType;

                        TRACE("Adding cert with hash %s\n",
                         debugstr_w(subKeyName));
                        context = CRYPT_ReadSerializedElement(buf, size,
                         contextType, &addedType);
                        if (context)
                        {
                            const WINE_CONTEXT_INTERFACE *contextInterface;
                            BYTE hash[20];

                            switch (addedType)
                            {
                            case CERT_STORE_CERTIFICATE_CONTEXT:
                                contextInterface = pCertInterface;
                                break;
                            case CERT_STORE_CRL_CONTEXT:
                                contextInterface = pCRLInterface;
                                break;
                            case CERT_STORE_CTL_CONTEXT:
                                contextInterface = pCTLInterface;
                                break;
                            default:
                                contextInterface = NULL;
                            }
                            if (contextInterface)
                            {
                                size = sizeof(hash);
                                if (contextInterface->getProp(context,
                                 CERT_HASH_PROP_ID, hash, &size))
                                {
                                    WCHAR asciiHash[20 * 2 + 1];

                                    CRYPT_HashToStr(hash, asciiHash);
                                    TRACE("comparing %s\n",
                                     debugstr_w(asciiHash));
                                    TRACE("with %s\n", debugstr_w(subKeyName));
                                    if (!wcscmp(asciiHash, subKeyName))
                                    {
                                        TRACE("hash matches, adding\n");
                                        contextInterface->addContextToStore(
                                         store, context,
                                         disposition, NULL);
                                    }
                                    else
                                        TRACE("hash doesn't match, ignoring\n");
                                }
                                Context_Release(context_from_ptr(context));
                            }
                        }
                    }
                    CryptMemFree(buf);
                }
                RegCloseKey(subKey);
            }
            /* Ignore intermediate errors, continue enumerating */
            rc = ERROR_SUCCESS;
        }
    } while (!rc);
}

static void CRYPT_RegReadFromReg(HKEY key, HCERTSTORE store, DWORD disposition)
{
    static const WCHAR * const subKeys[] = { L"Certificates", L"CRLs", L"CTLs" };
    static const DWORD contextFlags[] = { CERT_STORE_CERTIFICATE_CONTEXT_FLAG,
     CERT_STORE_CRL_CONTEXT_FLAG, CERT_STORE_CTL_CONTEXT_FLAG };
    DWORD i;

    for (i = 0; i < ARRAY_SIZE(subKeys); i++)
    {
        HKEY hKey;
        LONG rc;

        rc = RegCreateKeyExW(key, subKeys[i], 0, NULL, 0, KEY_READ, NULL,
         &hKey, NULL);
        if (!rc)
        {
            CRYPT_RegReadSerializedFromReg(hKey, contextFlags[i], store, disposition);
            RegCloseKey(hKey);
        }
    }
}

/* Hash is assumed to be 20 bytes in length (a SHA-1 hash) */
static BOOL CRYPT_WriteSerializedToReg(HKEY key, DWORD flags, const BYTE *hash, const BYTE *buf,
 DWORD len)
{
    WCHAR asciiHash[20 * 2 + 1];
    LONG rc;
    HKEY subKey;
    BOOL ret;

    CRYPT_HashToStr(hash, asciiHash);
    rc = RegCreateKeyExW(key, asciiHash, 0, NULL, flags, KEY_ALL_ACCESS, NULL,
     &subKey, NULL);
    if (!rc)
    {
        rc = RegSetValueExW(subKey, L"Blob", 0, REG_BINARY, buf, len);
        RegCloseKey(subKey);
    }
    if (!rc)
        ret = TRUE;
    else
    {
        SetLastError(rc);
        ret = FALSE;
    }
    return ret;
}

BOOL CRYPT_SerializeContextToReg(HKEY key, DWORD flags, const WINE_CONTEXT_INTERFACE *context_iface,
 const void *context)
{
    BYTE hash[20];
    DWORD hash_size = sizeof(hash);
    DWORD size = 0;
    BYTE *buf;
    BOOL ret;

    if (!context_iface->getProp(context, CERT_HASH_PROP_ID, hash,  &hash_size))
        return FALSE;

    context_iface->serialize(context, 0, NULL, &size);
    if (!size)
        return FALSE;

    if (!(buf = CryptMemAlloc(size)))
        return FALSE;

    if (!(context_iface->serialize(context, 0, buf, &size)))
    {
        CryptMemFree(buf);
        return FALSE;
    }
    ret = CRYPT_WriteSerializedToReg(key, flags, hash, buf, size);
    CryptMemFree(buf);
    return ret;
}

static BOOL CRYPT_SerializeContextsToReg(HKEY key, DWORD flags,
 const WINE_CONTEXT_INTERFACE *contextInterface, HCERTSTORE memStore)
{
    const void *context = NULL;
    BOOL ret;

    do {
        context = contextInterface->enumContextsInStore(memStore, context);
        ret = !context || CRYPT_SerializeContextToReg(key, flags, contextInterface, context);
    } while (ret && context != NULL);
    if (context)
        Context_Release(context_from_ptr(context));
    return ret;
}

void CRYPT_RegDeleteFromReg(HKEY key, const BYTE *sha1_hash)
{
    WCHAR hash[20 * 2 + 1];

    CRYPT_HashToStr(sha1_hash, hash);
    TRACE("Removing %s\n", debugstr_w(hash));
    RegDeleteKeyW(key, hash);
}

static BOOL CRYPT_RegWriteToReg(WINE_REGSTOREINFO *store)
{
    static const WCHAR * const subKeys[] = { L"Certificates", L"CRLs", L"CTLs" };
    const WINE_CONTEXT_INTERFACE * const interfaces[] = { pCertInterface,
     pCRLInterface, pCTLInterface };
    struct list *listToDelete[] = { &store->certsToDelete, &store->crlsToDelete,
     &store->ctlsToDelete };
    BOOL ret = TRUE;
    DWORD i;

    for (i = 0; ret && i < ARRAY_SIZE(subKeys); i++)
    {
        HKEY key;
        LONG rc = RegCreateKeyExW(store->key, subKeys[i], 0, NULL, 0,
         KEY_ALL_ACCESS, NULL, &key, NULL);

        if (!rc)
        {
            if (listToDelete[i])
            {
                WINE_HASH_TO_DELETE *toDelete, *next;

                EnterCriticalSection(&store->cs);
                LIST_FOR_EACH_ENTRY_SAFE(toDelete, next, listToDelete[i],
                 WINE_HASH_TO_DELETE, entry)
                {
                    CRYPT_RegDeleteFromReg(key, toDelete->hash);
                    list_remove(&toDelete->entry);
                    CryptMemFree(toDelete);
                }
                LeaveCriticalSection(&store->cs);
            }
            ret = CRYPT_SerializeContextsToReg(key, 0, interfaces[i], store->memStore);
            RegCloseKey(key);
        }
        else
        {
            SetLastError(rc);
            ret = FALSE;
        }
    }
    return ret;
}

/* If force is true or the registry store is dirty, writes the contents of the
 * store to the registry.
 */
static BOOL CRYPT_RegFlushStore(WINE_REGSTOREINFO *store, BOOL force)
{
    BOOL ret;

    TRACE("(%p, %d)\n", store, force);

    if (store->dirty || force)
    {
        ret = CRYPT_RegWriteToReg(store);
        if (ret)
            store->dirty = FALSE;
    }
    else
        ret = TRUE;
    return ret;
}

static void CALLBACK CRYPT_RegNotifyChange(void *context, BOOLEAN timed_out)
{
    WINE_REGSTORE_NOTIFY *notify = context;

    if (!timed_out && !SetEvent(notify->signal_event))
        WARN("Failed to signal registry store change event, error %lu.\n", GetLastError());
}

static BOOL CRYPT_RegStopChangeNotification(WINE_REGSTORE_NOTIFY *notify)
{
    if (notify->wait)
    {
        if (!UnregisterWaitEx(notify->wait, INVALID_HANDLE_VALUE))
            return FALSE;
        notify->wait = NULL;
    }
    if (notify->key)
    {
        RegCloseKey(notify->key);
        notify->key = NULL;
    }
    if (notify->change_event)
    {
        CloseHandle(notify->change_event);
        notify->change_event = NULL;
    }
    return TRUE;
}

static void CRYPT_RegFreeChangeNotification(WINE_REGSTORE_NOTIFY *notify)
{
    if (notify->close_signal_event)
        CloseHandle(notify->signal_event);
    CryptMemFree(notify);
}

static BOOL CRYPT_RegArmExternalChangeNotification(WINE_REGSTOREINFO *store,
 WINE_REGSTORE_NOTIFY *notify)
{
    HANDLE change_event, wait;
    HKEY key;
    DWORD err;

    if (!CRYPT_RegStopChangeNotification(notify))
        return FALSE;
    if (!ResetEvent(notify->signal_event))
        return FALSE;
    if (!(change_event = CreateEventW(NULL, FALSE, FALSE, NULL)))
        return FALSE;
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)store->key,
        GetCurrentProcess(), (HANDLE *)&key, KEY_NOTIFY, FALSE, 0))
    {
        CloseHandle(change_event);
        return FALSE;
    }
    if ((err = RegNotifyChangeKeyValue(key, TRUE,
        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
        change_event, TRUE)))
    {
        RegCloseKey(key);
        CloseHandle(change_event);
        SetLastError(err);
        return FALSE;
    }
    if (!RegisterWaitForSingleObject(&wait, change_event, CRYPT_RegNotifyChange,
        notify, INFINITE, WT_EXECUTEONLYONCE))
    {
        err = GetLastError();
        RegCloseKey(key);
        CloseHandle(change_event);
        SetLastError(err);
        return FALSE;
    }
    notify->change_event = change_event;
    notify->wait = wait;
    notify->key = key;
    return TRUE;
}

static WINE_REGSTORE_NOTIFY *CRYPT_RegFindChangeNotification(
 WINE_REGSTOREINFO *store, HANDLE event)
{
    WINE_REGSTORE_NOTIFY *notify;

    LIST_FOR_EACH_ENTRY(notify, &store->notifications, WINE_REGSTORE_NOTIFY, entry)
        if (notify->event == event)
            return notify;
    return NULL;
}

static BOOL CRYPT_RegAddChangeNotification(WINE_REGSTOREINFO *store,
 DWORD flags, HANDLE event)
{
    WINE_REGSTORE_NOTIFY *notify;
    DWORD err;

    if (flags & ~CERT_STORE_CTRL_INHIBIT_DUPLICATE_HANDLE_FLAG)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (CRYPT_RegFindChangeNotification(store, event))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(notify = CryptMemAlloc(sizeof(*notify))))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    notify->event = event;
    notify->change_event = NULL;
    notify->wait = NULL;
    notify->key = NULL;
    notify->close_signal_event = !(flags & CERT_STORE_CTRL_INHIBIT_DUPLICATE_HANDLE_FLAG);
    if (notify->close_signal_event)
    {
        if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
            &notify->signal_event, 0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            CryptMemFree(notify);
            return FALSE;
        }
    }
    else
        notify->signal_event = event;

    if (!CRYPT_RegArmExternalChangeNotification(store, notify))
    {
        err = GetLastError();
        CRYPT_RegFreeChangeNotification(notify);
        SetLastError(err);
        return FALSE;
    }
    list_add_tail(&store->notifications, &notify->entry);
    return TRUE;
}

static BOOL CRYPT_RegCancelChangeNotification(WINE_REGSTOREINFO *store,
 HANDLE event)
{
    WINE_REGSTORE_NOTIFY *notify;

    if (!(notify = CRYPT_RegFindChangeNotification(store, event)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!CRYPT_RegStopChangeNotification(notify))
        return FALSE;
    list_remove(&notify->entry);
    CRYPT_RegFreeChangeNotification(notify);
    return TRUE;
}

static void WINAPI CRYPT_RegCloseStore(HCERTSTORE hCertStore, DWORD dwFlags)
{
    WINE_REGSTOREINFO *store = hCertStore;
    WINE_REGSTORE_NOTIFY *notify, *next;

    TRACE("(%p, %08lx)\n", store, dwFlags);
    if (dwFlags)
        FIXME("Unimplemented flags: %08lx\n", dwFlags);

    CRYPT_RegFlushStore(store, FALSE);
    LIST_FOR_EACH_ENTRY_SAFE(notify, next, &store->notifications, WINE_REGSTORE_NOTIFY, entry)
    {
        if (!CRYPT_RegStopChangeNotification(notify))
        {
            ERR("Failed to stop registry store change notification, error %lu.\n", GetLastError());
            continue;
        }
        list_remove(&notify->entry);
        CRYPT_RegFreeChangeNotification(notify);
    }
    RegCloseKey(store->key);
    CloseHandle(store->key_modified_event);
    store->cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&store->cs);
    CryptMemFree(store);
}

static BOOL CRYPT_RegWriteContext(WINE_REGSTOREINFO *store,
 const void *context, DWORD dwFlags)
{
    BOOL ret;

    if (dwFlags & CERT_STORE_PROV_WRITE_ADD_FLAG)
    {
        store->dirty = TRUE;
        ret = TRUE;
    }
    else
        ret = FALSE;
    return ret;
}

static BOOL CRYPT_RegDeleteContext(WINE_REGSTOREINFO *store,
 struct list *deleteList, const void *context,
 const WINE_CONTEXT_INTERFACE *contextInterface)
{
    BOOL ret;

    if (store->dwOpenFlags & CERT_STORE_READONLY_FLAG)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        ret = FALSE;
    }
    else
    {
        WINE_HASH_TO_DELETE *toDelete = CryptMemAlloc(sizeof(WINE_HASH_TO_DELETE));

        if (toDelete)
        {
            DWORD size = sizeof(toDelete->hash);

            ret = contextInterface->getProp(context, CERT_HASH_PROP_ID,
             toDelete->hash, &size);
            if (ret)
            {
                EnterCriticalSection(&store->cs);
                list_add_tail(deleteList, &toDelete->entry);
                LeaveCriticalSection(&store->cs);
            }
            else
            {
                CryptMemFree(toDelete);
                ret = FALSE;
            }
        }
        else
            ret = FALSE;
        if (ret)
            store->dirty = TRUE;
    }
    return ret;
}

static BOOL WINAPI CRYPT_RegWriteCert(HCERTSTORE hCertStore,
 PCCERT_CONTEXT cert, DWORD dwFlags)
{
    WINE_REGSTOREINFO *store = hCertStore;

    TRACE("(%p, %p, %ld)\n", hCertStore, cert, dwFlags);

    return CRYPT_RegWriteContext(store, cert, dwFlags);
}

static BOOL WINAPI CRYPT_RegDeleteCert(HCERTSTORE hCertStore,
 PCCERT_CONTEXT pCertContext, DWORD dwFlags)
{
    WINE_REGSTOREINFO *store = hCertStore;

    TRACE("(%p, %p, %08lx)\n", store, pCertContext, dwFlags);

    return CRYPT_RegDeleteContext(store, &store->certsToDelete, pCertContext,
     pCertInterface);
}

static BOOL WINAPI CRYPT_RegWriteCRL(HCERTSTORE hCertStore,
 PCCRL_CONTEXT crl, DWORD dwFlags)
{
    WINE_REGSTOREINFO *store = hCertStore;

    TRACE("(%p, %p, %ld)\n", hCertStore, crl, dwFlags);

    return CRYPT_RegWriteContext(store, crl, dwFlags);
}

static BOOL WINAPI CRYPT_RegDeleteCRL(HCERTSTORE hCertStore,
 PCCRL_CONTEXT pCrlContext, DWORD dwFlags)
{
    WINE_REGSTOREINFO *store = hCertStore;

    TRACE("(%p, %p, %08lx)\n", store, pCrlContext, dwFlags);

    return CRYPT_RegDeleteContext(store, &store->crlsToDelete, pCrlContext,
     pCRLInterface);
}

static BOOL WINAPI CRYPT_RegWriteCTL(HCERTSTORE hCertStore,
 PCCTL_CONTEXT ctl, DWORD dwFlags)
{
    WINE_REGSTOREINFO *store = hCertStore;

    TRACE("(%p, %p, %ld)\n", hCertStore, ctl, dwFlags);

    return CRYPT_RegWriteContext(store, ctl, dwFlags);
}

static BOOL WINAPI CRYPT_RegDeleteCTL(HCERTSTORE hCertStore,
 PCCTL_CONTEXT pCtlContext, DWORD dwFlags)
{
    WINE_REGSTOREINFO *store = hCertStore;

    TRACE("(%p, %p, %08lx)\n", store, pCtlContext, dwFlags);

    return CRYPT_RegDeleteContext(store, &store->ctlsToDelete, pCtlContext,
     pCTLInterface);
}

static BOOL CRYPT_RegArmChangeNotification(WINE_REGSTOREINFO *store)
{
    DWORD err;

    if (!store->key_modified_event &&
        !(store->key_modified_event = CreateEventW(NULL, FALSE, FALSE, NULL)))
        return FALSE;

    if ((err = RegNotifyChangeKeyValue(store->key, TRUE,
            REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
            store->key_modified_event, TRUE)))
    {
        CloseHandle(store->key_modified_event);
        store->key_modified_event = NULL;
        SetLastError(err);
        return FALSE;
    }
    return TRUE;
}

static BOOL CRYPT_RegReloadStore(WINE_REGSTOREINFO *store)
{
    HCERTSTORE mem_store;
    BOOL ret;

    if (!(mem_store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
        CERT_STORE_CREATE_NEW_FLAG, NULL)))
        return FALSE;
    CRYPT_RegReadFromReg(store->key, mem_store, CERT_STORE_ADD_REPLACE_EXISTING);
    ret = I_CertUpdateStore(store->memStore, mem_store, 0, 0);
    CertCloseStore(mem_store, 0);
    return ret;
}

static BOOL WINAPI CRYPT_RegControl(HCERTSTORE hCertStore, DWORD dwFlags,
 DWORD dwCtrlType, void const *pvCtrlPara)
{
    WINE_REGSTOREINFO *store = hCertStore;
    BOOL ret = TRUE;

    TRACE("(%p, %08lx, %ld, %p)\n", hCertStore, dwFlags, dwCtrlType,
     pvCtrlPara);

    switch (dwCtrlType)
    {
    case CERT_STORE_CTRL_RESYNC:
    {
        DWORD wait;

        if (dwFlags)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            ret = FALSE;
            break;
        }
        EnterCriticalSection(&store->cs);
        ret = CRYPT_RegFlushStore(store, FALSE);
        if (pvCtrlPara)
        {
            WINE_REGSTORE_NOTIFY *notify;

            notify = CRYPT_RegFindChangeNotification(store, *(const HANDLE *)pvCtrlPara);
            if (!notify)
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                ret = FALSE;
            }
            else
            {
                if (!CRYPT_RegArmExternalChangeNotification(store, notify))
                    ret = FALSE;
                if (ret)
                    ret = CRYPT_RegReloadStore(store);
            }
        }
        else if (!store->key_modified_event && !CRYPT_RegArmChangeNotification(store))
            ret = FALSE;
        else if ((wait = WaitForSingleObject(store->key_modified_event, 0)) == WAIT_OBJECT_0)
        {
            if (!CRYPT_RegArmChangeNotification(store))
                ret = FALSE;
            if (ret)
                ret = CRYPT_RegReloadStore(store);
        }
        else if (wait != WAIT_TIMEOUT)
        {
            ret = FALSE;
        }
        LeaveCriticalSection(&store->cs);
        break;
    }
    case CERT_STORE_CTRL_COMMIT:
        ret = CRYPT_RegFlushStore(store,
         dwFlags & CERT_STORE_CTRL_COMMIT_FORCE_FLAG);
        break;
    case CERT_STORE_CTRL_AUTO_RESYNC:
        FIXME("CERT_STORE_CTRL_AUTO_RESYNC: stub\n");
        SetLastError(ERROR_NOT_SUPPORTED);
        ret = FALSE;
        break;
    case CERT_STORE_CTRL_NOTIFY_CHANGE:
        if ((!pvCtrlPara && dwFlags) ||
            (dwFlags & ~CERT_STORE_CTRL_INHIBIT_DUPLICATE_HANDLE_FLAG))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            ret = FALSE;
            break;
        }
        EnterCriticalSection(&store->cs);
        if (pvCtrlPara)
            ret = CRYPT_RegAddChangeNotification(store, dwFlags,
                *(const HANDLE *)pvCtrlPara);
        else
            ret = store->key_modified_event || CRYPT_RegArmChangeNotification(store);
        LeaveCriticalSection(&store->cs);
        break;
    case CERT_STORE_CTRL_CANCEL_NOTIFY:
        if (dwFlags || !pvCtrlPara)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            ret = FALSE;
            break;
        }
        EnterCriticalSection(&store->cs);
        ret = CRYPT_RegCancelChangeNotification(store, *(const HANDLE *)pvCtrlPara);
        LeaveCriticalSection(&store->cs);
        break;
    default:
        FIXME("%lu: stub\n", dwCtrlType);
        SetLastError(ERROR_NOT_SUPPORTED);
        ret = FALSE;
    }
    return ret;
}

static void *regProvFuncs[] = {
    CRYPT_RegCloseStore,
    NULL, /* CERT_STORE_PROV_READ_CERT_FUNC */
    CRYPT_RegWriteCert,
    CRYPT_RegDeleteCert,
    NULL, /* CERT_STORE_PROV_SET_CERT_PROPERTY_FUNC */
    NULL, /* CERT_STORE_PROV_READ_CRL_FUNC */
    CRYPT_RegWriteCRL,
    CRYPT_RegDeleteCRL,
    NULL, /* CERT_STORE_PROV_SET_CRL_PROPERTY_FUNC */
    NULL, /* CERT_STORE_PROV_READ_CTL_FUNC */
    CRYPT_RegWriteCTL,
    CRYPT_RegDeleteCTL,
    NULL, /* CERT_STORE_PROV_SET_CTL_PROPERTY_FUNC */
    CRYPT_RegControl,
};

WINECRYPT_CERTSTORE *CRYPT_RegOpenStore(HCRYPTPROV hCryptProv, DWORD dwFlags,
 const void *pvPara)
{
    WINECRYPT_CERTSTORE *store = NULL;

    TRACE("(%Id, %08lx, %p)\n", hCryptProv, dwFlags, pvPara);

    if (dwFlags & CERT_STORE_DELETE_FLAG)
    {
        DWORD rc = RegDeleteTreeW((HKEY)pvPara, L"Certificates");

        if (rc == ERROR_SUCCESS || rc == ERROR_NO_MORE_ITEMS)
            rc = RegDeleteTreeW((HKEY)pvPara, L"CRLs");
        if (rc == ERROR_SUCCESS || rc == ERROR_NO_MORE_ITEMS)
            rc = RegDeleteTreeW((HKEY)pvPara, L"CTLs");
        if (rc == ERROR_NO_MORE_ITEMS)
            rc = ERROR_SUCCESS;
        SetLastError(rc);
    }
    else
    {
        HKEY key;

        if (DuplicateHandle(GetCurrentProcess(), (HANDLE)pvPara,
         GetCurrentProcess(), (LPHANDLE)&key,
         dwFlags & CERT_STORE_READONLY_FLAG ? KEY_READ : KEY_ALL_ACCESS,
         TRUE, 0))
        {
            WINECRYPT_CERTSTORE *memStore;

            memStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, hCryptProv,
             CERT_STORE_CREATE_NEW_FLAG, NULL);
            if (memStore)
            {
                WINE_REGSTOREINFO *regInfo = CryptMemAlloc(
                 sizeof(WINE_REGSTOREINFO));

                if (regInfo)
                {
                    CERT_STORE_PROV_INFO provInfo = { 0 };

                    regInfo->dwOpenFlags = dwFlags;
                    regInfo->memStore = memStore;
                    regInfo->key = key;
                    InitializeCriticalSectionEx(&regInfo->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
                    regInfo->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": PWINE_REGSTOREINFO->cs");
                    list_init(&regInfo->certsToDelete);
                    list_init(&regInfo->crlsToDelete);
                    list_init(&regInfo->ctlsToDelete);
                    list_init(&regInfo->notifications);
                    regInfo->key_modified_event = NULL;
                    if (!CRYPT_RegArmChangeNotification(regInfo))
                        ERR("RegNotifyChangeKeyValue failed.\n");
                    CRYPT_RegReadFromReg(regInfo->key, regInfo->memStore, CERT_STORE_ADD_ALWAYS);
                    regInfo->dirty = FALSE;
                    provInfo.cbSize = sizeof(provInfo);
                    provInfo.cStoreProvFunc = ARRAY_SIZE(regProvFuncs);
                    provInfo.rgpvStoreProvFunc = regProvFuncs;
                    provInfo.hStoreProv = regInfo;
                    store = CRYPT_ProvCreateStore(dwFlags, memStore, &provInfo);
                    /* Reg store doesn't need crypto provider, so close it */
                    if (hCryptProv &&
                     !(dwFlags & CERT_STORE_NO_CRYPT_RELEASE_FLAG))
                        CryptReleaseContext(hCryptProv, 0);
                }
            }
        }
    }
    TRACE("returning %p\n", store);
    return store;
}
