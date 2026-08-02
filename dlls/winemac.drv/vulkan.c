/* Mac Driver Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2018 Andrew Eikum for CodeWeavers
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

/* NOTE: If making changes here, consider whether they should be reflected in
 * the other drivers. */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <math.h>

#include "ntstatus.h"
#include "macdrv.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static const struct vulkan_driver_funcs macdrv_vulkan_driver_funcs;

static BOOL macdrv_color_config_from_vk(VkFormat format, VkColorSpaceKHR color_space,
        struct macdrv_metal_color_config *config)
{
    memset(config, 0, sizeof(*config));

    switch (color_space)
    {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
            config->color_space = MACDRV_METAL_COLOR_SPACE_SRGB;
            config->pixel_format = MACDRV_METAL_PIXEL_FORMAT_PROVIDER;
            return TRUE;

        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
            if (format != VK_FORMAT_R16G16B16A16_SFLOAT) return FALSE;
            config->color_space = MACDRV_METAL_COLOR_SPACE_EXTENDED_LINEAR_SRGB;
            config->pixel_format = MACDRV_METAL_PIXEL_FORMAT_RGBA16_FLOAT;
            return TRUE;

        case VK_COLOR_SPACE_HDR10_ST2084_EXT:
            if (format != VK_FORMAT_A2B10G10R10_UNORM_PACK32) return FALSE;
            config->color_space = MACDRV_METAL_COLOR_SPACE_HDR10_PQ_BT2020;
            config->pixel_format = MACDRV_METAL_PIXEL_FORMAT_RGB10A2_UNORM;
            return TRUE;

        default:
            return FALSE;
    }
}

static VkResult macdrv_color_result_to_vk(enum macdrv_metal_color_result result, BOOL validation)
{
    switch (result)
    {
        case MACDRV_METAL_COLOR_SUCCESS:
            return VK_SUCCESS;
        case MACDRV_METAL_COLOR_UNSUPPORTED:
            return validation ? VK_ERROR_OUT_OF_DATE_KHR : VK_ERROR_INITIALIZATION_FAILED;
        default:
            return VK_ERROR_INITIALIZATION_FAILED;
    }
}

static VkBool32 macdrv_surface_supports_format(struct client_surface *client, VkFormat format,
        VkColorSpaceKHR color_space)
{
    struct macdrv_metal_color_config config;

    if (!macdrv_color_config_from_vk(format, color_space, &config)) return VK_FALSE;
    return macdrv_client_surface_supports_color_config(impl_from_client_surface(client), &config)
            == MACDRV_METAL_COLOR_SUCCESS;
}

static VkResult macdrv_surface_configure(struct client_surface *client, const VkSurfaceFormatKHR *format)
{
    struct macdrv_metal_color_config config;

    if (!format)
    {
        memset(&config, 0, sizeof(config));
        config.color_space = MACDRV_METAL_COLOR_SPACE_SRGB;
        config.pixel_format = MACDRV_METAL_PIXEL_FORMAT_PROVIDER;
    }
    else if (!macdrv_color_config_from_vk(format->format, format->colorSpace, &config))
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return macdrv_color_result_to_vk(macdrv_client_surface_set_color_config(
            impl_from_client_surface(client), &config), FALSE);
}

static VkResult macdrv_surface_validate(struct client_surface *client, const VkSurfaceFormatKHR *format)
{
    struct macdrv_metal_color_config config;

    if (!format || !macdrv_color_config_from_vk(format->format, format->colorSpace, &config))
        return VK_ERROR_OUT_OF_DATE_KHR;
    return macdrv_color_result_to_vk(macdrv_client_surface_validate_color_config(
            impl_from_client_surface(client), &config), TRUE);
}

static BOOL macdrv_vk_chromaticity_to_uint16(const VkXYColorEXT *value, uint16_t result[2])
{
    double x, y;

    if (!isfinite(value->x) || !isfinite(value->y) || value->x < 0.0f || value->y < 0.0f
            || value->x > 1.0f || value->y > 1.0f || value->x + value->y > 1.0f)
        return FALSE;
    x = value->x * 50000.0;
    y = value->y * 50000.0;
    if (x > UINT16_MAX || y > UINT16_MAX) return FALSE;
    result[0] = lround(x);
    result[1] = lround(y);
    return TRUE;
}

static VkResult macdrv_surface_set_hdr_metadata(struct client_surface *client,
        const VkHdrMetadataEXT *metadata)
{
    struct macdrv_hdr10_metadata hdr10;

    if (!metadata)
        return macdrv_color_result_to_vk(macdrv_client_surface_set_hdr10_metadata(
                impl_from_client_surface(client), NULL), FALSE);

    memset(&hdr10, 0, sizeof(hdr10));
    if (metadata->sType != VK_STRUCTURE_TYPE_HDR_METADATA_EXT
            || !macdrv_vk_chromaticity_to_uint16(&metadata->displayPrimaryRed, hdr10.red_primary)
            || !macdrv_vk_chromaticity_to_uint16(&metadata->displayPrimaryGreen, hdr10.green_primary)
            || !macdrv_vk_chromaticity_to_uint16(&metadata->displayPrimaryBlue, hdr10.blue_primary)
            || !macdrv_vk_chromaticity_to_uint16(&metadata->whitePoint, hdr10.white_point)
            || !isfinite(metadata->maxLuminance) || metadata->maxLuminance < 0.0f
            || metadata->maxLuminance > 10000.0f
            || !isfinite(metadata->minLuminance) || metadata->minLuminance < 0.0f
            || (metadata->maxLuminance && metadata->minLuminance > metadata->maxLuminance)
            || !isfinite(metadata->maxContentLightLevel) || metadata->maxContentLightLevel < 0.0f
            || metadata->maxContentLightLevel > 10000.0f
            || !isfinite(metadata->maxFrameAverageLightLevel)
            || metadata->maxFrameAverageLightLevel < 0.0f
            || metadata->maxFrameAverageLightLevel > 10000.0f
            || (metadata->maxContentLightLevel && metadata->maxFrameAverageLightLevel >
                    metadata->maxContentLightLevel))
        return VK_ERROR_INITIALIZATION_FAILED;

    hdr10.max_mastering_luminance = lround(metadata->maxLuminance * 10000.0);
    hdr10.min_mastering_luminance = lround(metadata->minLuminance * 10000.0);
    hdr10.max_content_light_level = lround(metadata->maxContentLightLevel);
    hdr10.max_frame_average_light_level = lround(metadata->maxFrameAverageLightLevel);
    return macdrv_color_result_to_vk(macdrv_client_surface_set_hdr10_metadata(
            impl_from_client_surface(client), &hdr10), FALSE);
}

static VkResult macdrv_vulkan_surface_create(struct client_surface *client, const struct vulkan_instance *instance, VkSurfaceKHR *handle)
{
    VkResult res;
    struct macdrv_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s %p %p\n", debugstr_client_surface(client), instance, handle);

    if (!macdrv_client_surface_acquire_metal_swapchain(surface)) return VK_ERROR_INCOMPATIBLE_DRIVER;

    if (instance->p_vkCreateMetalSurfaceEXT)
    {
        VkMetalSurfaceCreateInfoEXT create_info_host;
        create_info_host.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        create_info_host.pNext = NULL;
        create_info_host.flags = 0; /* reserved */
        create_info_host.pLayer = macdrv_swapchain_get_layer(surface->metal_swapchain);

        if ((res = instance->p_vkCreateMetalSurfaceEXT(instance->host.instance, &create_info_host, NULL /* allocator */, handle))) return res;
    }
    else
    {
        VkMacOSSurfaceCreateInfoMVK create_info_host;
        create_info_host.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
        create_info_host.pNext = NULL;
        create_info_host.flags = 0; /* reserved */
        create_info_host.pView = macdrv_swapchain_get_layer(surface->metal_swapchain);

        if ((res = instance->p_vkCreateMacOSSurfaceMVK(instance->host.instance, &create_info_host, NULL /* allocator */, handle))) return res;
    }

    TRACE("Created surface=0x%s\n", wine_dbgstr_longlong(*handle));
    return VK_SUCCESS;
}

static VkBool32 macdrv_get_physical_device_presentation_support(struct vulkan_physical_device *physical_device,
        uint32_t index)
{
    TRACE("%p %u\n", physical_device, index);

    return VK_TRUE;
}

static BOOL use_VK_EXT_metal_surface;

static void macdrv_map_instance_extensions(struct vulkan_instance_extensions *extensions)
{
    if (use_VK_EXT_metal_surface)
    {
        if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_metal_surface = 1;
        if (extensions->has_VK_EXT_metal_surface) extensions->has_VK_KHR_win32_surface = 1;
    }
    else
    {
        if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_MVK_macos_surface = 1;
        if (extensions->has_VK_MVK_macos_surface) extensions->has_VK_KHR_win32_surface = 1;
    }
}

static void macdrv_map_device_extensions(struct vulkan_device_extensions *extensions)
{
}

static const struct vulkan_driver_funcs macdrv_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = macdrv_vulkan_surface_create,
    .p_surface_supports_format = macdrv_surface_supports_format,
    .p_surface_configure = macdrv_surface_configure,
    .p_surface_validate = macdrv_surface_validate,
    .p_surface_set_hdr_metadata = macdrv_surface_set_hdr_metadata,
    .p_get_physical_device_presentation_support = macdrv_get_physical_device_presentation_support,
    .p_map_instance_extensions = macdrv_map_instance_extensions,
    .p_map_device_extensions = macdrv_map_device_extensions,
};

UINT macdrv_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    use_VK_EXT_metal_surface = !!dlsym(vulkan_handle, "vkCreateMetalSurfaceEXT");

    *driver_funcs = &macdrv_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
