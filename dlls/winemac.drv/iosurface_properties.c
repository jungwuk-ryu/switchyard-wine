/*
 * macOS IOSurface property helpers
 *
 * Copyright 2026 Switchyard project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <IOSurface/IOSurface.h>

#include "iosurface_properties.h"

static int set_number(CFMutableDictionaryRef properties, CFStringRef key, uint32_t value)
{
    int64_t number_value = value;
    CFNumberRef number;

    if (!(number = CFNumberCreate(NULL, kCFNumberSInt64Type, &number_value))) return 0;
    CFDictionarySetValue(properties, key, number);
    CFRelease(number);
    return 1;
}

CFMutableDictionaryRef macdrv_create_private_iosurface_properties(unsigned int width,
        unsigned int height, unsigned int bytes_per_element, uint32_t pixel_format)
{
    CFMutableDictionaryRef properties;

    if (!width || !height || !bytes_per_element) return NULL;
    properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    if (!properties) return NULL;
    if (!set_number(properties, kIOSurfaceWidth, width) ||
        !set_number(properties, kIOSurfaceHeight, height) ||
        !set_number(properties, kIOSurfaceBytesPerElement, bytes_per_element) ||
        !set_number(properties, kIOSurfacePixelFormat, pixel_format))
    {
        CFRelease(properties);
        return NULL;
    }
    return properties;
}
