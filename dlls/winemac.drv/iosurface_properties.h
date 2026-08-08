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

#ifndef __WINE_MACDRV_IOSURFACE_PROPERTIES_H
#define __WINE_MACDRV_IOSURFACE_PROPERTIES_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>

CFMutableDictionaryRef macdrv_create_private_iosurface_properties(unsigned int width,
        unsigned int height, unsigned int bytes_per_element, uint32_t pixel_format);

#endif /* __WINE_MACDRV_IOSURFACE_PROPERTIES_H */
