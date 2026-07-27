/*
 * Switchyard host GPU identity helper
 *
 * Copyright 2026 Jungwuk
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

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct gpu_identity
{
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t subsystem_id;
    uint32_t revision_id;
    char name[256];
};

static int get_entry_property_uint32(io_registry_entry_t entry, CFStringRef property,
        uint32_t *value)
{
    CFTypeRef object;
    int ret = -1;

    object = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, property,
            kCFAllocatorDefault, 0);
    if (!object)
        return ret;

    if (CFGetTypeID(object) == CFDataGetTypeID()
            && CFDataGetLength((CFDataRef)object) >= (CFIndex)sizeof(*value))
    {
        CFDataGetBytes((CFDataRef)object, CFRangeMake(0, sizeof(*value)),
                (UInt8 *)value);
        ret = 0;
    }
    else if (CFGetTypeID(object) == CFNumberGetTypeID()
            && CFNumberGetValue((CFNumberRef)object, kCFNumberSInt32Type, value))
    {
        ret = 0;
    }

    CFRelease(object);
    return ret;
}

static int get_entry_property_string(io_registry_entry_t entry, CFStringRef property,
        char *buffer, size_t size)
{
    CFTypeRef object;
    int ret = -1;

    object = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, property,
            kCFAllocatorDefault, 0);
    if (!object)
        return ret;

    if (CFGetTypeID(object) == CFDataGetTypeID())
    {
        CFIndex length = CFDataGetLength((CFDataRef)object);

        if (length >= 0 && length < (CFIndex)size)
        {
            CFDataGetBytes((CFDataRef)object, CFRangeMake(0, length),
                    (UInt8 *)buffer);
            buffer[length] = 0;
            ret = 0;
        }
    }
    else if (CFGetTypeID(object) == CFStringGetTypeID()
            && CFStringGetCString((CFStringRef)object, buffer, size,
                    kCFStringEncodingUTF8))
    {
        ret = 0;
    }

    CFRelease(object);
    return ret;
}

static int get_identity_from_entry(struct gpu_identity *identity,
        io_registry_entry_t entry)
{
    io_registry_entry_t current = entry, parent;
    kern_return_t result;

    if (!entry)
        return -1;

    while (!IOObjectConformsTo(current, "IOPCIDevice"))
    {
        result = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent);
        if (result == kIOReturnNoDevice)
        {
            if (current != entry)
                IOObjectRelease(current);
            current = entry;
            break;
        }
        if (result != kIOReturnSuccess)
        {
            if (current != entry)
                IOObjectRelease(current);
            return -1;
        }

        if (current != entry)
            IOObjectRelease(current);
        current = parent;
    }

    get_entry_property_uint32(current, CFSTR("vendor-id"), &identity->vendor_id);
    get_entry_property_uint32(current, CFSTR("device-id"), &identity->device_id);
    get_entry_property_uint32(current, CFSTR("subsystem-id"),
            &identity->subsystem_id);
    get_entry_property_uint32(current, CFSTR("revision-id"),
            &identity->revision_id);
    get_entry_property_string(current, CFSTR("model"), identity->name,
            sizeof(identity->name));

    if (current != entry)
        IOObjectRelease(current);
    return 0;
}

static int get_default_gpu_identity(struct gpu_identity *identity)
{
    id<MTLDevice> device;
    io_registry_entry_t entry;
    uint64_t registry_id;

    memset(identity, 0, sizeof(*identity));
    device = MTLCreateSystemDefaultDevice();
    if (!device || ![device respondsToSelector:@selector(registryID)])
        return -1;

    registry_id = [device registryID];
    entry = IOServiceGetMatchingService(0, IORegistryEntryIDMatching(registry_id));
    if (!entry)
        return -1;

    get_identity_from_entry(identity, entry);
    IOObjectRelease(entry);

#if defined(MAC_OS_X_VERSION_10_15) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_15
    /*
     * Apple GPUs are not PCI devices and have no device-id property.  Match
     * winemac's stable identifier by using the highest supported Apple Metal
     * GPU family.
     */
    if (!identity->device_id
            && [device respondsToSelector:@selector(supportsFamily:)]
            && [device supportsFamily:MTLGPUFamilyApple1])
    {
        MTLGPUFamily highest = MTLGPUFamilyApple1;

        while (highest < UINT16_MAX)
        {
            MTLGPUFamily next = highest + 1;

            if (![device supportsFamily:next])
                break;
            highest = next;
        }
        identity->device_id = highest;
    }
#endif

    if (!identity->vendor_id
            && [device respondsToSelector:@selector(supportsFamily:)]
            && [device supportsFamily:MTLGPUFamilyApple1])
        identity->vendor_id = 0x106b;

    if (!identity->name[0])
    {
        const char *name = [[device name] UTF8String];

        if (name)
            snprintf(identity->name, sizeof(identity->name), "%s", name);
    }

    return identity->vendor_id && identity->device_id && identity->name[0] ? 0 : -1;
}

static void sanitize_name(char *name)
{
    unsigned char *cursor = (unsigned char *)name;

    while (*cursor)
    {
        if (*cursor == '\t' || *cursor == '\r' || *cursor == '\n'
                || *cursor < 0x20 || *cursor == 0x7f)
            *cursor = ' ';
        cursor++;
    }
}

int main(void)
{
    struct gpu_identity identity;

    @autoreleasepool
    {
        if (get_default_gpu_identity(&identity))
        {
            fputs("Could not determine the default Metal GPU identity.\n", stderr);
            return 1;
        }
    }

    sanitize_name(identity.name);
    printf("%08x\t%08x\t%08x\t%08x\t%s\n", identity.vendor_id,
            identity.device_id, identity.subsystem_id, identity.revision_id,
            identity.name);
    return 0;
}
