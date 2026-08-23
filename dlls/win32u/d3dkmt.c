/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <pthread.h>

#include "ntstatus.h"
#include "ntgdi_private.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "d3dkmdt.h"
#include "d3dkmt_private.h"

#include <d3d9types.h>
#include <dxgi.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>

WINE_DEFAULT_DEBUG_CHANNEL(d3dkmt);

/* D3DKMT runtime descriptors */

struct d3dkmt_dxgi_desc
{
    UINT                        size;
    UINT                        version;
    UINT                        width;
    UINT                        height;
    DXGI_FORMAT                 format;
    UINT                        unknown_0;
    UINT                        unknown_1;
    UINT                        keyed_mutex;
    D3DKMT_HANDLE               mutex_handle;
    D3DKMT_HANDLE               sync_handle;
    UINT                        nt_shared;
    UINT                        unknown_2;
    UINT                        unknown_3;
    UINT                        unknown_4;
};

struct d3dkmt_d3d9_desc
{
    struct d3dkmt_dxgi_desc     dxgi;
    D3DFORMAT                   format;
    D3DRESOURCETYPE             type;
    UINT                        usage;
    union
    {
        struct
        {
            UINT                unknown_0;
            UINT                width;
            UINT                height;
            UINT                levels;
            UINT                depth;
        } texture;
        struct
        {
            UINT                unknown_0;
            UINT                unknown_1;
            UINT                unknown_2;
            UINT                width;
            UINT                height;
        } surface;
        struct
        {
            UINT                unknown_0;
            UINT                width;
            UINT                format;
            UINT                unknown_1;
            UINT                unknown_2;
        } buffer;
    };
};

C_ASSERT( sizeof(struct d3dkmt_d3d9_desc) == 0x58 );

struct d3dkmt_d3d11_desc
{
    struct d3dkmt_dxgi_desc     dxgi;
    D3D11_RESOURCE_DIMENSION    dimension;
    union
    {
        D3D10_BUFFER_DESC       d3d10_buf;
        D3D10_TEXTURE1D_DESC    d3d10_1d;
        D3D10_TEXTURE2D_DESC    d3d10_2d;
        D3D10_TEXTURE3D_DESC    d3d10_3d;
        D3D11_BUFFER_DESC       d3d11_buf;
        D3D11_TEXTURE1D_DESC    d3d11_1d;
        D3D11_TEXTURE2D_DESC    d3d11_2d;
        D3D11_TEXTURE3D_DESC    d3d11_3d;
    };
};

C_ASSERT( sizeof(struct d3dkmt_d3d11_desc) == 0x68 );

struct d3dkmt_d3d12_desc
{
    struct d3dkmt_d3d11_desc    d3d11;
    UINT                        unknown_5[4];
    UINT                        resource_size;
    UINT                        unknown_6[7];
    UINT                        resource_align;
    UINT                        unknown_7[9];
    union
    {
        D3D12_RESOURCE_DESC     desc;
        D3D12_RESOURCE_DESC1    desc1;
    };
    UINT64                      unknown_8[1];
};

C_ASSERT( sizeof(struct d3dkmt_d3d12_desc) == 0x108 );
C_ASSERT( offsetof(struct d3dkmt_d3d12_desc, unknown_5) == sizeof(struct d3dkmt_d3d11_desc) );

union d3dkmt_desc
{
    struct d3dkmt_dxgi_desc     dxgi;
    struct d3dkmt_d3d9_desc     d3d9;
    struct d3dkmt_d3d11_desc    d3d11;
    struct d3dkmt_d3d12_desc    d3d12;
};

struct d3dkmt_object
{
    enum d3dkmt_type    type;           /* object type */
    D3DKMT_HANDLE       local;          /* object local handle */
    D3DKMT_HANDLE       global;         /* object global handle */
    BOOL                shared;         /* object is shared using nt handles */
    HANDLE              handle;         /* internal handle of the server object */
    unsigned int        pin_count;      /* transient native operation references */
    BOOL                delete_pending; /* local handle removed while pinned */
};

struct d3dkmt_mutex
{
    struct d3dkmt_object obj;
    BOOL owned;
};

struct d3dkmt_resource
{
    struct d3dkmt_object obj;
    D3DKMT_HANDLE allocation;
};

struct d3dkmt_adapter
{
    struct d3dkmt_object obj;
    struct vulkan_physical_device *physical_device;
};

struct d3dkmt_device
{
    struct d3dkmt_object obj;
};

struct d3dkmt_vidpn_source
{
    D3DKMT_VIDPNSOURCEOWNER_TYPE type;      /* VidPN source owner type */
    D3DDDI_VIDEO_PRESENT_SOURCE_ID id;      /* VidPN present source id */
    D3DKMT_HANDLE device;                   /* Kernel mode device context */
    struct list entry;                      /* List entry */
};

static pthread_mutex_t d3dkmt_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list d3dkmt_vidpn_sources = LIST_INIT( d3dkmt_vidpn_sources );   /* VidPN source information list */

static struct d3dkmt_object **objects, **objects_end, **objects_next;

#define D3DKMT_HANDLE_BIT  0x40000000

static BOOL is_d3dkmt_global( D3DKMT_HANDLE handle )
{
    return (handle & 0xc0000000) && (handle & 0x3f) == 2;
}

static D3DKMT_HANDLE index_to_handle( int index )
{
    return (index << 6) | D3DKMT_HANDLE_BIT;
}

static int handle_to_index( D3DKMT_HANDLE handle )
{
    return (handle & ~0xc0000000) >> 6;
}

static NTSTATUS init_handle_table(void)
{
    if (!(objects = calloc( 1024, sizeof(*objects) ))) return STATUS_NO_MEMORY;
    objects_end = objects + 1024;
    objects_next = objects;
    return STATUS_SUCCESS;
}

static struct d3dkmt_object **grow_handle_table(void)
{
    size_t old_capacity = objects_end - objects, max_capacity = handle_to_index( D3DKMT_HANDLE_BIT - 1 );
    unsigned int new_capacity = old_capacity * 3 / 2;
    struct d3dkmt_object **tmp;

    if (new_capacity > max_capacity) new_capacity = max_capacity;
    if (new_capacity <= old_capacity) return NULL; /* exhausted handle capacity */

    if (!(tmp = realloc( objects, new_capacity * sizeof(*objects) ))) return NULL;
    memset( tmp + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*tmp) );

    objects = tmp;
    objects_end = tmp + new_capacity;
    objects_next = tmp + old_capacity;

    return objects_next;
}

/* allocate a d3dkmt object with a local handle */
static NTSTATUS alloc_object_handle( struct d3dkmt_object *object )
{
    struct d3dkmt_object **entry;

    pthread_mutex_lock( &d3dkmt_lock );
    if (!objects && init_handle_table()) goto done;

    for (entry = objects_next; entry < objects_end; entry++) if (!*entry) break;
    if (entry == objects_end)
    {
        for (entry = objects; entry < objects_next; entry++) if (!*entry) break;
        if (entry == objects_next && !(entry = grow_handle_table())) goto done;
    }

    object->local = index_to_handle( entry - objects );
    objects_next = entry + 1;
    *entry = object;

done:
    pthread_mutex_unlock( &d3dkmt_lock );
    return object->local ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* Pin a handle-table object across validation and the operation that consumes it. */
static void *pin_d3dkmt_object( D3DKMT_HANDLE local, enum d3dkmt_type type )
{
    unsigned int index = handle_to_index( local );
    struct d3dkmt_object *object;

    pthread_mutex_lock( &d3dkmt_lock );
    if (!objects || index >= objects_end - objects) object = NULL;
    else object = objects[index];
    if (!object || object->local != local || object->delete_pending ||
        object->pin_count == ~0u || (type != -1 && object->type != type)) object = NULL;
    else object->pin_count++;
    pthread_mutex_unlock( &d3dkmt_lock );
    return object;
}

/* Validate a handle without borrowing the object.  The caller must hold
 * d3dkmt_lock, so a consuming close cannot race the metadata check. */
static BOOL validate_d3dkmt_object_handle_locked( D3DKMT_HANDLE local,
                                                   enum d3dkmt_type type )
{
    unsigned int index = handle_to_index( local );
    struct d3dkmt_object *object;

    if (!objects || index >= objects_end - objects) object = NULL;
    else object = objects[index];
    return object && object->local == local && !object->delete_pending &&
           (type == -1 || object->type == type);
}

static BOOL validate_d3dkmt_object_handle( D3DKMT_HANDLE local,
                                            enum d3dkmt_type type )
{
    BOOL valid;

    pthread_mutex_lock( &d3dkmt_lock );
    valid = validate_d3dkmt_object_handle_locked( local, type );
    pthread_mutex_unlock( &d3dkmt_lock );
    return valid;
}

static void destroy_d3dkmt_object( struct d3dkmt_object *object )
{
    /* No table entry or operation pin can still access the object here. */
    if (object->type == D3DKMT_MUTEX && ((struct d3dkmt_mutex *)object)->owned)
    {
        SERVER_START_REQ( d3dkmt_mutex_release )
        {
            req->mutex = object->global;
            req->abandon = 1;
            wine_server_call( req );
        }
        SERVER_END_REQ;
    }
    if (object->handle) NtClose( object->handle );
    free( object );
}

static void unpin_d3dkmt_object( struct d3dkmt_object *object )
{
    BOOL destroy = FALSE;

    pthread_mutex_lock( &d3dkmt_lock );
    assert( object->pin_count );
    if (!--object->pin_count && object->delete_pending) destroy = TRUE;
    pthread_mutex_unlock( &d3dkmt_lock );
    if (destroy) destroy_d3dkmt_object( object );
}

static NTSTATUS d3dkmt_object_alloc( UINT size, enum d3dkmt_type type, void **obj )
{
    struct d3dkmt_object *object;

    if (!(object = calloc( 1, size ))) return STATUS_NO_MEMORY;
    object->type = type;

    *obj = object;
    return STATUS_SUCCESS;
}

/* create a global D3DKMT object, either with a global handle or later shareable */
static NTSTATUS d3dkmt_object_create( struct d3dkmt_object *object, int fd, UINT value, BOOL shared,
                                      const void *runtime, UINT runtime_size )
{
    NTSTATUS status;

    if (fd >= 0) wine_server_send_fd( fd );

    SERVER_START_REQ( d3dkmt_object_create )
    {
        req->type = object->type;
        req->fd = fd;
        req->value = value;
        if (runtime_size) wine_server_add_data( req, runtime, runtime_size );
        status = wine_server_call( req );
        object->handle = wine_server_ptr_handle( reply->handle );
        object->global = reply->global;
        object->shared = shared;
    }
    SERVER_END_REQ;

    if (!status) status = alloc_object_handle( object );

    if (status) WARN( "Failed to create global object for %p, status %#x\n", object, status );
    else TRACE( "Created global object %#x for %p/%#x\n", object->global, object, object->local );
    return status;
}

static NTSTATUS d3dkmt_object_update( struct d3dkmt_object *object, const void *runtime, UINT runtime_size )
{
    NTSTATUS status;

    SERVER_START_REQ( d3dkmt_object_update )
    {
        req->type = object->type;
        req->global = object->global;
        if (runtime_size) wine_server_add_data( req, runtime, runtime_size );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    if (status) WARN( "Failed to update object %#x/%p global %#x, status %#x\n", object->local, object, object->global, status );
    else TRACE( "Updated object %#x/%p global %#x\n", object->local, object, object->global );
    return status;
}

static NTSTATUS d3dkmt_object_open( struct d3dkmt_object *obj, D3DKMT_HANDLE global, HANDLE handle,
                                    void *runtime, UINT *runtime_size )
{
    NTSTATUS status;

    SERVER_START_REQ( d3dkmt_object_open )
    {
        req->type = obj->type;
        req->global = global;
        req->handle = wine_server_obj_handle( handle );
        if (runtime) wine_server_set_reply( req, runtime, *runtime_size );
        status = wine_server_call( req );
        obj->handle = wine_server_ptr_handle( reply->handle );
        obj->global = reply->global;
        obj->shared = !global;
        *runtime_size = reply->runtime_size;
    }
    SERVER_END_REQ;
    if (!status) status = alloc_object_handle( obj );

    if (status) WARN( "Failed to open global object %#x/%p, status %#x\n", global, handle, status );
    else TRACE( "Opened global object %#x/%p as %p/%#x\n", global, handle, obj, obj->local );
    return status;
}

static NTSTATUS d3dkmt_object_query( enum d3dkmt_type type, D3DKMT_HANDLE global, HANDLE handle,
                                     UINT *runtime_size )
{
    NTSTATUS status;

    SERVER_START_REQ( d3dkmt_object_query )
    {
        req->type = type;
        req->global = global;
        req->handle = wine_server_obj_handle( handle );
        status = wine_server_call( req );
        *runtime_size = reply->runtime_size;
    }
    SERVER_END_REQ;

    if (status) WARN( "Failed to query global object %#x/%p, status %#x\n", global, handle, status );
    else TRACE( "Found global object %#x/%p with runtime size %#x\n", global, handle, *runtime_size );
    return status;
}

static void d3dkmt_object_free( struct d3dkmt_object *object )
{
    BOOL destroy = FALSE;

    TRACE( "object %p/%#x, global %#x\n", object, object->local, object->global );
    pthread_mutex_lock( &d3dkmt_lock );
    if (object->local)
    {
        unsigned int index = handle_to_index( object->local );

        assert( objects + index < objects_end && objects[index] == object );
        objects[index] = NULL;
        object->local = 0;
    }
    if (object->pin_count) object->delete_pending = TRUE;
    else destroy = TRUE;
    pthread_mutex_unlock( &d3dkmt_lock );
    if (destroy) destroy_d3dkmt_object( object );
}

static void remove_vidpn_source_owners_locked( D3DKMT_HANDLE device )
{
    struct d3dkmt_vidpn_source *source, *next;

    LIST_FOR_EACH_ENTRY_SAFE( source, next, &d3dkmt_vidpn_sources,
                              struct d3dkmt_vidpn_source, entry )
    {
        if (source->device != device) continue;
        list_remove( &source->entry );
        free( source );
    }
}

/* Remove a caller-owned handle and acquire the destruction responsibility in
 * one locked step.  This is the consuming counterpart to pin_d3dkmt_object():
 * exactly one concurrent close wins, while an already-pinned operation keeps
 * the detached object alive until its final unpin. */
static NTSTATUS release_d3dkmt_object_handle( D3DKMT_HANDLE local,
                                               enum d3dkmt_type type )
{
    unsigned int index = handle_to_index( local );
    struct d3dkmt_object *object = NULL;
    D3DKMT_HANDLE global = 0;
    BOOL destroy = FALSE;

    if (!local) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &d3dkmt_lock );
    if (objects && index < objects_end - objects)
        object = objects[index];
    if (!object || object->local != local || object->delete_pending ||
        object->type != type)
        object = NULL;
    else
    {
        if (type == D3DKMT_DEVICE)
            remove_vidpn_source_owners_locked( local );
        objects[index] = NULL;
        global = object->global;
        object->local = 0;
        if (object->pin_count) object->delete_pending = TRUE;
        else destroy = TRUE;
    }
    pthread_mutex_unlock( &d3dkmt_lock );

    if (!object) return STATUS_INVALID_PARAMETER;
    TRACE( "released handle %#x for object %p, global %#x\n", local, object,
           global );
    if (destroy) destroy_d3dkmt_object( object );
    return STATUS_SUCCESS;
}

/* create a struct security_descriptor and contained information in one contiguous piece of memory */
static unsigned int alloc_object_attributes( const OBJECT_ATTRIBUTES *attr, struct object_attributes **ret,
                                             data_size_t *ret_len )
{
    unsigned int len = sizeof(**ret);
    SID *owner = NULL, *group = NULL;
    ACL *dacl = NULL, *sacl = NULL;
    SECURITY_DESCRIPTOR *sd;

    *ret = NULL;
    *ret_len = 0;

    if (!attr) return STATUS_SUCCESS;

    if (attr->Length != sizeof(*attr)) return STATUS_INVALID_PARAMETER;

    if ((sd = attr->SecurityDescriptor))
    {
        len += sizeof(struct security_descriptor);
    if (sd->Revision != SECURITY_DESCRIPTOR_REVISION) return STATUS_UNKNOWN_REVISION;
        if (sd->Control & SE_SELF_RELATIVE)
        {
            SECURITY_DESCRIPTOR_RELATIVE *rel = (SECURITY_DESCRIPTOR_RELATIVE *)sd;
            if (rel->Owner) owner = (PSID)((BYTE *)rel + rel->Owner);
            if (rel->Group) group = (PSID)((BYTE *)rel + rel->Group);
            if ((sd->Control & SE_SACL_PRESENT) && rel->Sacl) sacl = (PSID)((BYTE *)rel + rel->Sacl);
            if ((sd->Control & SE_DACL_PRESENT) && rel->Dacl) dacl = (PSID)((BYTE *)rel + rel->Dacl);
        }
        else
        {
            owner = sd->Owner;
            group = sd->Group;
            if (sd->Control & SE_SACL_PRESENT) sacl = sd->Sacl;
            if (sd->Control & SE_DACL_PRESENT) dacl = sd->Dacl;
        }

        if (owner) len += offsetof( SID, SubAuthority[owner->SubAuthorityCount] );
        if (group) len += offsetof( SID, SubAuthority[group->SubAuthorityCount] );
        if (sacl) len += sacl->AclSize;
        if (dacl) len += dacl->AclSize;

        /* fix alignment for the Unicode name that follows the structure */
        len = (len + sizeof(WCHAR) - 1) & ~(sizeof(WCHAR) - 1);
    }

    if (attr->ObjectName)
    {
        if ((ULONG_PTR)attr->ObjectName->Buffer & (sizeof(WCHAR) - 1)) return STATUS_DATATYPE_MISALIGNMENT;
        if (attr->ObjectName->Length & (sizeof(WCHAR) - 1)) return STATUS_OBJECT_NAME_INVALID;
        len += attr->ObjectName->Length;
    }
    else if (attr->RootDirectory) return STATUS_OBJECT_NAME_INVALID;

    len = (len + 3) & ~3;  /* DWORD-align the entire structure */

    if (!(*ret = calloc( len, 1 ))) return STATUS_NO_MEMORY;

    (*ret)->rootdir = wine_server_obj_handle( attr->RootDirectory );
    (*ret)->attributes = attr->Attributes;

    if (attr->SecurityDescriptor)
    {
        struct security_descriptor *descr = (struct security_descriptor *)(*ret + 1);
        unsigned char *ptr = (unsigned char *)(descr + 1);

        descr->control = sd->Control & ~SE_SELF_RELATIVE;
        if (owner) descr->owner_len = offsetof( SID, SubAuthority[owner->SubAuthorityCount] );
        if (group) descr->group_len = offsetof( SID, SubAuthority[group->SubAuthorityCount] );
        if (sacl) descr->sacl_len = sacl->AclSize;
        if (dacl) descr->dacl_len = dacl->AclSize;

        memcpy( ptr, owner, descr->owner_len );
        ptr += descr->owner_len;
        memcpy( ptr, group, descr->group_len );
        ptr += descr->group_len;
        memcpy( ptr, sacl, descr->sacl_len );
        ptr += descr->sacl_len;
        memcpy( ptr, dacl, descr->dacl_len );
        (*ret)->sd_len = (sizeof(*descr) + descr->owner_len + descr->group_len + descr->sacl_len +
                          descr->dacl_len + sizeof(WCHAR) - 1) & ~(sizeof(WCHAR) - 1);
    }

    if (attr->ObjectName)
    {
        unsigned char *ptr = (unsigned char *)(*ret + 1) + (*ret)->sd_len;
        (*ret)->name_len = attr->ObjectName->Length;
        memcpy( ptr, attr->ObjectName->Buffer, (*ret)->name_len );
    }

    *ret_len = len;
    return STATUS_SUCCESS;
}

static struct vulkan_instance *d3dkmt_vulkan_instance; /* Vulkan instance for D3DKMT functions */

static void d3dkmt_init_vulkan(void)
{
    static const struct vulkan_instance_extensions extensions =
    {
        .has_VK_KHR_get_physical_device_properties2 = 1,
        .has_VK_KHR_external_fence_capabilities = 1,
    };

    d3dkmt_vulkan_instance = vulkan_instance_create( &extensions );
    if (!d3dkmt_vulkan_instance) WARN( "Failed to create the vulkan instance\n" );
}

static struct vulkan_instance *get_d3dkmt_vulkan_instance(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once( &once, d3dkmt_init_vulkan );
    return d3dkmt_vulkan_instance;
}

static unsigned int validate_open_object_attributes( const OBJECT_ATTRIBUTES *attr )
{
    if (!attr || attr->Length != sizeof(*attr)) return STATUS_INVALID_PARAMETER;

    if (attr->ObjectName)
    {
        if ((ULONG_PTR)attr->ObjectName->Buffer & (sizeof(WCHAR) - 1)) return STATUS_DATATYPE_MISALIGNMENT;
        if (attr->ObjectName->Length & (sizeof(WCHAR) - 1)) return STATUS_OBJECT_NAME_INVALID;
    }
    else if (attr->RootDirectory) return STATUS_OBJECT_NAME_INVALID;

    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDIOpenAdapterFromHdc    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenAdapterFromHdc( D3DKMT_OPENADAPTERFROMHDC *desc )
{
    FIXME( "(%p): stub\n", desc );
    return STATUS_NO_MEMORY;
}

/******************************************************************************
 *           NtGdiDdDDIEscape    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIEscape( const D3DKMT_ESCAPE *desc )
{
    switch (desc->Type)
    {
    case D3DKMT_ESCAPE_UPDATE_RESOURCE_WINE:
    {
        struct d3dkmt_resource *resource;
        NTSTATUS status;

        TRACE( "D3DKMT_ESCAPE_UPDATE_RESOURCE_WINE hContext %#x, pPrivateDriverData %p, PrivateDriverDataSize %#x\n",
               desc->hContext, desc->pPrivateDriverData, desc->PrivateDriverDataSize );

        if (!(resource = pin_d3dkmt_object( desc->hContext, D3DKMT_RESOURCE ))) return STATUS_INVALID_PARAMETER;
        status = d3dkmt_object_update( &resource->obj, desc->pPrivateDriverData, desc->PrivateDriverDataSize );
        unpin_d3dkmt_object( &resource->obj );
        return status;
    }

    case D3DKMT_ESCAPE_SET_PRESENT_RECT_WINE:
    {
        HWND hwnd = UlongToHandle( desc->hContext );
        RECT *rect = desc->pPrivateDriverData;
        UINT dpi = get_dpi_for_window( hwnd );
        WND *win;

        if (desc->PrivateDriverDataSize != sizeof(*rect)) return STATUS_INVALID_PARAMETER;

        TRACE( "hwnd %p, rect %s\n", hwnd, wine_dbgstr_rect( rect ) );
        if (!(win = get_win_ptr( hwnd ))) return STATUS_INVALID_PARAMETER;
        win->present_rect = map_dpi_rect( *rect, get_thread_dpi(), dpi );
        release_win_ptr( win );

        return STATUS_SUCCESS;
    }

    default:
        FIXME( "(%p): stub\n", desc );
        return STATUS_NO_MEMORY;
    }
}

/******************************************************************************
 *           NtGdiDdDDICloseAdapter    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICloseAdapter( const D3DKMT_CLOSEADAPTER *desc )
{
    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hAdapter) return STATUS_INVALID_PARAMETER;
    return release_d3dkmt_object_handle( desc->hAdapter, D3DKMT_ADAPTER );
}

struct d3dkmt_open_adapter_luid_desc32
{
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t adapter;
};

struct d3dkmt_create_device_desc32
{
    uint32_t adapter;
    uint32_t flags;
    uint32_t device;
    uint32_t command_buffer;
    uint32_t command_buffer_size;
    uint32_t allocation_list;
    uint32_t allocation_list_size;
    uint32_t patch_location_list;
    uint32_t patch_location_list_size;
};

C_ASSERT( sizeof(struct d3dkmt_open_adapter_luid_desc32) == 12 );
C_ASSERT( offsetof(struct d3dkmt_open_adapter_luid_desc32, adapter) == 8 );
C_ASSERT( sizeof(struct d3dkmt_create_device_desc32) == 36 );
C_ASSERT( offsetof(struct d3dkmt_create_device_desc32, device) == 8 );

struct d3dkmt_lifecycle_wow64
{
    struct wine_d3dkmt_lifecycle32 *packet;
    struct ntdll_wow64_user_write_range output_range;
    D3DKMT_HANDLE output;
};

static NTSTATUS d3dkmt_lifecycle_fault( struct d3dkmt_lifecycle_wow64 *wow64,
                                         NTSTATUS status )
{
    wow64->packet->fault_status = status;
    return status;
}

static NTSTATUS d3dkmt_lifecycle_prepare_output(
    struct d3dkmt_lifecycle_wow64 *wow64, SIZE_T offset )
{
    uint64_t address = (uint64_t)wow64->packet->guest_desc + offset;
    NTSTATUS status;
    void *output;

    if (address + sizeof(wow64->output) > 0x100000000ull)
        return d3dkmt_lifecycle_fault( wow64, STATUS_ACCESS_VIOLATION );
    if ((status = ntdll_wow64_guest32_to_host( address, &output )))
        return d3dkmt_lifecycle_fault( wow64, status );

    wow64->output_range.dst = output;
    wow64->output_range.src = &wow64->output;
    wow64->output_range.size = sizeof(wow64->output);
    if ((status = ntdll_wow64_probe_user_writev( &wow64->output_range, 1 )))
        return d3dkmt_lifecycle_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_lifecycle_publish(
    struct d3dkmt_lifecycle_wow64 *wow64, D3DKMT_HANDLE handle )
{
    NTSTATUS status;

    wow64->output = handle;
    if ((status = ntdll_wow64_atomic_writev( &wow64->output_range, 1 )))
        return d3dkmt_lifecycle_fault( wow64, status );
    wow64->packet->output_handle = handle;
    return STATUS_SUCCESS;
}

static struct vulkan_physical_device *get_vulkan_physical_device( struct vulkan_instance *instance, const LUID *luid )
{
    GUID uuid;

    if (!get_gpu_uuid_from_luid( luid, &uuid ))
    {
        WARN( "Failed to find Vulkan device with LUID %08x:%08x.\n", luid->HighPart, luid->LowPart );
        return NULL;
    }

    for (UINT i = 0; i < instance->physical_device_count; ++i)
    {
        VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
        struct vulkan_physical_device *physical_device = instance->physical_devices + i;

        instance->p_vkGetPhysicalDeviceProperties2KHR( physical_device->host.physical_device, &properties2 );
        if (IsEqualGUID( &uuid, id.deviceUUID )) return physical_device;
    }

    return NULL;
}

/******************************************************************************
 *           NtGdiDdDDIOpenAdapterFromLuid    (win32u.@)
 */
static NTSTATUS d3dkmt_open_adapter_from_luid(
    D3DKMT_OPENADAPTERFROMLUID *desc, struct d3dkmt_lifecycle_wow64 *wow64 )
{
    struct vulkan_instance *instance;
    struct d3dkmt_adapter *adapter;
    NTSTATUS status;

    if (wow64 && (status = d3dkmt_lifecycle_prepare_output(
        wow64, offsetof(struct d3dkmt_open_adapter_luid_desc32, adapter) )))
        return status;
    if ((status = d3dkmt_object_alloc( sizeof(*adapter), D3DKMT_ADAPTER, (void **)&adapter ))) return status;
    if ((status = alloc_object_handle( &adapter->obj ))) goto failed;

    if (!(instance = get_d3dkmt_vulkan_instance())) WARN( "Vulkan is unavailable.\n" );
    else adapter->physical_device = get_vulkan_physical_device( instance, &desc->AdapterLuid );
    if (!adapter->physical_device) WARN( "Failed to find Vulkan physical device\n" );

    if (wow64)
    {
        if ((status = d3dkmt_lifecycle_publish( wow64, adapter->obj.local )))
            goto failed;
    }
    else desc->hAdapter = adapter->obj.local;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( &adapter->obj );
    return status;
}

NTSTATUS WINAPI NtGdiDdDDIOpenAdapterFromLuid( D3DKMT_OPENADAPTERFROMLUID *desc )
{
    return d3dkmt_open_adapter_from_luid( desc, NULL );
}

/******************************************************************************
 *           NtGdiDdDDICreateDevice    (win32u.@)
 */
static NTSTATUS d3dkmt_create_device( D3DKMT_CREATEDEVICE *desc,
                                      struct d3dkmt_lifecycle_wow64 *wow64 )
{
    struct d3dkmt_adapter *adapter;
    struct d3dkmt_device *device = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    TRACE( "(%p)\n", desc );

    if (!desc) return STATUS_INVALID_PARAMETER;
    if (desc->Flags.LegacyMode || desc->Flags.RequestVSync || desc->Flags.DisableGpuTimeout) FIXME( "Flags unsupported.\n" );

    if (wow64)
    {
        if (!(adapter = pin_d3dkmt_object( desc->hAdapter, D3DKMT_ADAPTER )))
            return STATUS_INVALID_PARAMETER;
    }
    else if (!validate_d3dkmt_object_handle( desc->hAdapter, D3DKMT_ADAPTER ))
        return STATUS_INVALID_PARAMETER;

    if (wow64 && (status = d3dkmt_lifecycle_prepare_output(
        wow64, offsetof(struct d3dkmt_create_device_desc32, device) )))
        goto done;
    if ((status = d3dkmt_object_alloc( sizeof(*device), D3DKMT_DEVICE,
                                       (void **)&device )))
        goto done;
    if ((status = alloc_object_handle( &device->obj ))) goto done;

    if (wow64)
    {
        if ((status = d3dkmt_lifecycle_publish( wow64, device->obj.local )))
            goto done;
    }
    else desc->hDevice = device->obj.local;
    device = NULL;

done:
    if (device) d3dkmt_object_free( &device->obj );
    if (wow64) unpin_d3dkmt_object( &adapter->obj );
    return status;
}

NTSTATUS WINAPI NtGdiDdDDICreateDevice( D3DKMT_CREATEDEVICE *desc )
{
    return d3dkmt_create_device( desc, NULL );
}

/******************************************************************************
 *           NtGdiDdDDIDestroyDevice    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIDestroyDevice( const D3DKMT_DESTROYDEVICE *desc )
{
    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hDevice) return STATUS_INVALID_PARAMETER;
    return release_d3dkmt_object_handle( desc->hDevice, D3DKMT_DEVICE );
}

NTSTATUS WINAPI __wine_win32u_d3dkmt_lifecycle(
    struct wine_d3dkmt_lifecycle32 *packet )
{
    struct d3dkmt_lifecycle_wow64 wow64 = { .packet = packet };

    if (!packet || packet->version != WINE_D3DKMT_LIFECYCLE32_VERSION ||
        packet->size != sizeof(*packet) || !packet->guest_desc ||
        packet->output_handle || packet->fault_status ||
        packet->reserved[0] || packet->reserved[1])
        return STATUS_INVALID_PARAMETER;
    packet->fault_status = 0;

    switch (packet->variant)
    {
    case WINE_D3DKMT_LIFECYCLE32_OPEN_ADAPTER_FROM_LUID:
    {
        D3DKMT_OPENADAPTERFROMLUID desc = {0};

        desc.AdapterLuid.LowPart = packet->luid_low;
        desc.AdapterLuid.HighPart = packet->luid_high;
        return d3dkmt_open_adapter_from_luid( &desc, &wow64 );
    }
    case WINE_D3DKMT_LIFECYCLE32_CLOSE_ADAPTER:
    {
        D3DKMT_CLOSEADAPTER desc = { .hAdapter = packet->input_handle };

        return NtGdiDdDDICloseAdapter( &desc );
    }
    case WINE_D3DKMT_LIFECYCLE32_CREATE_DEVICE:
    {
        D3DKMT_CREATEDEVICE desc = { .hAdapter = packet->input_handle };

        memcpy( &desc.Flags, &packet->flags, sizeof(desc.Flags) );
        return d3dkmt_create_device( &desc, &wow64 );
    }
    case WINE_D3DKMT_LIFECYCLE32_DESTROY_DEVICE:
    {
        D3DKMT_DESTROYDEVICE desc = { .hDevice = packet->input_handle };

        return NtGdiDdDDIDestroyDevice( &desc );
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/******************************************************************************
 *           NtGdiDdDDIQueryAdapterInfo    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryAdapterInfo( D3DKMT_QUERYADAPTERINFO *desc )
{
    struct d3dkmt_adapter *adapter;

    TRACE( "(%p).\n", desc );

    if (!desc || !desc->hAdapter || !desc->pPrivateDriverData)
        return STATUS_INVALID_PARAMETER;

    switch (desc->Type)
    {
    case KMTQAITYPE_CHECKDRIVERUPDATESTATUS:
    {
        BOOL *value = desc->pPrivateDriverData;

        if (desc->PrivateDriverDataSize < sizeof(*value))
            return STATUS_INVALID_PARAMETER;

        *value = FALSE;
        return STATUS_SUCCESS;
    }
    case KMTQAITYPE_DRIVERVERSION:
    {
        D3DKMT_DRIVERVERSION *value = desc->pPrivateDriverData;

        if (desc->PrivateDriverDataSize < sizeof(*value))
            return STATUS_INVALID_PARAMETER;

        *value = KMT_DRIVERVERSION_WDDM_3_1;
        return STATUS_SUCCESS;
    }
    case KMTQAITYPE_WDDM_2_7_CAPS:
    {
        VkPhysicalDeviceDriverPropertiesKHR driverProperties;
        struct vulkan_physical_device *physical_device;
        VkPhysicalDeviceProperties2KHR properties2;
        struct vulkan_instance *instance;
        D3DKMT_WDDM_2_7_CAPS *data;
        const char *e;

        if (desc->PrivateDriverDataSize < sizeof(*data))
            return STATUS_INVALID_PARAMETER;
        if (!(adapter = pin_d3dkmt_object( desc->hAdapter, D3DKMT_ADAPTER ))) return STATUS_INVALID_PARAMETER;
        if (!(physical_device = adapter->physical_device))
        {
            unpin_d3dkmt_object( &adapter->obj );
            return STATUS_INVALID_PARAMETER;
        }
        instance = physical_device->instance;

        memset( &driverProperties, 0, sizeof(driverProperties) );
        memset( &properties2, 0, sizeof(properties2) );
        driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR;
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
        properties2.pNext = &driverProperties;
        instance->p_vkGetPhysicalDeviceProperties2KHR( physical_device->host.physical_device, &properties2 );

        /*
         * Advertise Hardware-Scheduling as enabled for NVIDIA Adapters. NVIDIA driver does
         * userspace submission. Allow overriding this value via the
         * WINE_DISABLE_HARDWARE_SCHEDULING environment variable.
         */
        data = desc->pPrivateDriverData;
        memset( data, 0, sizeof(*data) );
        e = getenv( "WINE_DISABLE_HARDWARE_SCHEDULING" );
        if ((!e || *e == '\0' || *e == '0') && (driverProperties.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY))
        {
            data->HwSchEnabled = 1;
            data->HwSchSupported = 1;
            data->HwSchEnabledByDefault = 1;
        }

        unpin_d3dkmt_object( &adapter->obj );
        return STATUS_SUCCESS;
    }
    default:
    {
        FIXME( "type %d not handled.\n", desc->Type );
        return STATUS_NOT_IMPLEMENTED;
    }
    }
}

/******************************************************************************
 *           NtGdiDdDDIQueryStatistics    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryStatistics( D3DKMT_QUERYSTATISTICS *stats )
{
    FIXME( "(%p): stub\n", stats );
    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDIQueryVideoMemoryInfo    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryVideoMemoryInfo( D3DKMT_QUERYVIDEOMEMORYINFO *desc )
{
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
    struct vulkan_physical_device *physical_device;
    VkPhysicalDeviceMemoryProperties2 properties2;
    struct d3dkmt_adapter *adapter;
    OBJECT_BASIC_INFORMATION info;
    NTSTATUS status;
    unsigned int i;

    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hAdapter ||
        (desc->MemorySegmentGroup != D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL &&
         desc->MemorySegmentGroup != D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL))
        return STATUS_INVALID_PARAMETER;

    /* FIXME: Wine currently doesn't support linked adapters */
    if (desc->PhysicalAdapterIndex > 0) return STATUS_INVALID_PARAMETER;

    status = NtQueryObject( desc->hProcess ? desc->hProcess : GetCurrentProcess(),
                            ObjectBasicInformation, &info, sizeof(info), NULL );
    if (status != STATUS_SUCCESS) return status;
    if (!(info.GrantedAccess & PROCESS_QUERY_INFORMATION)) return STATUS_ACCESS_DENIED;

    if (!(adapter = pin_d3dkmt_object( desc->hAdapter, D3DKMT_ADAPTER ))) return STATUS_INVALID_PARAMETER;

    desc->Budget = 0;
    desc->CurrentUsage = 0;
    desc->CurrentReservation = 0;
    desc->AvailableForReservation = 0;

    if ((physical_device = adapter->physical_device))
    {
        struct vulkan_instance *instance = physical_device->instance;

        memset( &budget, 0, sizeof(budget) );
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        properties2.pNext = &budget;

        instance->p_vkGetPhysicalDeviceMemoryProperties2KHR( physical_device->host.physical_device, &properties2 );
        for (i = 0; i < properties2.memoryProperties.memoryHeapCount; ++i)
        {
            if ((desc->MemorySegmentGroup == D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL &&
                 properties2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
                (desc->MemorySegmentGroup == D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL &&
                 !(properties2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)))
            {
                desc->Budget += budget.heapBudget[i];
                desc->CurrentUsage += min( budget.heapBudget[i], budget.heapUsage[i] );
            }
        }

        desc->AvailableForReservation = desc->Budget / 2;
    }
    else WARN( "Failed to find Vulkan physical device\n" );
    unpin_d3dkmt_object( &adapter->obj );
    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDISetQueuedLimit    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDISetQueuedLimit( D3DKMT_SETQUEUEDLIMIT *desc )
{
    FIXME( "(%p): stub\n", desc );
    return STATUS_NOT_IMPLEMENTED;
}

/******************************************************************************
 *           NtGdiDdDDISetVidPnSourceOwner    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDISetVidPnSourceOwner( const D3DKMT_SETVIDPNSOURCEOWNER *desc )
{
    struct d3dkmt_vidpn_source *source, *source2;
    BOOL found;
    UINT i;

    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hDevice || (desc->VidPnSourceCount && (!desc->pType || !desc->pVidPnSourceId)))
        return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &d3dkmt_lock );

    /* Keep validation and owner-list mutation atomic with DestroyDevice. */
    if (!validate_d3dkmt_object_handle_locked( desc->hDevice, D3DKMT_DEVICE ))
    {
        pthread_mutex_unlock( &d3dkmt_lock );
        return STATUS_INVALID_PARAMETER;
    }

    /* Check parameters */
    for (i = 0; i < desc->VidPnSourceCount; ++i)
    {
        LIST_FOR_EACH_ENTRY( source, &d3dkmt_vidpn_sources, struct d3dkmt_vidpn_source, entry )
        {
            if (source->id == desc->pVidPnSourceId[i])
            {
                /* Same device */
                if (source->device == desc->hDevice)
                {
                    if ((source->type == D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE &&
                         (desc->pType[i] == D3DKMT_VIDPNSOURCEOWNER_SHARED ||
                          desc->pType[i] == D3DKMT_VIDPNSOURCEOWNER_EMULATED)) ||
                        (source->type == D3DKMT_VIDPNSOURCEOWNER_EMULATED &&
                         desc->pType[i] == D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE))
                    {
                        pthread_mutex_unlock( &d3dkmt_lock );
                        return STATUS_INVALID_PARAMETER;
                    }
                }
                /* Different devices */
                else
                {
                    if ((source->type == D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE || source->type == D3DKMT_VIDPNSOURCEOWNER_EMULATED) &&
                        (desc->pType[i] == D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE ||
                         desc->pType[i] == D3DKMT_VIDPNSOURCEOWNER_EMULATED))
                    {
                        pthread_mutex_unlock( &d3dkmt_lock );
                        return STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE;
                    }
                }
            }
        }

        /* On Windows, it seems that all video present sources are owned by DMM clients, so any attempt to set
         * D3DKMT_VIDPNSOURCEOWNER_SHARED come back STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE */
        if (desc->pType[i] == D3DKMT_VIDPNSOURCEOWNER_SHARED)
        {
            pthread_mutex_unlock( &d3dkmt_lock );
            return STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE;
        }

        /* FIXME: D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVEGDI unsupported */
        if (desc->pType[i] == D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVEGDI || desc->pType[i] > D3DKMT_VIDPNSOURCEOWNER_EMULATED)
        {
            pthread_mutex_unlock( &d3dkmt_lock );
            return STATUS_INVALID_PARAMETER;
        }
    }

    /* Remove owner */
    if (!desc->VidPnSourceCount && !desc->pType && !desc->pVidPnSourceId)
    {
        LIST_FOR_EACH_ENTRY_SAFE( source, source2, &d3dkmt_vidpn_sources, struct d3dkmt_vidpn_source, entry )
        {
            if (source->device == desc->hDevice)
            {
                list_remove( &source->entry );
                free( source );
            }
        }

        pthread_mutex_unlock( &d3dkmt_lock );
        return STATUS_SUCCESS;
    }

    /* Add owner */
    for (i = 0; i < desc->VidPnSourceCount; ++i)
    {
        found = FALSE;
        LIST_FOR_EACH_ENTRY( source, &d3dkmt_vidpn_sources, struct d3dkmt_vidpn_source, entry )
        {
            if (source->device == desc->hDevice && source->id == desc->pVidPnSourceId[i])
            {
                found = TRUE;
                break;
            }
        }

        if (found) source->type = desc->pType[i];
        else
        {
            source = malloc( sizeof(*source) );
            if (!source)
            {
                pthread_mutex_unlock( &d3dkmt_lock );
                return STATUS_NO_MEMORY;
            }

            source->id = desc->pVidPnSourceId[i];
            source->type = desc->pType[i];
            source->device = desc->hDevice;
            list_add_tail( &d3dkmt_vidpn_sources, &source->entry );
        }
    }

    pthread_mutex_unlock( &d3dkmt_lock );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtGdiDdDDICheckOcclusion( const D3DKMT_CHECKOCCLUSION *desc )
{
    FIXME( "desc %p stub!\n", desc );
    return STATUS_PROCEDURE_NOT_FOUND;
}

/******************************************************************************
 *           NtGdiDdDDICheckVidPnExclusiveOwnership    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICheckVidPnExclusiveOwnership( const D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP *desc )
{
    struct d3dkmt_vidpn_source *source;

    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hAdapter) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &d3dkmt_lock );

    LIST_FOR_EACH_ENTRY( source, &d3dkmt_vidpn_sources, struct d3dkmt_vidpn_source, entry )
    {
        if (source->id == desc->VidPnSourceId && source->type == D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE)
        {
            pthread_mutex_unlock( &d3dkmt_lock );
            return STATUS_GRAPHICS_PRESENT_OCCLUDED;
        }
    }

    pthread_mutex_unlock( &d3dkmt_lock );
    return STATUS_SUCCESS;
}

struct vk_physdev_info
{
    VkPhysicalDeviceProperties2 properties2;
    VkPhysicalDeviceIDProperties id;
    VkPhysicalDeviceMemoryProperties mem_properties;
};

static int compare_vulkan_physical_devices( const void *v1, const void *v2 )
{
    static const int device_type_rank[6] = { 100, 1, 0, 2, 3, 200 };
    const struct vk_physdev_info *d1 = v1, *d2 = v2;
    int rank1, rank2;

    rank1 = device_type_rank[ min( d1->properties2.properties.deviceType, ARRAY_SIZE(device_type_rank) - 1) ];
    rank2 = device_type_rank[ min( d2->properties2.properties.deviceType, ARRAY_SIZE(device_type_rank) - 1) ];
    if (rank1 != rank2) return rank1 - rank2;

    return memcmp( &d1->id.deviceUUID, &d2->id.deviceUUID, sizeof(d1->id.deviceUUID) );
}

BOOL get_vulkan_gpus( struct list *gpus )
{
    struct vulkan_instance *instance;
    struct vk_physdev_info *devinfo;
    UINT i, j;

    if (!(instance = get_d3dkmt_vulkan_instance())) return FALSE;
    if (!(devinfo = calloc( instance->physical_device_count, sizeof(*devinfo) ))) return FALSE;

    for (i = 0; i < instance->physical_device_count; ++i)
    {
        struct vulkan_physical_device *physical_device = instance->physical_devices + i;

        devinfo[i].id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        devinfo[i].properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        devinfo[i].properties2.pNext = &devinfo[i].id;

        instance->p_vkGetPhysicalDeviceProperties2KHR( physical_device->host.physical_device, &devinfo[i].properties2 );
        instance->p_vkGetPhysicalDeviceMemoryProperties( physical_device->host.physical_device, &devinfo[i].mem_properties );
    }
    qsort( devinfo, instance->physical_device_count, sizeof(*devinfo), compare_vulkan_physical_devices );

    for (i = 0; i < instance->physical_device_count; ++i)
    {
        struct gpu_info *gpu;

        if (!(gpu = calloc( 1, sizeof(*gpu) ))) break;
        memcpy( &gpu->uuid, devinfo[i].id.deviceUUID, sizeof(gpu->uuid) );
        gpu->name = strdup( devinfo[i].properties2.properties.deviceName );
        gpu->pci_id.vendor = devinfo[i].properties2.properties.vendorID;
        gpu->pci_id.device = devinfo[i].properties2.properties.deviceID;

        for (j = 0; j < devinfo[i].mem_properties.memoryHeapCount; j++)
        {
            if (devinfo[i].mem_properties.memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                gpu->memory += devinfo[i].mem_properties.memoryHeaps[j].size;
        }

        list_add_tail( gpus, &gpu->entry );
    }

    free( devinfo );
    return TRUE;
}

/******************************************************************************
 *           NtGdiDdDDIShareObjects    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIShareObjects( UINT count, const D3DKMT_HANDLE *handles, OBJECT_ATTRIBUTES *attr,
                                        UINT access, HANDLE *handle )
{
    struct d3dkmt_object *object, *resource = NULL, *sync = NULL, *mutex = NULL;
    struct d3dkmt_object *pinned[3] = {0};
    unsigned int pinned_count = 0;
    struct object_attributes *objattr;
    data_size_t len;
    NTSTATUS status;

    TRACE( "count %u, handles %p, attr %p, access %#x, handle %p\n", count, handles, attr, access, handle );

    if (count == 1)
    {
        if (!(object = pin_d3dkmt_object( handles[0], -1 ))) goto failed;
        pinned[pinned_count++] = object;
        if (!object->shared) goto failed;
        if (object->type == D3DKMT_RESOURCE) resource = object;
        else if (object->type == D3DKMT_SYNC) sync = object;
        else goto failed;
    }
    else if (count == 3)
    {
        if (!(object = pin_d3dkmt_object( handles[0], -1 ))) goto failed;
        pinned[pinned_count++] = object;
        if (!object->shared) goto failed;
        if (object->type != D3DKMT_RESOURCE) goto failed;
        resource = object;

        if (!(object = pin_d3dkmt_object( handles[1], -1 ))) goto failed;
        pinned[pinned_count++] = object;
        if (!object->shared) goto failed;
        if (object->type != D3DKMT_MUTEX) goto failed;
        mutex = object;

        if (!(object = pin_d3dkmt_object( handles[2], -1 ))) goto failed;
        pinned[pinned_count++] = object;
        if (!object->shared) goto failed;
        if (object->type != D3DKMT_SYNC) goto failed;
        sync = object;
    }
    else goto failed;

    if ((status = alloc_object_attributes( attr, &objattr, &len ))) goto done;

    SERVER_START_REQ( d3dkmt_share_objects )
    {
        req->access = access | STANDARD_RIGHTS_ALL;
        if (resource) req->resource = resource->global;
        if (mutex) req->mutex = mutex->global;
        if (sync) req->sync = sync->global;
        wine_server_add_data( req, objattr, len );
        status = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );

    if (status) WARN( "Failed to share objects, status %#x\n", status );
    else TRACE( "Shared objects with handle %p\n", *handle );
    goto done;

failed:
    WARN( "Unsupported object count / types / handles\n" );
    status = STATUS_INVALID_PARAMETER;

done:
    while (pinned_count) unpin_d3dkmt_object( pinned[--pinned_count] );
    return status;
}

struct d3dkmt_create_standard_allocation32
{
    UINT type;
    UINT size;
    UINT flags;
};

struct d3dddi_allocation_info32
{
    D3DKMT_HANDLE allocation;
    ULONG system_memory;
    ULONG private_driver_data;
    UINT private_driver_data_size;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID source_id;
    UINT flags;
};

struct d3dddi_allocation_info2_32
{
    D3DKMT_HANDLE allocation;
    union
    {
        ULONG section;
        ULONG system_memory;
    };
    ULONG private_driver_data;
    UINT private_driver_data_size;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID source_id;
    UINT flags;
    D3DGPU_VIRTUAL_ADDRESS gpu_address;
    ULONG priority;
    ULONG reserved[5];
};

struct d3dkmt_create_allocation_desc32
{
    D3DKMT_HANDLE device;
    D3DKMT_HANDLE resource;
    D3DKMT_HANDLE global_share;
    ULONG private_runtime_data;
    UINT private_runtime_data_size;
    ULONG standard_or_private;
    UINT private_driver_data_size;
    UINT allocation_count;
    ULONG allocation_info;
    UINT flags;
    ULONG private_runtime_resource_handle;
};

C_ASSERT( sizeof(struct d3dkmt_create_standard_allocation32) == 12 );
C_ASSERT( sizeof(struct d3dddi_allocation_info32) == 24 );
C_ASSERT( sizeof(struct d3dddi_allocation_info2_32) == 56 );
C_ASSERT( sizeof(struct d3dkmt_create_allocation_desc32) == 44 );
C_ASSERT( offsetof(struct d3dkmt_create_allocation_desc32, global_share) ==
          offsetof(struct d3dkmt_create_allocation_desc32, resource) + sizeof(D3DKMT_HANDLE) );
C_ASSERT( offsetof(struct d3dddi_allocation_info32, allocation) == 0 );
C_ASSERT( offsetof(struct d3dddi_allocation_info2_32, allocation) == 0 );
C_ASSERT( offsetof(struct d3dddi_allocation_info2_32, gpu_address) == 24 );

struct d3dkmt_create_allocation_wow64
{
    struct wine_d3dkmt_create_allocation32 *packet;
    struct d3dkmt_create_allocation_desc32 desc32;
    struct d3dkmt_create_standard_allocation32 standard32;
    D3DKMT_CREATESTANDARDALLOCATION standard;
    union
    {
        struct d3dddi_allocation_info32 info;
        struct d3dddi_allocation_info2_32 info2;
    } allocation32;
    union
    {
        D3DDDI_ALLOCATIONINFO info;
        D3DDDI_ALLOCATIONINFO2 info2;
    } allocation;
    void *desc_user;
    void *allocation_user;
    struct ntdll_wow64_user_write_range outputs[3];
    ULONG output_count;
};

static NTSTATUS d3dkmt_create_allocation_fault(
    struct d3dkmt_create_allocation_wow64 *wow64, NTSTATUS status )
{
    wow64->packet->fault_status = status;
    return status;
}

static NTSTATUS d3dkmt_create_allocation_guest_ptr( ULONG address, void **host )
{
    NTSTATUS status = ntdll_wow64_guest32_to_host( address, host );

    if (status) *host = NULL;
    return status;
}

static NTSTATUS d3dkmt_create_allocation_capture_standard(
    D3DKMT_CREATEALLOCATION *params, struct d3dkmt_create_allocation_wow64 *wow64 )
{
    void *standard_user;
    NTSTATUS status;

    if ((status = d3dkmt_create_allocation_guest_ptr(
                     wow64->packet->guest_standard_or_private, &standard_user )))
        return status;
    if ((status = ntdll_wow64_copy_from_user( &wow64->standard32, standard_user,
                                               sizeof(wow64->standard32) )))
        return d3dkmt_create_allocation_fault( wow64, status );
    wow64->standard.Type = wow64->standard32.type;
    wow64->standard.ExistingHeapData.Size = wow64->standard32.size;
    wow64->standard.Flags.Value = wow64->standard32.flags;
    params->pStandardAllocation = &wow64->standard;
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_create_allocation_capture_allocation(
    D3DKMT_CREATEALLOCATION *params, BOOL version2,
    struct d3dkmt_create_allocation_wow64 *wow64 )
{
    void *system_memory, *private_driver_data, *runtime;
    NTSTATUS status;

    if ((status = d3dkmt_create_allocation_guest_ptr(
                     wow64->packet->guest_allocation, &wow64->allocation_user )))
        return status;
    if ((status = d3dkmt_create_allocation_guest_ptr(
                     wow64->packet->guest_runtime, &runtime )))
        return status;

    if (version2)
    {
        struct d3dddi_allocation_info2_32 *in = &wow64->allocation32.info2;
        D3DDDI_ALLOCATIONINFO2 *out = &wow64->allocation.info2;

        if ((status = ntdll_wow64_copy_from_user( in, wow64->allocation_user, sizeof(*in) )))
            return d3dkmt_create_allocation_fault( wow64, status );
        if (params->Flags.ExistingSection)
            out->hSection = UlongToHandle( in->section );
        else
        {
            if ((status = d3dkmt_create_allocation_guest_ptr( in->system_memory,
                                                               &system_memory )))
                return status;
            out->pSystemMem = system_memory;
        }
        if ((status = d3dkmt_create_allocation_guest_ptr( in->private_driver_data,
                                                           &private_driver_data )))
            return status;
        out->hAllocation = in->allocation;
        out->pPrivateDriverData = private_driver_data;
        out->PrivateDriverDataSize = in->private_driver_data_size;
        out->VidPnSourceId = in->source_id;
        out->Flags.Value = in->flags;
        out->GpuVirtualAddress = in->gpu_address;
        out->Priority = in->priority;
        params->pAllocationInfo2 = out;
    }
    else
    {
        struct d3dddi_allocation_info32 *in = &wow64->allocation32.info;
        D3DDDI_ALLOCATIONINFO *out = &wow64->allocation.info;

        if ((status = ntdll_wow64_copy_from_user( in, wow64->allocation_user, sizeof(*in) )))
            return d3dkmt_create_allocation_fault( wow64, status );
        if ((status = d3dkmt_create_allocation_guest_ptr( in->system_memory,
                                                           &system_memory )))
            return status;
        if ((status = d3dkmt_create_allocation_guest_ptr( in->private_driver_data,
                                                           &private_driver_data )))
            return status;
        out->hAllocation = in->allocation;
        out->pSystemMem = system_memory;
        out->pPrivateDriverData = private_driver_data;
        out->PrivateDriverDataSize = in->private_driver_data_size;
        out->VidPnSourceId = in->source_id;
        out->Flags.Value = in->flags;
        params->pAllocationInfo = out;
    }
    params->pPrivateRuntimeData = runtime;
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_create_allocation_prepare_outputs(
    BOOL version2, struct d3dkmt_create_allocation_wow64 *wow64 )
{
    NTSTATUS status;

    if ((status = d3dkmt_create_allocation_guest_ptr( wow64->packet->guest_desc,
                                                       &wow64->desc_user )))
        return status;
    wow64->outputs[0].dst = (char *)wow64->desc_user +
                            offsetof(struct d3dkmt_create_allocation_desc32, resource);
    wow64->outputs[0].src = &wow64->desc32.resource;
    wow64->outputs[0].size = 2 * sizeof(D3DKMT_HANDLE);
    wow64->outputs[1].dst = wow64->allocation_user;
    wow64->outputs[1].src = &wow64->allocation32.info.allocation;
    wow64->outputs[1].size = sizeof(D3DKMT_HANDLE);
    wow64->output_count = 2;
    if (version2)
    {
        wow64->outputs[2].dst = (char *)wow64->allocation_user +
                                offsetof(struct d3dddi_allocation_info2_32, gpu_address);
        wow64->outputs[2].src = &wow64->allocation32.info2.gpu_address;
        wow64->outputs[2].size = sizeof(D3DGPU_VIRTUAL_ADDRESS);
        wow64->output_count++;
    }
    if ((status = ntdll_wow64_probe_user_writev( wow64->outputs, wow64->output_count )))
        return d3dkmt_create_allocation_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_create_allocation_publish(
    D3DKMT_CREATEALLOCATION *params, BOOL version2,
    struct d3dkmt_create_allocation_wow64 *wow64 )
{
    NTSTATUS status;

    wow64->desc32.resource = params->hResource;
    wow64->desc32.global_share = params->hGlobalShare;
    if (version2)
    {
        wow64->allocation32.info2.allocation = params->pAllocationInfo2->hAllocation;
        wow64->allocation32.info2.gpu_address = params->pAllocationInfo2->GpuVirtualAddress;
    }
    else
        wow64->allocation32.info.allocation = params->pAllocationInfo->hAllocation;

    /* Preserve the original thunk's scalar publication order.  Atomic writev
     * defines later overlapping ranges to win. */
    if ((status = ntdll_wow64_atomic_writev( wow64->outputs, wow64->output_count )))
        return d3dkmt_create_allocation_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_create_allocation( D3DKMT_CREATEALLOCATION *params, BOOL version2,
                                          struct d3dkmt_create_allocation_wow64 *wow64 )
{
    D3DKMT_CREATESTANDARDALLOCATION *standard;
    struct d3dkmt_resource *resource = NULL, *existing_resource = NULL;
    D3DDDI_ALLOCATIONINFO *alloc_info;
    struct d3dkmt_object *allocation = NULL;
    struct d3dkmt_device *device;
    NTSTATUS status = STATUS_SUCCESS;

    FIXME( "params %p semi-stub!\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    device = pin_d3dkmt_object( params->hDevice, D3DKMT_DEVICE );
    if (!device) return STATUS_INVALID_PARAMETER;

    if (!params->Flags.StandardAllocation || params->PrivateDriverDataSize)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    if (params->NumAllocations != 1 ||
        (wow64 ? (!wow64->packet->guest_allocation ||
                  !wow64->packet->guest_standard_or_private) :
                 (!params->pAllocationInfo || !params->pStandardAllocation)))
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    if (wow64 &&
        (status = d3dkmt_create_allocation_capture_standard( params, wow64 )))
        goto done;

    standard = params->pStandardAllocation;
    if (standard->Type != D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP ||
        (standard->ExistingHeapData.Size & 0xfff) || !params->Flags.ExistingSysMem)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    if (wow64 &&
        (status = d3dkmt_create_allocation_capture_allocation( params, version2, wow64 )))
        goto done;
    alloc_info = params->pAllocationInfo;
    if (!alloc_info->pSystemMem)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    if (params->Flags.CreateResource)
    {
        if (params->hResource)
        {
            existing_resource = pin_d3dkmt_object( params->hResource, D3DKMT_RESOURCE );
            if (!existing_resource)
            {
                status = STATUS_INVALID_HANDLE;
                goto done;
            }
        }
        if (wow64 && params->Flags.CreateShared && params->PrivateRuntimeDataSize &&
            (status = ntdll_wow64_probe_user_read( params->pPrivateRuntimeData,
                                                    params->PrivateRuntimeDataSize )))
        {
            status = d3dkmt_create_allocation_fault( wow64, status );
            goto done;
        }
        if (wow64 &&
            (status = d3dkmt_create_allocation_prepare_outputs( version2, wow64 )))
            goto done;
        if ((status = d3dkmt_object_alloc( sizeof(*resource), D3DKMT_RESOURCE,
                                           (void **)&resource )))
            goto done;
        if ((status = d3dkmt_object_alloc( sizeof(*allocation), D3DKMT_ALLOCATION, (void **)&allocation ))) goto failed;

        if (!params->Flags.CreateShared) status = alloc_object_handle( &resource->obj );
        else status = d3dkmt_object_create( &resource->obj, -1, 0, params->Flags.NtSecuritySharing,
                                            params->pPrivateRuntimeData, params->PrivateRuntimeDataSize );
        if (status) goto failed;

        params->hGlobalShare = resource->obj.shared ? 0 : resource->obj.global;
        params->hResource = resource->obj.local;
    }
    else
    {
        if (params->Flags.CreateShared)
        {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        if (params->hResource)
        {
            existing_resource = pin_d3dkmt_object( params->hResource, D3DKMT_RESOURCE );
            status = existing_resource ? STATUS_INVALID_PARAMETER : STATUS_INVALID_HANDLE;
            goto done;
        }
        if (wow64 &&
            (status = d3dkmt_create_allocation_prepare_outputs( version2, wow64 )))
            goto done;
        if ((status = d3dkmt_object_alloc( sizeof(*allocation), D3DKMT_ALLOCATION,
                                           (void **)&allocation )))
            goto done;
        params->hGlobalShare = 0;
    }

    if ((status = alloc_object_handle( allocation ))) goto failed;
    if (resource) resource->allocation = allocation->local;
    alloc_info->hAllocation = allocation->local;
    if (wow64 && (status = d3dkmt_create_allocation_publish( params, version2, wow64 )))
        goto failed;
    allocation = NULL;
    resource = NULL;
    goto done;

failed:
    if (allocation) d3dkmt_object_free( allocation );
    if (resource) d3dkmt_object_free( &resource->obj );
done:
    if (wow64 && status == STATUS_ACCESS_VIOLATION && !wow64->packet->fault_status)
        wow64->packet->fault_status = status;
    if (existing_resource) unpin_d3dkmt_object( &existing_resource->obj );
    unpin_d3dkmt_object( &device->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDICreateAllocation2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICreateAllocation2( D3DKMT_CREATEALLOCATION *params )
{
    return d3dkmt_create_allocation( params, TRUE, NULL );
}

/******************************************************************************
 *           NtGdiDdDDICreateAllocation    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICreateAllocation( D3DKMT_CREATEALLOCATION *params )
{
    return d3dkmt_create_allocation( params, FALSE, NULL );
}

NTSTATUS WINAPI __wine_win32u_d3dkmt_create_allocation(
    struct wine_d3dkmt_create_allocation32 *packet )
{
    struct d3dkmt_create_allocation_wow64 wow64 = { .packet = packet };
    D3DKMT_CREATEALLOCATION params = {0};
    BOOL version2;

    if (!packet || packet->version != WINE_D3DKMT_CREATE_ALLOCATION32_VERSION ||
        packet->size != sizeof(*packet) || packet->fault_status ||
        packet->reserved[0] || packet->reserved[1])
        return STATUS_INVALID_PARAMETER;
    packet->fault_status = 0;
    if (packet->variant == WINE_D3DKMT_CREATE_ALLOCATION32_V1) version2 = FALSE;
    else if (packet->variant == WINE_D3DKMT_CREATE_ALLOCATION32_V2) version2 = TRUE;
    else return STATUS_INVALID_PARAMETER;
    if (!packet->guest_desc) return STATUS_INVALID_PARAMETER;

    wow64.desc32.device = params.hDevice = packet->hDevice;
    wow64.desc32.resource = params.hResource = packet->hResource;
    wow64.desc32.global_share = params.hGlobalShare = packet->hGlobalShare;
    wow64.desc32.private_runtime_data = packet->guest_runtime;
    wow64.desc32.private_runtime_data_size = params.PrivateRuntimeDataSize =
        packet->private_runtime_data_size;
    wow64.desc32.standard_or_private = packet->guest_standard_or_private;
    wow64.desc32.private_driver_data_size = params.PrivateDriverDataSize =
        packet->private_driver_data_size;
    wow64.desc32.allocation_count = params.NumAllocations = packet->num_allocations;
    wow64.desc32.allocation_info = packet->guest_allocation;
    wow64.desc32.flags = packet->flags;
    memcpy( &params.Flags, &packet->flags, sizeof(params.Flags) );
    wow64.desc32.private_runtime_resource_handle = packet->private_runtime_resource_handle;
    params.hPrivateRuntimeResourceHandle = UlongToHandle(
        packet->private_runtime_resource_handle );

    return d3dkmt_create_allocation( &params, version2, &wow64 );
}

/******************************************************************************
 *           NtGdiDdDDIDestroyAllocation2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIDestroyAllocation2( const D3DKMT_DESTROYALLOCATION2 *params )
{
    D3DKMT_HANDLE alloc_handle = 0;
    NTSTATUS status;
    UINT i;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!validate_d3dkmt_object_handle( params->hDevice, D3DKMT_DEVICE )) return STATUS_INVALID_PARAMETER;

    if (params->AllocationCount && !params->phAllocationList) return STATUS_INVALID_PARAMETER;

    if (params->hResource)
    {
        struct d3dkmt_resource *resource;

        if (!(resource = pin_d3dkmt_object( params->hResource, D3DKMT_RESOURCE )))
            return STATUS_INVALID_PARAMETER;
        alloc_handle = resource->allocation;
        status = release_d3dkmt_object_handle( params->hResource, D3DKMT_RESOURCE );
        unpin_d3dkmt_object( &resource->obj );
        if (status) return status;
    }

    for (i = 0; i < params->AllocationCount; i++)
    {
        if ((status = release_d3dkmt_object_handle( params->phAllocationList[i],
                                                     D3DKMT_ALLOCATION )))
            return status;
    }

    if (alloc_handle) release_d3dkmt_object_handle( alloc_handle, D3DKMT_ALLOCATION );

    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDIDestroyAllocation    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIDestroyAllocation( const D3DKMT_DESTROYALLOCATION *params )
{
    D3DKMT_DESTROYALLOCATION2 params2 = {0};

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;

    params2.hDevice = params->hDevice;
    params2.hResource = params->hResource;
    params2.phAllocationList = params->phAllocationList;
    params2.AllocationCount = params->AllocationCount;
    return NtGdiDdDDIDestroyAllocation2( &params2 );
}

struct d3dkmt_open_resource_wow64
{
    struct wine_d3dkmt_open_resource32 *packet;
    union
    {
        struct wine_d3dddi_openallocationinfo32 info;
        struct wine_d3dddi_openallocationinfo2_32 info2;
    } allocation32;
    union
    {
        D3DDDI_OPENALLOCATIONINFO info;
        D3DDDI_OPENALLOCATIONINFO2 info2;
    } allocation;
    void *desc_user;
    void *allocation_user;
    struct ntdll_wow64_user_write_range outputs[8];
    ULONG output_count;
};

static NTSTATUS d3dkmt_open_resource_fault( struct d3dkmt_open_resource_wow64 *wow64,
                                             NTSTATUS status )
{
    wow64->packet->fault_status = status;
    return status;
}

static NTSTATUS d3dkmt_open_resource_guest_ptr( ULONG address, void **host )
{
    NTSTATUS status = ntdll_wow64_guest32_to_host( address, host );

    if (status) *host = NULL;
    return status;
}

static NTSTATUS d3dkmt_open_resource_array_size( UINT count, SIZE_T element_size,
                                                 SIZE_T *size )
{
    if (element_size && count > ~(SIZE_T)0 / element_size) return STATUS_INVALID_PARAMETER;
    *size = (SIZE_T)count * element_size;
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_capture_allocation(
    D3DKMT_OPENRESOURCE *params, BOOL version2,
    struct d3dkmt_open_resource_wow64 *wow64 )
{
    void *private_driver_data;
    SIZE_T native_size, wire_size;
    NTSTATUS status;

    if (version2)
    {
        struct wine_d3dddi_openallocationinfo2_32 *in = &wow64->allocation32.info2;
        D3DDDI_OPENALLOCATIONINFO2 *out = &wow64->allocation.info2;

        if ((status = d3dkmt_open_resource_array_size( params->NumAllocations,
                                                       sizeof(*in), &wire_size )) ||
            (status = d3dkmt_open_resource_array_size( params->NumAllocations,
                                                       sizeof(*out), &native_size )))
            return status;
        if (wire_size > sizeof(wow64->allocation32) ||
            native_size > sizeof(wow64->allocation))
            return STATUS_INVALID_PARAMETER;
        if ((status = d3dkmt_open_resource_guest_ptr(
                         wow64->packet->guest_allocations, &wow64->allocation_user )))
            return status;
        if ((status = ntdll_wow64_copy_from_user( in, wow64->allocation_user, wire_size )))
            return d3dkmt_open_resource_fault( wow64, status );
        if ((status = d3dkmt_open_resource_guest_ptr( in->private_driver_data,
                                                       &private_driver_data )))
            return status;
        out->hAllocation = in->allocation;
        out->pPrivateDriverData = private_driver_data;
        out->PrivateDriverDataSize = in->private_driver_data_size;
        out->GpuVirtualAddress = in->gpu_address;
        params->pOpenAllocationInfo2 = out;
    }
    else
    {
        struct wine_d3dddi_openallocationinfo32 *in = &wow64->allocation32.info;
        D3DDDI_OPENALLOCATIONINFO *out = &wow64->allocation.info;

        if ((status = d3dkmt_open_resource_array_size( params->NumAllocations,
                                                       sizeof(*in), &wire_size )) ||
            (status = d3dkmt_open_resource_array_size( params->NumAllocations,
                                                       sizeof(*out), &native_size )))
            return status;
        if (wire_size > sizeof(wow64->allocation32) ||
            native_size > sizeof(wow64->allocation))
            return STATUS_INVALID_PARAMETER;
        if ((status = d3dkmt_open_resource_guest_ptr(
                         wow64->packet->guest_allocations, &wow64->allocation_user )))
            return status;
        if ((status = ntdll_wow64_copy_from_user( in, wow64->allocation_user, wire_size )))
            return d3dkmt_open_resource_fault( wow64, status );
        if ((status = d3dkmt_open_resource_guest_ptr( in->private_driver_data,
                                                       &private_driver_data )))
            return status;
        out->hAllocation = in->allocation;
        out->pPrivateDriverData = private_driver_data;
        out->PrivateDriverDataSize = in->private_driver_data_size;
        params->pOpenAllocationInfo = out;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_capture_buffers(
    D3DKMT_OPENRESOURCE *params, struct d3dkmt_open_resource_wow64 *wow64 )
{
    NTSTATUS status;

    if ((status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_private_runtime,
                                                   &params->pPrivateRuntimeData )) ||
        (status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_resource_private,
                                                   &params->pResourcePrivateDriverData )) ||
        (status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_total_private,
                                                   &params->pTotalPrivateDriverDataBuffer )))
        return status;
    if (params->PrivateRuntimeDataSize &&
        (status = ntdll_wow64_probe_user_write( params->pPrivateRuntimeData,
                                                params->PrivateRuntimeDataSize )))
        return d3dkmt_open_resource_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_prepare_outputs(
    BOOL version2, struct d3dkmt_open_resource_wow64 *wow64 )
{
    NTSTATUS status;

    if ((status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_desc,
                                                   &wow64->desc_user )))
        return status;
    wow64->outputs[0].dst = (char *)wow64->desc_user +
        offsetof(struct wine_d3dkmt_open_resource32_desc, total_private_driver_data_size);
    wow64->outputs[0].src = &wow64->packet->total_private_driver_data_size;
    wow64->outputs[0].size = sizeof(uint32_t);
    wow64->outputs[1].dst = (char *)wow64->desc_user +
        offsetof(struct wine_d3dkmt_open_resource32_desc, resource);
    wow64->outputs[1].src = &wow64->packet->resource;
    wow64->outputs[1].size = sizeof(uint32_t);
    wow64->outputs[2].dst = wow64->allocation_user;
    wow64->outputs[2].src = &wow64->allocation32.info.allocation;
    wow64->outputs[2].size = sizeof(uint32_t);
    wow64->outputs[3].dst = (char *)wow64->allocation_user +
        offsetof(struct wine_d3dddi_openallocationinfo32, private_driver_data_size);
    wow64->outputs[3].src = &wow64->allocation32.info.private_driver_data_size;
    wow64->outputs[3].size = sizeof(uint32_t);
    wow64->output_count = 4;
    if (version2)
    {
        wow64->outputs[4].dst = (char *)wow64->allocation_user +
            offsetof(struct wine_d3dddi_openallocationinfo2_32, gpu_address);
        wow64->outputs[4].src = &wow64->allocation32.info2.gpu_address;
        wow64->outputs[4].size = sizeof(uint64_t);
        wow64->output_count++;
    }
    if ((status = ntdll_wow64_probe_user_writev( wow64->outputs,
                                                  wow64->output_count )))
        return d3dkmt_open_resource_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_publish(
    D3DKMT_OPENRESOURCE *params, BOOL version2,
    struct d3dkmt_open_resource_wow64 *wow64 )
{
    NTSTATUS status;

    wow64->packet->total_private_driver_data_size =
        params->TotalPrivateDriverDataBufferSize;
    wow64->packet->resource = params->hResource;
    if (version2)
    {
        wow64->allocation32.info2.allocation = params->pOpenAllocationInfo2->hAllocation;
        wow64->allocation32.info2.private_driver_data_size =
            params->pOpenAllocationInfo2->PrivateDriverDataSize;
        wow64->allocation32.info2.gpu_address =
            params->pOpenAllocationInfo2->GpuVirtualAddress;
    }
    else
    {
        wow64->allocation32.info.allocation = params->pOpenAllocationInfo->hAllocation;
        wow64->allocation32.info.private_driver_data_size =
            params->pOpenAllocationInfo->PrivateDriverDataSize;
    }
    if ((status = ntdll_wow64_atomic_writev( wow64->outputs,
                                              wow64->output_count )))
        return d3dkmt_open_resource_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_params(
    D3DKMT_OPENRESOURCE *params, BOOL version2,
    struct d3dkmt_open_resource_wow64 *wow64 )
{
    struct d3dkmt_object *allocation = NULL;
    struct d3dkmt_resource *resource = NULL;
    struct d3dkmt_device *device;
    D3DDDI_OPENALLOCATIONINFO *alloc_info;
    UINT query_runtime_size, runtime_size;
    NTSTATUS status = STATUS_SUCCESS;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!(device = pin_d3dkmt_object( params->hDevice, D3DKMT_DEVICE )))
        return STATUS_INVALID_PARAMETER;
    if (!is_d3dkmt_global( params->hGlobalShare ))
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if ((status = d3dkmt_object_query( D3DKMT_RESOURCE, params->hGlobalShare,
                                       NULL, &query_runtime_size )))
        goto done;
    if (params->ResourcePrivateDriverDataSize)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (params->NumAllocations != 1 ||
        (wow64 ? !wow64->packet->guest_allocations :
                 (version2 ? !params->pOpenAllocationInfo2 :
                             !params->pOpenAllocationInfo)))
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (params->PrivateRuntimeDataSize &&
        params->PrivateRuntimeDataSize != query_runtime_size)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (wow64 &&
        ((status = d3dkmt_open_resource_capture_allocation( params, version2, wow64 )) ||
         (status = d3dkmt_open_resource_capture_buffers( params, wow64 )) ||
         (status = d3dkmt_open_resource_prepare_outputs( version2, wow64 ))))
        goto done;

    alloc_info = params->pOpenAllocationInfo;
    if ((status = d3dkmt_object_alloc( sizeof(*resource), D3DKMT_RESOURCE,
                                       (void **)&resource )))
        goto done;
    if ((status = d3dkmt_object_alloc( sizeof(*allocation), D3DKMT_ALLOCATION,
                                       (void **)&allocation )))
        goto done;

    runtime_size = params->PrivateRuntimeDataSize;
    if ((status = d3dkmt_object_open( &resource->obj, params->hGlobalShare, NULL,
                                      params->pPrivateRuntimeData, &runtime_size )))
        goto done;
    if ((status = alloc_object_handle( allocation ))) goto done;
    resource->allocation = allocation->local;
    alloc_info->hAllocation = allocation->local;
    alloc_info->PrivateDriverDataSize = 0;

    params->hResource = resource->obj.local;
    params->PrivateRuntimeDataSize = runtime_size;
    params->TotalPrivateDriverDataBufferSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    if (wow64 && (status = d3dkmt_open_resource_publish( params, version2, wow64 )))
        goto done;
    allocation = NULL;
    resource = NULL;

done:
    if (allocation) d3dkmt_object_free( allocation );
    if (resource) d3dkmt_object_free( &resource->obj );
    unpin_d3dkmt_object( &device->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenResource    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenResource( D3DKMT_OPENRESOURCE *params )
{
    return d3dkmt_open_resource_params( params, FALSE, NULL );
}

/******************************************************************************
 *           NtGdiDdDDIOpenResource2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenResource2( D3DKMT_OPENRESOURCE *params )
{
    return d3dkmt_open_resource_params( params, TRUE, NULL );
}

static NTSTATUS d3dkmt_open_resource_nt_capture(
    D3DKMT_OPENRESOURCEFROMNTHANDLE *params,
    struct d3dkmt_open_resource_wow64 *wow64 )
{
    struct wine_d3dddi_openallocationinfo2_32 *in = &wow64->allocation32.info2;
    void *private_driver_data;
    SIZE_T native_size, wire_size;
    NTSTATUS status;

    if ((status = d3dkmt_open_resource_array_size( params->NumAllocations,
                                                   sizeof(*in), &wire_size )) ||
        (status = d3dkmt_open_resource_array_size( params->NumAllocations,
                                                   sizeof(wow64->allocation.info2),
                                                   &native_size )))
        return status;
    if (wire_size > sizeof(wow64->allocation32) ||
        native_size > sizeof(wow64->allocation))
        return STATUS_INVALID_PARAMETER;
    if ((status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_allocations,
                                                   &wow64->allocation_user )))
        return status;
    if ((status = ntdll_wow64_copy_from_user( in, wow64->allocation_user, wire_size )))
        return d3dkmt_open_resource_fault( wow64, status );
    if ((status = d3dkmt_open_resource_guest_ptr( in->private_driver_data,
                                                   &private_driver_data )) ||
        (status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_private_runtime,
                                                   &params->pPrivateRuntimeData )) ||
        (status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_resource_private,
                                                   &params->pResourcePrivateDriverData )) ||
        (status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_total_private,
                                                   &params->pTotalPrivateDriverDataBuffer )) ||
        (status = d3dkmt_open_resource_guest_ptr(
                       wow64->packet->guest_keyed_mutex_runtime,
                       &params->pKeyedMutexPrivateRuntimeData )))
        return status;
    wow64->allocation.info2.hAllocation = in->allocation;
    wow64->allocation.info2.pPrivateDriverData = private_driver_data;
    wow64->allocation.info2.PrivateDriverDataSize = in->private_driver_data_size;
    wow64->allocation.info2.GpuVirtualAddress = in->gpu_address;
    params->pOpenAllocationInfo2 = &wow64->allocation.info2;
    if (params->PrivateRuntimeDataSize &&
        (status = ntdll_wow64_probe_user_write( params->pPrivateRuntimeData,
                                                params->PrivateRuntimeDataSize )))
        return d3dkmt_open_resource_fault( wow64, status );
    if (params->KeyedMutexPrivateRuntimeDataSize &&
        (status = ntdll_wow64_probe_user_write(
                       params->pKeyedMutexPrivateRuntimeData,
                       params->KeyedMutexPrivateRuntimeDataSize )))
        return d3dkmt_open_resource_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_nt_prepare_outputs(
    struct d3dkmt_open_resource_wow64 *wow64 )
{
    NTSTATUS status;

    if ((status = d3dkmt_open_resource_guest_ptr( wow64->packet->guest_desc,
                                                   &wow64->desc_user )))
        return status;
    wow64->outputs[0].dst = (char *)wow64->desc_user +
        offsetof(struct wine_d3dkmt_open_resource_nt32_desc, private_runtime_data_size);
    wow64->outputs[0].src = &wow64->packet->private_runtime_data_size;
    wow64->outputs[0].size = sizeof(uint32_t);
    wow64->outputs[1].dst = (char *)wow64->desc_user +
        offsetof(struct wine_d3dkmt_open_resource_nt32_desc,
                 resource_private_driver_data_size);
    wow64->outputs[1].src = &wow64->packet->resource_private_driver_data_size;
    wow64->outputs[1].size = sizeof(uint32_t);
    wow64->outputs[2].dst = (char *)wow64->desc_user +
        offsetof(struct wine_d3dkmt_open_resource_nt32_desc,
                 total_private_driver_data_size);
    wow64->outputs[2].src = &wow64->packet->total_private_driver_data_size;
    wow64->outputs[2].size = sizeof(uint32_t);
    wow64->outputs[3].dst = (char *)wow64->desc_user +
        offsetof(struct wine_d3dkmt_open_resource_nt32_desc, resource);
    wow64->outputs[3].src = &wow64->packet->resource;
    wow64->outputs[3].size = 2 * sizeof(uint32_t);
    wow64->outputs[4].dst = (char *)wow64->desc_user +
        offsetof(struct wine_d3dkmt_open_resource_nt32_desc, sync_object);
    wow64->outputs[4].src = &wow64->packet->sync_object;
    wow64->outputs[4].size = sizeof(uint32_t);
    wow64->outputs[5].dst = wow64->allocation_user;
    wow64->outputs[5].src = &wow64->allocation32.info2.allocation;
    wow64->outputs[5].size = sizeof(uint32_t);
    wow64->outputs[6].dst = (char *)wow64->allocation_user +
        offsetof(struct wine_d3dddi_openallocationinfo2_32, private_driver_data_size);
    wow64->outputs[6].src = &wow64->allocation32.info2.private_driver_data_size;
    wow64->outputs[6].size = sizeof(uint32_t);
    wow64->outputs[7].dst = (char *)wow64->allocation_user +
        offsetof(struct wine_d3dddi_openallocationinfo2_32, gpu_address);
    wow64->outputs[7].src = &wow64->allocation32.info2.gpu_address;
    wow64->outputs[7].size = sizeof(uint64_t);
    wow64->output_count = 8;
    if ((status = ntdll_wow64_probe_user_writev( wow64->outputs,
                                                  wow64->output_count )))
        return d3dkmt_open_resource_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_nt_publish(
    D3DKMT_OPENRESOURCEFROMNTHANDLE *params,
    struct d3dkmt_open_resource_wow64 *wow64 )
{
    NTSTATUS status;

    wow64->packet->private_runtime_data_size = params->PrivateRuntimeDataSize;
    wow64->packet->resource_private_driver_data_size =
        params->ResourcePrivateDriverDataSize;
    wow64->packet->total_private_driver_data_size =
        params->TotalPrivateDriverDataBufferSize;
    wow64->packet->resource = params->hResource;
    wow64->packet->keyed_mutex = params->hKeyedMutex;
    wow64->packet->sync_object = params->hSyncObject;
    wow64->allocation32.info2.allocation = params->pOpenAllocationInfo2->hAllocation;
    wow64->allocation32.info2.private_driver_data_size =
        params->pOpenAllocationInfo2->PrivateDriverDataSize;
    wow64->allocation32.info2.gpu_address =
        params->pOpenAllocationInfo2->GpuVirtualAddress;
    if ((status = ntdll_wow64_atomic_writev( wow64->outputs,
                                              wow64->output_count )))
        return d3dkmt_open_resource_fault( wow64, status );
    return STATUS_SUCCESS;
}

static NTSTATUS d3dkmt_open_resource_nt( D3DKMT_OPENRESOURCEFROMNTHANDLE *params,
                                         struct d3dkmt_open_resource_wow64 *wow64 )
{
    struct d3dkmt_object *sync = NULL;
    struct d3dkmt_mutex *mutex = NULL;
    struct d3dkmt_resource *resource = NULL;
    struct d3dkmt_device *device;
    UINT dummy = 0, query_runtime_size;
    NTSTATUS status = STATUS_SUCCESS;

    FIXME( "params %p semi-stub!\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!(device = pin_d3dkmt_object( params->hDevice, D3DKMT_DEVICE )))
        return STATUS_INVALID_PARAMETER;
    if ((status = d3dkmt_object_query( D3DKMT_RESOURCE, 0, params->hNtHandle,
                                       &query_runtime_size )))
        goto done;
    if (wow64 ? (!wow64->packet->guest_private_runtime ||
                 !wow64->packet->guest_total_private ||
                 !wow64->packet->guest_allocations) :
                (!params->pPrivateRuntimeData ||
                 !params->pTotalPrivateDriverDataBuffer ||
                 !params->pOpenAllocationInfo2))
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (params->NumAllocations != 1)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (params->PrivateRuntimeDataSize &&
        params->PrivateRuntimeDataSize != query_runtime_size)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (wow64 &&
        ((status = d3dkmt_open_resource_nt_capture( params, wow64 )) ||
         (status = d3dkmt_open_resource_nt_prepare_outputs( wow64 ))))
        goto done;

    if ((status = d3dkmt_object_alloc( sizeof(*resource), D3DKMT_RESOURCE,
                                       (void **)&resource )))
        goto done;
    if ((status = d3dkmt_object_alloc( sizeof(*mutex), D3DKMT_MUTEX,
                                       (void **)&mutex )))
        goto done;
    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC,
                                       (void **)&sync )))
        goto done;

    if ((status = d3dkmt_object_open( &resource->obj, 0, params->hNtHandle,
                                      params->pPrivateRuntimeData,
                                      &params->PrivateRuntimeDataSize )))
        goto done;
    if (d3dkmt_object_open( &mutex->obj, 0, params->hNtHandle,
                            params->pKeyedMutexPrivateRuntimeData,
                            &params->KeyedMutexPrivateRuntimeDataSize ))
    {
        d3dkmt_object_free( &mutex->obj );
        mutex = NULL;
    }
    if (d3dkmt_object_open( sync, 0, params->hNtHandle, NULL, &dummy ))
    {
        d3dkmt_object_free( sync );
        sync = NULL;
    }

    params->hResource = resource->obj.local;
    params->hKeyedMutex = mutex ? mutex->obj.local : 0;
    params->hSyncObject = sync ? sync->local : 0;
    params->TotalPrivateDriverDataBufferSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    if (wow64 && (status = d3dkmt_open_resource_nt_publish( params, wow64 )))
        goto done;
    resource = NULL;
    mutex = NULL;
    sync = NULL;

done:
    if (sync) d3dkmt_object_free( sync );
    if (mutex) d3dkmt_object_free( &mutex->obj );
    if (resource) d3dkmt_object_free( &resource->obj );
    unpin_d3dkmt_object( &device->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenResourceFromNtHandle    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenResourceFromNtHandle( D3DKMT_OPENRESOURCEFROMNTHANDLE *params )
{
    return d3dkmt_open_resource_nt( params, NULL );
}

NTSTATUS WINAPI __wine_win32u_d3dkmt_open_resource(
    struct wine_d3dkmt_open_resource32 *packet )
{
    struct d3dkmt_open_resource_wow64 wow64 = { .packet = packet };

    if (!packet || packet->version != WINE_D3DKMT_OPEN_RESOURCE32_VERSION ||
        packet->size != sizeof(*packet) || packet->fault_status ||
        packet->reserved[0] || packet->reserved[1])
        return STATUS_INVALID_PARAMETER;
    packet->fault_status = 0;
    if (packet->variant == WINE_D3DKMT_OPEN_RESOURCE32_NT_HANDLE)
    {
        D3DKMT_OPENRESOURCEFROMNTHANDLE params = {0};

        params.hDevice = packet->device;
        params.hNtHandle = UlongToHandle( packet->shared_handle );
        params.NumAllocations = packet->allocation_count;
        params.PrivateRuntimeDataSize = packet->private_runtime_data_size;
        params.ResourcePrivateDriverDataSize =
            packet->resource_private_driver_data_size;
        params.TotalPrivateDriverDataBufferSize =
            packet->total_private_driver_data_size;
        params.KeyedMutexPrivateRuntimeDataSize =
            packet->keyed_mutex_private_runtime_data_size;
        params.hResource = packet->resource;
        params.hKeyedMutex = packet->keyed_mutex;
        params.hSyncObject = packet->sync_object;
        return d3dkmt_open_resource_nt( &params, &wow64 );
    }
    else
    {
        D3DKMT_OPENRESOURCE params = {0};
        BOOL version2;

        if (packet->variant == WINE_D3DKMT_OPEN_RESOURCE32_V1) version2 = FALSE;
        else if (packet->variant == WINE_D3DKMT_OPEN_RESOURCE32_V2) version2 = TRUE;
        else return STATUS_INVALID_PARAMETER;
        params.hDevice = packet->device;
        params.hGlobalShare = packet->shared_handle;
        params.NumAllocations = packet->allocation_count;
        params.PrivateRuntimeDataSize = packet->private_runtime_data_size;
        params.ResourcePrivateDriverDataSize =
            packet->resource_private_driver_data_size;
        params.TotalPrivateDriverDataBufferSize =
            packet->total_private_driver_data_size;
        params.hResource = packet->resource;
        return d3dkmt_open_resource_params( &params, version2, &wow64 );
    }
}


/******************************************************************************
 *           NtGdiDdDDIOpenNtHandleFromName    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenNtHandleFromName( D3DKMT_OPENNTHANDLEFROMNAME *params )
{
    OBJECT_ATTRIBUTES *attr = params->pObjAttrib;
    DWORD access = params->dwDesiredAccess;
    NTSTATUS status;

    TRACE( "params %p\n", params );

    params->hNtHandle = 0;
    if ((status = validate_open_object_attributes( attr ))) return status;

    SERVER_START_REQ( d3dkmt_object_open_name )
    {
        req->type       = D3DKMT_RESOURCE;
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName) wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        status = wine_server_call( req );
        params->hNtHandle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    return status;
}


/******************************************************************************
 *           NtGdiDdDDIQueryResourceInfo    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryResourceInfo( D3DKMT_QUERYRESOURCEINFO *params )
{
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!validate_d3dkmt_object_handle( params->hDevice, D3DKMT_DEVICE )) return STATUS_INVALID_PARAMETER;
    if (!is_d3dkmt_global( params->hGlobalShare )) return STATUS_INVALID_PARAMETER;

    if ((status = d3dkmt_object_query( D3DKMT_RESOURCE, params->hGlobalShare, NULL,
                                       &params->PrivateRuntimeDataSize )))
        return status;

    params->TotalPrivateDriverDataSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    params->NumAllocations = 1;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDIQueryResourceInfoFromNtHandle    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryResourceInfoFromNtHandle( D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *params )
{
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if ((status = d3dkmt_object_query( D3DKMT_RESOURCE, 0, params->hNtHandle,
                                       &params->PrivateRuntimeDataSize )))
        return status;

    params->TotalPrivateDriverDataSize = 0;
    params->ResourcePrivateDriverDataSize = 0;
    params->NumAllocations = 1;
    return STATUS_SUCCESS;
}


/******************************************************************************
 *           NtGdiDdDDICreateKeyedMutex2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICreateKeyedMutex2( D3DKMT_CREATEKEYEDMUTEX2 *params )
{
    struct d3dkmt_mutex *mutex;
    NTSTATUS status;

    FIXME( "params %p semi-stub!\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;

    if ((status = d3dkmt_object_alloc( sizeof(*mutex), D3DKMT_MUTEX, (void **)&mutex ))) return status;
    if ((status = d3dkmt_object_create( &mutex->obj, -1, params->InitialValue, params->Flags.NtSecuritySharing,
                                        params->pPrivateRuntimeData, params->PrivateRuntimeDataSize )))
        goto failed;

    params->hSharedHandle = mutex->obj.shared ? 0 : mutex->obj.global;
    params->hKeyedMutex = mutex->obj.local;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( &mutex->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDICreateKeyedMutex    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICreateKeyedMutex( D3DKMT_CREATEKEYEDMUTEX *params )
{
    D3DKMT_CREATEKEYEDMUTEX2 params2 = {0};
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;

    params2.InitialValue = params->InitialValue;
    status = NtGdiDdDDICreateKeyedMutex2( &params2 );
    params->hSharedHandle = params2.hSharedHandle;
    params->hKeyedMutex = params2.hKeyedMutex;
    return status;
}

NTSTATUS d3dkmt_destroy_mutex( D3DKMT_HANDLE local )
{
    TRACE( "local %#x\n", local );
    return release_d3dkmt_object_handle( local, D3DKMT_MUTEX );
}

/******************************************************************************
 *           NtGdiDdDDIDestroyKeyedMutex    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIDestroyKeyedMutex( const D3DKMT_DESTROYKEYEDMUTEX *params )
{
    TRACE( "params %p\n", params );

    return d3dkmt_destroy_mutex( params->hKeyedMutex );
}

/******************************************************************************
 *           NtGdiDdDDIOpenKeyedMutex2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenKeyedMutex2( D3DKMT_OPENKEYEDMUTEX2 *params )
{
    struct d3dkmt_mutex *mutex;
    UINT runtime_size;
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!is_d3dkmt_global( params->hSharedHandle )) return STATUS_INVALID_PARAMETER;
    if (params->PrivateRuntimeDataSize && !params->pPrivateRuntimeData) return STATUS_INVALID_PARAMETER;

    if ((status = d3dkmt_object_alloc( sizeof(*mutex), D3DKMT_MUTEX, (void **)&mutex ))) return status;

    runtime_size = params->PrivateRuntimeDataSize;
    if ((status = d3dkmt_object_open( &mutex->obj, params->hSharedHandle, NULL, params->pPrivateRuntimeData, &runtime_size ))) goto failed;

    params->hKeyedMutex = mutex->obj.local;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( &mutex->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenKeyedMutex    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenKeyedMutex( D3DKMT_OPENKEYEDMUTEX *params )
{
    D3DKMT_OPENKEYEDMUTEX2 params2 = {0};
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;

    params2.hSharedHandle = params->hSharedHandle;
    status = NtGdiDdDDIOpenKeyedMutex2( &params2 );
    params->hKeyedMutex = params2.hKeyedMutex;
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenKeyedMutexFromNtHandle    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenKeyedMutexFromNtHandle( D3DKMT_OPENKEYEDMUTEXFROMNTHANDLE *params )
{
    struct d3dkmt_mutex *mutex;
    NTSTATUS status;

    FIXME( "params %p semi-stub!\n", params );

    if ((status = d3dkmt_object_alloc( sizeof(*mutex), D3DKMT_MUTEX, (void **)&mutex ))) return status;
    if ((status = d3dkmt_object_open( &mutex->obj, 0, params->hNtHandle, params->pPrivateRuntimeData,
                                      &params->PrivateRuntimeDataSize )))
        goto failed;

    params->hKeyedMutex = mutex->obj.local;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( &mutex->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIAcquireKeyedMutex2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIAcquireKeyedMutex2( D3DKMT_ACQUIREKEYEDMUTEX2 *params )
{
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER now, *timeout;
    struct d3dkmt_mutex *mutex;
    HANDLE wait_handle = NULL;

    TRACE( "params %p\n", params );

    if ((timeout = params->pTimeout) && timeout->QuadPart < 0)
    {
        NtQuerySystemTime( &now );
        now.QuadPart -= timeout->QuadPart;
        timeout = &now;
    }

    if (!(mutex = pin_d3dkmt_object( params->hKeyedMutex, D3DKMT_MUTEX ))) return STATUS_INVALID_PARAMETER;

    do
    {
        if (wait_handle) status = NtWaitForSingleObject( wait_handle, FALSE, timeout );
        SERVER_START_REQ( d3dkmt_mutex_acquire )
        {
            req->mutex = mutex->obj.global;
            req->key_value = params->Key;
            req->wait_handle = wine_server_obj_handle( wait_handle );
            req->wait_status = status;

            status = wine_server_call( req );
            params->FenceValue = reply->fence_value;
            /* server never creates a new handle if one is provided, and always returns a handle if pending */
            if (reply->wait_handle) wait_handle = wine_server_ptr_handle( reply->wait_handle );
            else if (wait_handle) NtClose( wait_handle );
        }
        SERVER_END_REQ;
    } while (status == STATUS_PENDING);

    if (!status)
    {
        pthread_mutex_lock( &d3dkmt_lock );
        mutex->owned = TRUE;
        pthread_mutex_unlock( &d3dkmt_lock );
    }
    unpin_d3dkmt_object( &mutex->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIAcquireKeyedMutex    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIAcquireKeyedMutex( D3DKMT_ACQUIREKEYEDMUTEX *params )
{
    D3DKMT_ACQUIREKEYEDMUTEX2 params2 = {0};
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    params2.hKeyedMutex = params->hKeyedMutex;
    params2.pTimeout = params->pTimeout;
    params2.Key = params->Key;
    params2.FenceValue = params->FenceValue;
    status = NtGdiDdDDIAcquireKeyedMutex2( &params2 );
    params->FenceValue = params2.FenceValue;

    return status;
}

/******************************************************************************
 *           NtGdiDdDDIReleaseKeyedMutex2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIReleaseKeyedMutex2( D3DKMT_RELEASEKEYEDMUTEX2 *params )
{
    struct d3dkmt_mutex *mutex;
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if (!(mutex = pin_d3dkmt_object( params->hKeyedMutex, D3DKMT_MUTEX ))) return STATUS_INVALID_PARAMETER;

    SERVER_START_REQ( d3dkmt_mutex_release )
    {
        req->mutex = mutex->obj.global;
        req->key_value = params->Key;
        req->fence_value = params->FenceValue;
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    if (!status)
    {
        pthread_mutex_lock( &d3dkmt_lock );
        mutex->owned = FALSE;
        pthread_mutex_unlock( &d3dkmt_lock );
    }

    unpin_d3dkmt_object( &mutex->obj );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIReleaseKeyedMutex    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIReleaseKeyedMutex( D3DKMT_RELEASEKEYEDMUTEX *params )
{
    D3DKMT_RELEASEKEYEDMUTEX2 params2 = {0};

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    params2.hKeyedMutex = params->hKeyedMutex;
    params2.Key = params->Key;
    params2.FenceValue = params->FenceValue;
    return NtGdiDdDDIReleaseKeyedMutex2( &params2 );
}


/******************************************************************************
 *           NtGdiDdDDICreateSynchronizationObject2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICreateSynchronizationObject2( D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *params )
{
    struct d3dkmt_object *sync;
    NTSTATUS status;

    FIXME( "params %p semi-stub!\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!validate_d3dkmt_object_handle( params->hDevice, D3DKMT_DEVICE )) return STATUS_INVALID_PARAMETER;

    if (params->Info.Type < D3DDDI_SYNCHRONIZATION_MUTEX || params->Info.Type > D3DDDI_MONITORED_FENCE)
        return STATUS_INVALID_PARAMETER;

    if (params->Info.Type == D3DDDI_CPU_NOTIFICATION && !params->Info.CPUNotification.Event) return STATUS_INVALID_HANDLE;
    if (params->Info.Flags.NtSecuritySharing && !params->Info.Flags.Shared) return STATUS_INVALID_PARAMETER;

    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC, (void **)&sync ))) return status;
    if (!params->Info.Flags.Shared) status = alloc_object_handle( sync );
    else status = d3dkmt_object_create( sync, -1, 0, params->Info.Flags.NtSecuritySharing, NULL, 0 );
    if (status) goto failed;

    if (params->Info.Flags.Shared) params->Info.SharedHandle = sync->shared ? 0 : sync->global;
    params->hSyncObject = sync->local;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( sync );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDICreateSynchronizationObject    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICreateSynchronizationObject( D3DKMT_CREATESYNCHRONIZATIONOBJECT *params )
{
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 params2 = {0};
    NTSTATUS status;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;

    if (params->Info.Type != D3DDDI_SYNCHRONIZATION_MUTEX && params->Info.Type != D3DDDI_SEMAPHORE)
        return STATUS_INVALID_PARAMETER;

    params2.hDevice = params->hDevice;
    params2.Info.Type = params->Info.Type;
    params2.Info.Flags.Shared = 1;
    memcpy( &params2.Info.Reserved, &params->Info.Reserved, sizeof(params->Info.Reserved) );
    status = NtGdiDdDDICreateSynchronizationObject2( &params2 );
    params->hSyncObject = params2.hSyncObject;
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenSyncObjectFromNtHandle2    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenSyncObjectFromNtHandle2( D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 *params )
{
    struct d3dkmt_object *sync;
    NTSTATUS status;
    UINT dummy = 0;

    FIXME( "params %p semi-stub!\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!validate_d3dkmt_object_handle( params->hDevice, D3DKMT_DEVICE )) return STATUS_INVALID_PARAMETER;

    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC, (void **)&sync ))) return status;
    if ((status = d3dkmt_object_open( sync, 0, params->hNtHandle, NULL, &dummy ))) goto failed;

    params->hSyncObject = sync->local;
    params->MonitoredFence.FenceValueCPUVirtualAddress = 0;
    params->MonitoredFence.FenceValueGPUVirtualAddress = 0;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( sync );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenSyncObjectFromNtHandle    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenSyncObjectFromNtHandle( D3DKMT_OPENSYNCOBJECTFROMNTHANDLE *params )
{
    struct d3dkmt_object *sync;
    NTSTATUS status;
    UINT dummy = 0;

    FIXME( "params %p semi-stub!\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;

    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC, (void **)&sync ))) return status;
    if ((status = d3dkmt_object_open( sync, 0, params->hNtHandle, NULL, &dummy ))) goto failed;

    params->hSyncObject = sync->local;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( sync );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenSyncObjectNtHandleFromName    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenSyncObjectNtHandleFromName( D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME *params )
{
    OBJECT_ATTRIBUTES *attr = params->pObjAttrib;
    DWORD access = params->dwDesiredAccess;
    NTSTATUS status;

    TRACE( "params %p\n", params );

    params->hNtHandle = 0;
    if ((status = validate_open_object_attributes( attr ))) return status;

    SERVER_START_REQ( d3dkmt_object_open_name )
    {
        req->type       = D3DKMT_SYNC;
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName) wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        status = wine_server_call( req );
        params->hNtHandle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenSynchronizationObject    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenSynchronizationObject( D3DKMT_OPENSYNCHRONIZATIONOBJECT *params )
{
    struct d3dkmt_object *sync;
    NTSTATUS status;
    UINT dummy = 0;

    TRACE( "params %p\n", params );

    if (!params) return STATUS_INVALID_PARAMETER;
    if (!is_d3dkmt_global( params->hSharedHandle )) return STATUS_INVALID_PARAMETER;

    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC, (void **)&sync ))) return status;
    if ((status = d3dkmt_object_open( sync, params->hSharedHandle, NULL, NULL, &dummy ))) goto failed;

    params->hSyncObject = sync->local;
    return STATUS_SUCCESS;

failed:
    d3dkmt_object_free( sync );
    return status;
}

/******************************************************************************
 *           NtGdiDdDDIDestroySynchronizationObject    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIDestroySynchronizationObject( const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *params )
{
    TRACE( "params %p\n", params );

    return d3dkmt_destroy_sync( params->hSyncObject );
}

/******************************************************************************
 *           NtGdiDdDDISignalSynchronizationObjectFromCpu    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDISignalSynchronizationObjectFromCpu( const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *params )
{
    FIXME( "params %p stub!\n", params );
    return STATUS_NOT_IMPLEMENTED;
}

/******************************************************************************
 *           NtGdiDdDDIWaitForSynchronizationObjectFromCpu    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIWaitForSynchronizationObjectFromCpu( const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *params )
{
    FIXME( "params %p stub!\n", params );
    return STATUS_NOT_IMPLEMENTED;
}

static void get_resource_global_keyed_mutex( struct d3dkmt_dxgi_desc *desc, D3DKMT_HANDLE *mutex_global, D3DKMT_HANDLE *sync_global )
{
    if ((desc->size != sizeof(struct d3dkmt_d3d9_desc) && desc->size != sizeof(struct d3dkmt_d3d11_desc)) ||
        (desc->version != 0 && desc->version != 1 && desc->version != 4))
        WARN( "Unsupported runtime data size %#x version %#x\n", desc->size, desc->version );
    else if (desc->keyed_mutex && !desc->nt_shared)
    {
        *mutex_global = desc->mutex_handle;
        *sync_global = desc->sync_handle;
    }
}

/* get a locally opened D3DKMT object host-specific fd */
int d3dkmt_object_get_fd( D3DKMT_HANDLE local )
{
    struct d3dkmt_object *object;
    NTSTATUS status;
    int fd;

    TRACE( "local %#x\n", local );

    if (!(object = pin_d3dkmt_object( local, -1 ))) return -1;
    if ((status = wine_server_handle_to_fd( object->handle, GENERIC_ALL, &fd, NULL )))
    {
        WARN( "Failed to receive object %p/%#x fd, status %#x\n", object, local, status );
        unpin_d3dkmt_object( object );
        return -1;
    }

    unpin_d3dkmt_object( object );
    return fd;
}

/* create a D3DKMT global or shared resource from a host-specific fd */
D3DKMT_HANDLE d3dkmt_create_resource( int fd, D3DKMT_HANDLE *global )
{
    struct d3dkmt_resource *resource = NULL;
    struct d3dkmt_object *allocation = NULL;
    NTSTATUS status;

    TRACE( "fd %d, global %p\n", fd, global );

    if ((status = d3dkmt_object_alloc( sizeof(*resource), D3DKMT_RESOURCE, (void **)&resource ))) goto failed;
    if ((status = d3dkmt_object_alloc( sizeof(*allocation), D3DKMT_ALLOCATION, (void **)&allocation ))) goto failed;
    if ((status = d3dkmt_object_create( &resource->obj, fd, 0, !global, NULL, 0 ))) goto failed;

    if ((status = alloc_object_handle( allocation ))) goto failed;
    resource->allocation = allocation->local;

    if (global) *global = resource->obj.global;
    return resource->obj.local;

failed:
    WARN( "Failed to create resource, status %#x\n", status );
    if (allocation) d3dkmt_object_free( allocation );
    if (resource) d3dkmt_object_free( &resource->obj );
    return 0;
}

/* open a D3DKMT global or shared resource */
D3DKMT_HANDLE d3dkmt_open_resource( D3DKMT_HANDLE global, HANDLE shared, D3DKMT_HANDLE *mutex_local, D3DKMT_HANDLE *sync_local )
{
    struct d3dkmt_object *allocation = NULL, *mutex = NULL, *sync = NULL;
    UINT runtime_size, mutex_size = 0, sync_size = 0;
    D3DKMT_HANDLE mutex_global = 0, sync_global = 0;
    struct d3dkmt_resource *resource = NULL;
    void *runtime_data = NULL;
    NTSTATUS status;

    TRACE( "global %#x, shared %p\n", global, shared );

    if ((status = d3dkmt_object_query( D3DKMT_RESOURCE, global, shared, &runtime_size ))) goto failed;
    if (runtime_size && !(runtime_data = malloc( runtime_size ))) goto failed;

    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC, (void **)&sync ))) goto failed;
    if ((status = d3dkmt_object_alloc( sizeof(*mutex), D3DKMT_MUTEX, (void **)&mutex ))) goto failed;
    if ((status = d3dkmt_object_alloc( sizeof(*resource), D3DKMT_RESOURCE, (void **)&resource ))) goto failed;
    if ((status = d3dkmt_object_alloc( sizeof(*allocation), D3DKMT_ALLOCATION, (void **)&allocation ))) goto failed;
    if ((status = d3dkmt_object_open( &resource->obj, global, shared, runtime_data, &runtime_size ))) goto failed;

    if ((status = alloc_object_handle( allocation ))) goto failed;
    resource->allocation = allocation->local;

    if (!runtime_data || runtime_size <= sizeof(struct d3dkmt_dxgi_desc)) WARN( "Unsupported runtime data size %#x\n", runtime_size );
    else get_resource_global_keyed_mutex( runtime_data, &mutex_global, &sync_global );

    if (!d3dkmt_object_open( mutex, mutex_global, shared, NULL, &mutex_size ) &&
        !d3dkmt_object_open( sync, sync_global, shared, NULL, &sync_size ))
    {
        *mutex_local = mutex->local;
        *sync_local = sync->local;
    }
    else
    {
        d3dkmt_object_free( mutex );
        d3dkmt_object_free( sync );
        *mutex_local = *sync_local = 0;
    }

    free( runtime_data );
    return resource->obj.local;

failed:
    WARN( "Failed to open resource, status %#x\n", status );
    if (allocation) d3dkmt_object_free( allocation );
    if (resource) d3dkmt_object_free( &resource->obj );
    if (mutex) d3dkmt_object_free( mutex );
    if (sync) d3dkmt_object_free( sync );
    free( runtime_data );
    return 0;
}

/* destroy a locally opened D3DKMT resource */
NTSTATUS d3dkmt_destroy_resource( D3DKMT_HANDLE local )
{
    struct d3dkmt_resource *resource;
    D3DKMT_HANDLE allocation;
    NTSTATUS status;

    TRACE( "local %#x\n", local );

    if (!(resource = pin_d3dkmt_object( local, D3DKMT_RESOURCE ))) return STATUS_INVALID_PARAMETER;
    allocation = resource->allocation;
    status = release_d3dkmt_object_handle( local, D3DKMT_RESOURCE );
    unpin_d3dkmt_object( &resource->obj );
    if (status) return status;
    if (allocation) release_d3dkmt_object_handle( allocation, D3DKMT_ALLOCATION );

    return STATUS_SUCCESS;
}

/* create a D3DKMT global or shared sync */
D3DKMT_HANDLE d3dkmt_create_sync( int fd, D3DKMT_HANDLE *global )
{
    struct d3dkmt_object *sync = NULL;
    NTSTATUS status;

    TRACE( "global %p\n", global );

    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC, (void **)&sync ))) goto failed;
    if ((status = d3dkmt_object_create( sync, fd, 0, !global, NULL, 0 ))) goto failed;
    if (global) *global = sync->global;
    return sync->local;

failed:
    WARN( "Failed to create sync, status %#x\n", status );
    if (sync) d3dkmt_object_free( sync );
    return 0;
}

/* open a D3DKMT global or shared sync */
D3DKMT_HANDLE d3dkmt_open_sync( D3DKMT_HANDLE global, HANDLE shared )
{
    struct d3dkmt_object *sync = NULL;
    NTSTATUS status;
    UINT dummy = 0;

    TRACE( "global %#x, shared %p\n", global, shared );

    if ((status = d3dkmt_object_alloc( sizeof(*sync), D3DKMT_SYNC, (void **)&sync ))) goto failed;
    if ((status = d3dkmt_object_open( sync, global, shared, NULL, &dummy ))) goto failed;
    return sync->local;

failed:
    WARN( "Failed to open sync, status %#x\n", status );
    if (sync) d3dkmt_object_free( sync );
    return 0;
}

/* destroy a locally opened D3DKMT sync */
NTSTATUS d3dkmt_destroy_sync( D3DKMT_HANDLE local )
{
    TRACE( "local %#x\n", local );
    return release_d3dkmt_object_handle( local, D3DKMT_SYNC );
}
