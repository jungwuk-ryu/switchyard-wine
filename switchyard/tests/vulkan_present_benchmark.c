/*
 * Finite Win32 Vulkan WSI present benchmark.
 *
 * This intentionally resolves vulkan-1.dll at run time so the same PE binary
 * can measure different Wine Vulkan providers without an SDK import library.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include "wine/vulkan.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_INSTANCE_EXTENSIONS 128u
#define MAX_PHYSICAL_DEVICES 32u
#define MAX_DEVICE_EXTENSIONS 128u
#define MAX_QUEUE_FAMILIES 64u
#define MAX_SURFACE_FORMATS 64u
#define MAX_PRESENT_MODES 32u
#define MAX_SWAPCHAIN_IMAGES 16u
#define FRAME_CONTEXT_COUNT 2u
#define MAX_WARMUP_FRAMES 10000u
#define MAX_MEASURED_FRAMES 100000u
#define MAX_RECREATIONS 64u
#define WAIT_TIMEOUT_NS UINT64_C(5000000000)
#define INVALID_SAMPLE UINT32_MAX
#define PORTABILITY_SUBSET_NAME "VK_KHR_portability_subset"

struct vulkan_api
{
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdClearColorImage CmdClearColorImage;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkCreateQueryPool CreateQueryPool;
    PFN_vkDestroyQueryPool DestroyQueryPool;
    PFN_vkCmdResetQueryPool CmdResetQueryPool;
    PFN_vkCmdWriteTimestamp CmdWriteTimestamp;
    PFN_vkGetQueryPoolResults GetQueryPoolResults;
};

struct frame_context
{
    VkCommandBuffer command_buffer;
    VkSemaphore image_available;
    VkFence fence;
    uint32_t query_target;
    int query_pending;
};

struct benchmark
{
    HMODULE vulkan_module;
    HWND window;
    HINSTANCE app_instance;
    struct vulkan_api vk;

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceProperties properties;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t timestamp_valid_bits;

    VkSwapchainKHR swapchain;
    VkImage images[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore render_finished[MAX_SWAPCHAIN_IMAGES];
    VkFence image_fences[MAX_SWAPCHAIN_IMAGES];
    uint32_t image_count;
    VkFormat format;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;

    VkCommandPool command_pool;
    struct frame_context frames[FRAME_CONTEXT_COUNT];
    VkQueryPool query_pool;
    int timestamps_available;

    uint64_t *present_ns;
    uint64_t *frame_ns;
    uint64_t *gpu_ns;
    unsigned char *gpu_valid;
    uint32_t measured_capacity;
};

struct surface_config
{
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR format;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;
    VkCompositeAlphaFlagBitsKHR composite_alpha;
    uint32_t image_count;
};

struct statistics
{
    double mean;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
};

static LRESULT CALLBACK benchmark_window_proc(HWND window, UINT message,
        WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_CLOSE:
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

static int create_benchmark_window(struct benchmark *benchmark)
{
    static const WCHAR class_name[] = L"SwitchyardVulkanPresentBenchmark";
    WNDCLASSEXW window_class;
    const DWORD style = WS_CAPTION | WS_SYSMENU;
    RECT rect = {0, 0, 320, 240};

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = benchmark_window_proc;
    window_class.hInstance = benchmark->app_instance;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        fprintf(stderr, "RegisterClassExW failed: %lu\n", GetLastError());
        return 0;
    }

    if (!AdjustWindowRect(&rect, style, FALSE))
    {
        fprintf(stderr, "AdjustWindowRect failed: %lu\n", GetLastError());
        return 0;
    }
    benchmark->window = CreateWindowExW(0, class_name, L"Switchyard Vulkan SDR present benchmark",
            style, 64, 64, rect.right - rect.left, rect.bottom - rect.top,
            NULL, NULL, benchmark->app_instance, NULL);
    if (!benchmark->window)
    {
        fprintf(stderr, "CreateWindowExW failed: %lu\n", GetLastError());
        return 0;
    }
    ShowWindow(benchmark->window, SW_SHOWNORMAL);
    UpdateWindow(benchmark->window);
    return 1;
}

static int pump_window_messages(void)
{
    MSG message;

    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT) return 0;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 1;
}

static int has_extension(const VkExtensionProperties *extensions, uint32_t count,
        const char *name)
{
    uint32_t i;

    for (i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return 1;
    return 0;
}

static int load_global_api(struct benchmark *benchmark)
{
    struct vulkan_api *vk = &benchmark->vk;
    union
    {
        FARPROC generic;
        PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    } proc;

    benchmark->vulkan_module = LoadLibraryA("vulkan-1.dll");
    if (!benchmark->vulkan_module)
    {
        fprintf(stderr, "LoadLibraryA(vulkan-1.dll) failed: %lu\n", GetLastError());
        return 0;
    }
    proc.generic = GetProcAddress(benchmark->vulkan_module, "vkGetInstanceProcAddr");
    vk->GetInstanceProcAddr = proc.get_instance_proc_addr;
    if (!vk->GetInstanceProcAddr)
    {
        fprintf(stderr, "vulkan-1.dll does not export vkGetInstanceProcAddr\n");
        return 0;
    }
    vk->EnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)
            vk->GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    vk->CreateInstance = (PFN_vkCreateInstance)
            vk->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!vk->EnumerateInstanceExtensionProperties || !vk->CreateInstance)
    {
        fprintf(stderr, "Vulkan loader is missing required global entry points\n");
        return 0;
    }
    return 1;
}

#define LOAD_INSTANCE_FUNCTION(name) do { \
    vk->name = (PFN_vk##name)vk->GetInstanceProcAddr(benchmark->instance, "vk" #name); \
    if (!vk->name) { fprintf(stderr, "Vulkan instance is missing vk%s\n", #name); return 0; } \
} while (0)

static int load_instance_api(struct benchmark *benchmark)
{
    struct vulkan_api *vk = &benchmark->vk;

    LOAD_INSTANCE_FUNCTION(DestroyInstance);
    LOAD_INSTANCE_FUNCTION(EnumeratePhysicalDevices);
    LOAD_INSTANCE_FUNCTION(GetPhysicalDeviceProperties);
    LOAD_INSTANCE_FUNCTION(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE_FUNCTION(EnumerateDeviceExtensionProperties);
    LOAD_INSTANCE_FUNCTION(GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE_FUNCTION(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INSTANCE_FUNCTION(GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE_FUNCTION(GetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_INSTANCE_FUNCTION(CreateWin32SurfaceKHR);
    LOAD_INSTANCE_FUNCTION(DestroySurfaceKHR);
    LOAD_INSTANCE_FUNCTION(CreateDevice);
    LOAD_INSTANCE_FUNCTION(GetDeviceProcAddr);
    return 1;
}

#undef LOAD_INSTANCE_FUNCTION

#define LOAD_DEVICE_FUNCTION(name) do { \
    vk->name = (PFN_vk##name)vk->GetDeviceProcAddr(benchmark->device, "vk" #name); \
    if (!vk->name) { fprintf(stderr, "Vulkan device is missing vk%s\n", #name); return 0; } \
} while (0)

static int load_device_api(struct benchmark *benchmark)
{
    struct vulkan_api *vk = &benchmark->vk;

    LOAD_DEVICE_FUNCTION(DestroyDevice);
    LOAD_DEVICE_FUNCTION(GetDeviceQueue);
    LOAD_DEVICE_FUNCTION(CreateSwapchainKHR);
    LOAD_DEVICE_FUNCTION(DestroySwapchainKHR);
    LOAD_DEVICE_FUNCTION(GetSwapchainImagesKHR);
    LOAD_DEVICE_FUNCTION(CreateCommandPool);
    LOAD_DEVICE_FUNCTION(DestroyCommandPool);
    LOAD_DEVICE_FUNCTION(AllocateCommandBuffers);
    LOAD_DEVICE_FUNCTION(ResetCommandBuffer);
    LOAD_DEVICE_FUNCTION(BeginCommandBuffer);
    LOAD_DEVICE_FUNCTION(EndCommandBuffer);
    LOAD_DEVICE_FUNCTION(CmdPipelineBarrier);
    LOAD_DEVICE_FUNCTION(CmdClearColorImage);
    LOAD_DEVICE_FUNCTION(CreateSemaphore);
    LOAD_DEVICE_FUNCTION(DestroySemaphore);
    LOAD_DEVICE_FUNCTION(CreateFence);
    LOAD_DEVICE_FUNCTION(DestroyFence);
    LOAD_DEVICE_FUNCTION(WaitForFences);
    LOAD_DEVICE_FUNCTION(ResetFences);
    LOAD_DEVICE_FUNCTION(AcquireNextImageKHR);
    LOAD_DEVICE_FUNCTION(QueueSubmit);
    LOAD_DEVICE_FUNCTION(QueuePresentKHR);
    LOAD_DEVICE_FUNCTION(DeviceWaitIdle);
    LOAD_DEVICE_FUNCTION(CreateQueryPool);
    LOAD_DEVICE_FUNCTION(DestroyQueryPool);
    LOAD_DEVICE_FUNCTION(CmdResetQueryPool);
    LOAD_DEVICE_FUNCTION(CmdWriteTimestamp);
    LOAD_DEVICE_FUNCTION(GetQueryPoolResults);
    return 1;
}

#undef LOAD_DEVICE_FUNCTION

static int create_instance(struct benchmark *benchmark)
{
    VkExtensionProperties extensions[MAX_INSTANCE_EXTENSIONS];
    const char *enabled_extensions[3];
    VkApplicationInfo application_info;
    VkInstanceCreateInfo create_info;
    uint32_t extension_count = 0, available_count = 0;
    VkResult result;

    result = benchmark->vk.EnumerateInstanceExtensionProperties(NULL, &available_count, NULL);
    if (result != VK_SUCCESS || available_count > MAX_INSTANCE_EXTENSIONS)
    {
        fprintf(stderr, "Invalid Vulkan instance extension count %u (result %d)\n",
                available_count, result);
        return 0;
    }
    if (available_count)
    {
        uint32_t capacity = available_count;
        result = benchmark->vk.EnumerateInstanceExtensionProperties(NULL, &capacity, extensions);
        if (result != VK_SUCCESS || capacity > available_count)
        {
            fprintf(stderr, "Enumerating Vulkan instance extensions failed: %d\n", result);
            return 0;
        }
        available_count = capacity;
    }
    if (!has_extension(extensions, available_count, VK_KHR_SURFACE_EXTENSION_NAME) ||
            !has_extension(extensions, available_count, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
    {
        fprintf(stderr, "Vulkan loader does not advertise Win32 surface support\n");
        return 0;
    }

    enabled_extensions[extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
    enabled_extensions[extension_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;

    memset(&application_info, 0, sizeof(application_info));
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Switchyard Vulkan present benchmark";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "none";
    application_info.engineVersion = 1;
    application_info.apiVersion = VK_API_VERSION_1_0;

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application_info;
    create_info.enabledExtensionCount = extension_count;
    create_info.ppEnabledExtensionNames = enabled_extensions;
    if (has_extension(extensions, available_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        enabled_extensions[extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        create_info.enabledExtensionCount = extension_count;
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    result = benchmark->vk.CreateInstance(&create_info, NULL, &benchmark->instance);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        return 0;
    }
    return load_instance_api(benchmark);
}

static int create_surface(struct benchmark *benchmark)
{
    VkWin32SurfaceCreateInfoKHR create_info;
    VkResult result;

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create_info.hinstance = benchmark->app_instance;
    create_info.hwnd = benchmark->window;
    result = benchmark->vk.CreateWin32SurfaceKHR(benchmark->instance, &create_info,
            NULL, &benchmark->surface);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateWin32SurfaceKHR failed: %d\n", result);
        return 0;
    }
    return 1;
}

static int enumerate_device_extensions(struct benchmark *benchmark, VkPhysicalDevice device,
        VkExtensionProperties *extensions, uint32_t *extension_count)
{
    uint32_t count = 0, capacity;
    VkResult result;

    result = benchmark->vk.EnumerateDeviceExtensionProperties(device, NULL, &count, NULL);
    if (result != VK_SUCCESS || count > MAX_DEVICE_EXTENSIONS)
        return 0;
    capacity = count;
    if (count)
    {
        result = benchmark->vk.EnumerateDeviceExtensionProperties(device, NULL,
                &capacity, extensions);
        if (result != VK_SUCCESS || capacity > count) return 0;
    }
    *extension_count = capacity;
    return 1;
}

static int device_has_srgb_surface_format(struct benchmark *benchmark,
        VkPhysicalDevice device)
{
    VkSurfaceFormatKHR formats[MAX_SURFACE_FORMATS];
    uint32_t count = 0, capacity, i;
    VkResult result;

    result = benchmark->vk.GetPhysicalDeviceSurfaceFormatsKHR(device,
            benchmark->surface, &count, NULL);
    if (result != VK_SUCCESS || !count || count > MAX_SURFACE_FORMATS) return 0;
    capacity = count;
    result = benchmark->vk.GetPhysicalDeviceSurfaceFormatsKHR(device,
            benchmark->surface, &capacity, formats);
    if (result != VK_SUCCESS || capacity > count) return 0;
    for (i = 0; i < capacity; ++i)
    {
        if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
                 formats[i].format == VK_FORMAT_R8G8B8A8_SRGB))
            return 1;
    }
    return 0;
}

static int device_type_score(VkPhysicalDeviceType type)
{
    switch (type)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 500;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 400;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 300;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 200;
        default: return 100;
    }
}

static int select_physical_device(struct benchmark *benchmark)
{
    VkPhysicalDevice devices[MAX_PHYSICAL_DEVICES];
    VkExtensionProperties extensions[MAX_DEVICE_EXTENSIONS];
    VkQueueFamilyProperties queues[MAX_QUEUE_FAMILIES];
    VkPhysicalDevice best_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties best_properties;
    uint32_t best_queue = 0, best_timestamp_bits = 0;
    uint32_t device_count = 0, capacity, i;
    int best_score = -1;
    VkResult result;

    result = benchmark->vk.EnumeratePhysicalDevices(benchmark->instance, &device_count, NULL);
    if (result != VK_SUCCESS || !device_count || device_count > MAX_PHYSICAL_DEVICES)
    {
        fprintf(stderr, "Invalid Vulkan physical device count %u (result %d)\n",
                device_count, result);
        return 0;
    }
    capacity = device_count;
    result = benchmark->vk.EnumeratePhysicalDevices(benchmark->instance, &capacity, devices);
    if (result != VK_SUCCESS || capacity > device_count)
    {
        fprintf(stderr, "Enumerating Vulkan physical devices failed: %d\n", result);
        return 0;
    }

    for (i = 0; i < capacity; ++i)
    {
        VkPhysicalDeviceProperties properties;
        uint32_t extension_count, queue_count = 0, j;
        uint32_t candidate_queue = UINT32_MAX;
        int score;

        if (!enumerate_device_extensions(benchmark, devices[i], extensions, &extension_count) ||
                !has_extension(extensions, extension_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            continue;
        benchmark->vk.GetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, NULL);
        if (!queue_count || queue_count > MAX_QUEUE_FAMILIES) continue;
        {
            uint32_t queue_capacity = queue_count;
            benchmark->vk.GetPhysicalDeviceQueueFamilyProperties(devices[i],
                    &queue_capacity, queues);
            if (queue_capacity > queue_count) continue;
            queue_count = queue_capacity;
        }
        for (j = 0; j < queue_count; ++j)
        {
            VkBool32 present_supported = VK_FALSE;
            if (!(queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            result = benchmark->vk.GetPhysicalDeviceSurfaceSupportKHR(devices[i], j,
                    benchmark->surface, &present_supported);
            if (result == VK_SUCCESS && present_supported)
            {
                candidate_queue = j;
                break;
            }
        }
        if (candidate_queue == UINT32_MAX ||
                !device_has_srgb_surface_format(benchmark, devices[i]))
            continue;

        benchmark->vk.GetPhysicalDeviceProperties(devices[i], &properties);
        score = device_type_score(properties.deviceType);
        if (score > best_score || (score == best_score && best_device != VK_NULL_HANDLE &&
                strcmp(properties.deviceName, best_properties.deviceName) < 0))
        {
            best_score = score;
            best_device = devices[i];
            best_properties = properties;
            best_queue = candidate_queue;
            best_timestamp_bits = queues[candidate_queue].timestampValidBits;
        }
    }

    if (best_device == VK_NULL_HANDLE)
    {
        fprintf(stderr, "No Vulkan graphics+present device exposes an enumerated BGRA8/RGBA8 sRGB format\n");
        return 0;
    }
    benchmark->physical_device = best_device;
    benchmark->properties = best_properties;
    benchmark->queue_family = best_queue;
    benchmark->timestamp_valid_bits = best_timestamp_bits;
    return 1;
}

static int create_device(struct benchmark *benchmark)
{
    VkExtensionProperties extensions[MAX_DEVICE_EXTENSIONS];
    const char *enabled_extensions[2];
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo create_info;
    uint32_t extension_count, enabled_count = 0;
    float priority = 1.0f;
    VkResult result;

    if (!enumerate_device_extensions(benchmark, benchmark->physical_device,
            extensions, &extension_count))
    {
        fprintf(stderr, "Enumerating Vulkan device extensions failed\n");
        return 0;
    }
    enabled_extensions[enabled_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (has_extension(extensions, extension_count, PORTABILITY_SUBSET_NAME))
        enabled_extensions[enabled_count++] = PORTABILITY_SUBSET_NAME;

    memset(&queue_info, 0, sizeof(queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = benchmark->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount = enabled_count;
    create_info.ppEnabledExtensionNames = enabled_extensions;

    result = benchmark->vk.CreateDevice(benchmark->physical_device, &create_info,
            NULL, &benchmark->device);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateDevice failed: %d\n", result);
        return 0;
    }
    if (!load_device_api(benchmark)) return 0;
    benchmark->vk.GetDeviceQueue(benchmark->device, benchmark->queue_family, 0,
            &benchmark->queue);
    if (benchmark->queue == VK_NULL_HANDLE)
    {
        fprintf(stderr, "vkGetDeviceQueue returned a null queue\n");
        return 0;
    }
    return 1;
}

static uint32_t clamp_u32(uint32_t value, uint32_t minimum, uint32_t maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int choose_surface_config(struct benchmark *benchmark,
        struct surface_config *config)
{
    VkSurfaceFormatKHR formats[MAX_SURFACE_FORMATS];
    VkPresentModeKHR modes[MAX_PRESENT_MODES];
    VkResult result;
    uint32_t count = 0, capacity, i;
    RECT client_rect;
    int found = 0;

    memset(config, 0, sizeof(*config));
    result = benchmark->vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(
            benchmark->physical_device, benchmark->surface, &config->capabilities);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %d\n", result);
        return 0;
    }
    if (!(config->capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
        fprintf(stderr, "Vulkan surface images do not support transfer-destination clears\n");
        return 0;
    }

    result = benchmark->vk.GetPhysicalDeviceSurfaceFormatsKHR(benchmark->physical_device,
            benchmark->surface, &count, NULL);
    if (result != VK_SUCCESS || !count || count > MAX_SURFACE_FORMATS)
    {
        fprintf(stderr, "Invalid Vulkan surface format count %u (result %d)\n", count, result);
        return 0;
    }
    capacity = count;
    result = benchmark->vk.GetPhysicalDeviceSurfaceFormatsKHR(benchmark->physical_device,
            benchmark->surface, &capacity, formats);
    if (result != VK_SUCCESS || capacity > count) return 0;
    for (i = 0; i < capacity; ++i)
    {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            config->format = formats[i];
            found = 1;
            break;
        }
    }
    if (!found)
    {
        for (i = 0; i < capacity; ++i)
        {
            if (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB &&
                    formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                config->format = formats[i];
                found = 1;
                break;
            }
        }
    }
    if (!found)
    {
        fprintf(stderr, "No exact enumerated BGRA8/RGBA8 sRGB surface format remains available\n");
        return 0;
    }

    count = 0;
    result = benchmark->vk.GetPhysicalDeviceSurfacePresentModesKHR(
            benchmark->physical_device, benchmark->surface, &count, NULL);
    if (result != VK_SUCCESS || !count || count > MAX_PRESENT_MODES)
    {
        fprintf(stderr, "Invalid Vulkan present mode count %u (result %d)\n", count, result);
        return 0;
    }
    capacity = count;
    result = benchmark->vk.GetPhysicalDeviceSurfacePresentModesKHR(
            benchmark->physical_device, benchmark->surface, &capacity, modes);
    if (result != VK_SUCCESS || capacity > count) return 0;
    config->present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (i = 0; i < capacity; ++i)
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            config->present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (config->present_mode != VK_PRESENT_MODE_IMMEDIATE_KHR)
        for (i = 0; i < capacity; ++i)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                config->present_mode = VK_PRESENT_MODE_MAILBOX_KHR;

    if (config->capabilities.currentExtent.width != UINT32_MAX)
        config->extent = config->capabilities.currentExtent;
    else
    {
        if (!GetClientRect(benchmark->window, &client_rect))
        {
            fprintf(stderr, "GetClientRect failed: %lu\n", GetLastError());
            return 0;
        }
        config->extent.width = clamp_u32((uint32_t)(client_rect.right - client_rect.left),
                config->capabilities.minImageExtent.width,
                config->capabilities.maxImageExtent.width);
        config->extent.height = clamp_u32((uint32_t)(client_rect.bottom - client_rect.top),
                config->capabilities.minImageExtent.height,
                config->capabilities.maxImageExtent.height);
    }
    if (!config->extent.width || !config->extent.height)
    {
        fprintf(stderr, "Vulkan surface has a zero-sized extent\n");
        return 0;
    }

    config->image_count = config->capabilities.minImageCount;
    if (config->image_count < UINT32_MAX) ++config->image_count;
    if (config->capabilities.maxImageCount &&
            config->image_count > config->capabilities.maxImageCount)
        config->image_count = config->capabilities.maxImageCount;
    if (config->image_count > MAX_SWAPCHAIN_IMAGES)
        config->image_count = MAX_SWAPCHAIN_IMAGES;
    if (config->image_count < config->capabilities.minImageCount)
    {
        fprintf(stderr, "Vulkan surface requires more than %u swapchain images\n",
                MAX_SWAPCHAIN_IMAGES);
        return 0;
    }

    if (config->capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        config->composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    else if (config->capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        config->composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    else if (config->capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
        config->composite_alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    else if (config->capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        config->composite_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    else
    {
        fprintf(stderr, "Vulkan surface has no supported composite-alpha mode\n");
        return 0;
    }
    return 1;
}

static void destroy_swapchain_objects(struct benchmark *benchmark)
{
    uint32_t i;

    if (!benchmark->device) return;
    for (i = 0; i < benchmark->image_count; ++i)
    {
        if (benchmark->render_finished[i])
            benchmark->vk.DestroySemaphore(benchmark->device,
                    benchmark->render_finished[i], NULL);
        benchmark->render_finished[i] = VK_NULL_HANDLE;
        benchmark->image_fences[i] = VK_NULL_HANDLE;
        benchmark->images[i] = VK_NULL_HANDLE;
    }
    benchmark->image_count = 0;
    if (benchmark->swapchain)
        benchmark->vk.DestroySwapchainKHR(benchmark->device, benchmark->swapchain, NULL);
    benchmark->swapchain = VK_NULL_HANDLE;
}

static int create_swapchain(struct benchmark *benchmark)
{
    struct surface_config config;
    VkSwapchainCreateInfoKHR create_info;
    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    VkImage new_images[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore new_semaphores[MAX_SWAPCHAIN_IMAGES];
    VkSemaphoreCreateInfo semaphore_info;
    uint32_t image_count = 0, capacity, i;
    VkResult result;

    memset(new_images, 0, sizeof(new_images));
    memset(new_semaphores, 0, sizeof(new_semaphores));
    if (!choose_surface_config(benchmark, &config)) return 0;

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = benchmark->surface;
    create_info.minImageCount = config.image_count;
    create_info.imageFormat = config.format.format;
    create_info.imageColorSpace = config.format.colorSpace;
    create_info.imageExtent = config.extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = config.capabilities.currentTransform;
    create_info.compositeAlpha = config.composite_alpha;
    create_info.presentMode = config.present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = benchmark->swapchain;
    result = benchmark->vk.CreateSwapchainKHR(benchmark->device, &create_info, NULL,
            &new_swapchain);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateSwapchainKHR failed: %d\n", result);
        return 0;
    }

    result = benchmark->vk.GetSwapchainImagesKHR(benchmark->device, new_swapchain,
            &image_count, NULL);
    if (result != VK_SUCCESS || !image_count || image_count > MAX_SWAPCHAIN_IMAGES)
    {
        fprintf(stderr, "Invalid Vulkan swapchain image count %u (result %d)\n",
                image_count, result);
        goto fail;
    }
    capacity = image_count;
    result = benchmark->vk.GetSwapchainImagesKHR(benchmark->device, new_swapchain,
            &capacity, new_images);
    if (result != VK_SUCCESS || capacity != image_count)
    {
        fprintf(stderr, "Enumerating Vulkan swapchain images failed: %d\n", result);
        goto fail;
    }

    memset(&semaphore_info, 0, sizeof(semaphore_info));
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < image_count; ++i)
    {
        result = benchmark->vk.CreateSemaphore(benchmark->device, &semaphore_info,
                NULL, &new_semaphores[i]);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "Creating present semaphore %u failed: %d\n", i, result);
            goto fail;
        }
    }

    destroy_swapchain_objects(benchmark);
    benchmark->swapchain = new_swapchain;
    benchmark->image_count = image_count;
    benchmark->format = config.format.format;
    benchmark->present_mode = config.present_mode;
    benchmark->extent = config.extent;
    for (i = 0; i < image_count; ++i)
    {
        benchmark->images[i] = new_images[i];
        benchmark->render_finished[i] = new_semaphores[i];
        benchmark->image_fences[i] = VK_NULL_HANDLE;
    }
    return 1;

fail:
    for (i = 0; i < image_count && i < MAX_SWAPCHAIN_IMAGES; ++i)
        if (new_semaphores[i])
            benchmark->vk.DestroySemaphore(benchmark->device, new_semaphores[i], NULL);
    if (new_swapchain)
        benchmark->vk.DestroySwapchainKHR(benchmark->device, new_swapchain, NULL);
    return 0;
}

static int create_command_and_sync_objects(struct benchmark *benchmark)
{
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo allocate_info;
    VkCommandBuffer command_buffers[FRAME_CONTEXT_COUNT];
    VkSemaphoreCreateInfo semaphore_info;
    VkFenceCreateInfo fence_info;
    VkQueryPoolCreateInfo query_info;
    uint32_t i;
    VkResult result;

    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = benchmark->queue_family;
    result = benchmark->vk.CreateCommandPool(benchmark->device, &pool_info, NULL,
            &benchmark->command_pool);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkCreateCommandPool failed: %d\n", result);
        return 0;
    }

    memset(&allocate_info, 0, sizeof(allocate_info));
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = benchmark->command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = FRAME_CONTEXT_COUNT;
    result = benchmark->vk.AllocateCommandBuffers(benchmark->device, &allocate_info,
            command_buffers);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkAllocateCommandBuffers failed: %d\n", result);
        return 0;
    }

    memset(&semaphore_info, 0, sizeof(semaphore_info));
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (i = 0; i < FRAME_CONTEXT_COUNT; ++i)
    {
        benchmark->frames[i].command_buffer = command_buffers[i];
        benchmark->frames[i].query_target = INVALID_SAMPLE;
        result = benchmark->vk.CreateSemaphore(benchmark->device, &semaphore_info,
                NULL, &benchmark->frames[i].image_available);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "vkCreateSemaphore failed: %d\n", result);
            return 0;
        }
        result = benchmark->vk.CreateFence(benchmark->device, &fence_info, NULL,
                &benchmark->frames[i].fence);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "vkCreateFence failed: %d\n", result);
            return 0;
        }
    }

    benchmark->timestamps_available = benchmark->timestamp_valid_bits > 0 &&
            benchmark->properties.limits.timestampPeriod > 0.0f;
    if (benchmark->timestamps_available)
    {
        memset(&query_info, 0, sizeof(query_info));
        query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = FRAME_CONTEXT_COUNT * 2;
        result = benchmark->vk.CreateQueryPool(benchmark->device, &query_info, NULL,
                &benchmark->query_pool);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "Timestamp query pool unavailable (result %d); continuing without GPU timing\n",
                    result);
            benchmark->timestamps_available = 0;
        }
    }
    return 1;
}

static int record_clear_commands(struct benchmark *benchmark, uint32_t frame_index,
        uint32_t image_index, uint64_t sequence)
{
    static const float colors[][4] =
    {
        {0.030f, 0.055f, 0.090f, 1.0f},
        {0.055f, 0.090f, 0.030f, 1.0f},
        {0.090f, 0.030f, 0.055f, 1.0f},
        {0.055f, 0.055f, 0.055f, 1.0f},
    };
    VkCommandBuffer command_buffer = benchmark->frames[frame_index].command_buffer;
    VkCommandBufferBeginInfo begin_info;
    VkImageMemoryBarrier barrier;
    VkImageSubresourceRange range;
    VkClearColorValue clear_color;
    VkResult result;

    result = benchmark->vk.ResetCommandBuffer(command_buffer, 0);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkResetCommandBuffer failed: %d\n", result);
        return 0;
    }
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = benchmark->vk.BeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkBeginCommandBuffer failed: %d\n", result);
        return 0;
    }

    memset(&range, 0, sizeof(range));
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = benchmark->images[image_index];
    barrier.subresourceRange = range;
    benchmark->vk.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    if (benchmark->timestamps_available)
    {
        benchmark->vk.CmdResetQueryPool(command_buffer, benchmark->query_pool,
                frame_index * 2, 2);
        benchmark->vk.CmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                benchmark->query_pool, frame_index * 2);
    }
    memcpy(clear_color.float32, colors[sequence % ARRAY_SIZE(colors)],
            sizeof(clear_color.float32));
    benchmark->vk.CmdClearColorImage(command_buffer, benchmark->images[image_index],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);
    if (benchmark->timestamps_available)
        benchmark->vk.CmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                benchmark->query_pool, frame_index * 2 + 1);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    benchmark->vk.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    result = benchmark->vk.EndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkEndCommandBuffer failed: %d\n", result);
        return 0;
    }
    return 1;
}

static int collect_frame_query(struct benchmark *benchmark, uint32_t frame_index)
{
    struct frame_context *frame = &benchmark->frames[frame_index];
    uint64_t timestamps[2], mask, ticks;
    VkResult result;

    if (!benchmark->timestamps_available || !frame->query_pending) return 1;
    result = benchmark->vk.GetQueryPoolResults(benchmark->device, benchmark->query_pool,
            frame_index * 2, 2, sizeof(timestamps), timestamps, sizeof(timestamps[0]),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkGetQueryPoolResults failed: %d\n", result);
        return 0;
    }
    frame->query_pending = 0;
    if (frame->query_target == INVALID_SAMPLE) return 1;
    if (frame->query_target >= benchmark->measured_capacity)
    {
        fprintf(stderr, "Internal timestamp sample index overflow\n");
        return 0;
    }
    if (benchmark->timestamp_valid_bits < 64)
    {
        mask = (UINT64_C(1) << benchmark->timestamp_valid_bits) - 1;
        ticks = (timestamps[1] - timestamps[0]) & mask;
    }
    else
        ticks = timestamps[1] - timestamps[0];
    benchmark->gpu_ns[frame->query_target] = (uint64_t)
            ((long double)ticks * benchmark->properties.limits.timestampPeriod + 0.5L);
    benchmark->gpu_valid[frame->query_target] = 1;
    return 1;
}

static int wait_for_frame(struct benchmark *benchmark, uint32_t frame_index)
{
    VkResult result = benchmark->vk.WaitForFences(benchmark->device, 1,
            &benchmark->frames[frame_index].fence, VK_TRUE, WAIT_TIMEOUT_NS);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "Waiting for frame fence failed or timed out: %d\n", result);
        return 0;
    }
    return collect_frame_query(benchmark, frame_index);
}

static int drain_queries(struct benchmark *benchmark)
{
    uint32_t i;

    for (i = 0; i < FRAME_CONTEXT_COUNT; ++i)
        if (!collect_frame_query(benchmark, i)) return 0;
    return 1;
}

static uint64_t qpc_delta_ns(LONGLONG start, LONGLONG end, LONGLONG frequency)
{
    return (uint64_t)(((long double)(end - start) * 1000000000.0L /
            (long double)frequency) + 0.5L);
}

static int recreate_swapchain(struct benchmark *benchmark)
{
    VkResult result = benchmark->vk.DeviceWaitIdle(benchmark->device);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "vkDeviceWaitIdle before swapchain recreation failed: %d\n", result);
        return 0;
    }
    if (!drain_queries(benchmark)) return 0;
    return create_swapchain(benchmark);
}

static int run_benchmark(struct benchmark *benchmark, uint32_t warmup_count,
        uint32_t measured_count)
{
    uint64_t successful = 0, target_count = (uint64_t)warmup_count + measured_count;
    uint32_t frame_index = 0, recreations = 0;
    LARGE_INTEGER frequency;

    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        fprintf(stderr, "QueryPerformanceFrequency failed\n");
        return 0;
    }

    while (successful < target_count)
    {
        struct frame_context *frame = &benchmark->frames[frame_index];
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit_info;
        VkPresentInfoKHR present_info;
        VkResult result, per_swapchain_result = VK_SUCCESS;
        LARGE_INTEGER frame_start, present_start, present_end, frame_end;
        uint32_t image_index;
        uint32_t sample_index = successful >= warmup_count ?
                (uint32_t)(successful - warmup_count) : INVALID_SAMPLE;
        int accepted_present = 0, needs_recreate = 0;

        if (!pump_window_messages())
        {
            fprintf(stderr, "Benchmark window message loop terminated unexpectedly\n");
            return 0;
        }
        QueryPerformanceCounter(&frame_start);
        if (!wait_for_frame(benchmark, frame_index)) return 0;

        result = benchmark->vk.AcquireNextImageKHR(benchmark->device,
                benchmark->swapchain, WAIT_TIMEOUT_NS, frame->image_available,
                VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            if (++recreations > MAX_RECREATIONS || !recreate_swapchain(benchmark))
                return 0;
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            fprintf(stderr, "vkAcquireNextImageKHR failed or timed out: %d\n", result);
            return 0;
        }
        if (image_index >= benchmark->image_count)
        {
            fprintf(stderr, "vkAcquireNextImageKHR returned out-of-range image %u\n", image_index);
            return 0;
        }
        if (result == VK_SUBOPTIMAL_KHR) needs_recreate = 1;

        if (benchmark->image_fences[image_index] &&
                benchmark->image_fences[image_index] != frame->fence)
        {
            result = benchmark->vk.WaitForFences(benchmark->device, 1,
                    &benchmark->image_fences[image_index], VK_TRUE, WAIT_TIMEOUT_NS);
            if (result != VK_SUCCESS)
            {
                fprintf(stderr, "Waiting for swapchain image fence failed: %d\n", result);
                return 0;
            }
        }
        if (!record_clear_commands(benchmark, frame_index, image_index, successful))
            return 0;

        result = benchmark->vk.ResetFences(benchmark->device, 1, &frame->fence);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "vkResetFences failed: %d\n", result);
            return 0;
        }
        memset(&submit_info, 0, sizeof(submit_info));
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame->image_available;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &frame->command_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &benchmark->render_finished[image_index];
        result = benchmark->vk.QueueSubmit(benchmark->queue, 1, &submit_info, frame->fence);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "vkQueueSubmit failed: %d\n", result);
            return 0;
        }
        benchmark->image_fences[image_index] = frame->fence;
        if (benchmark->timestamps_available)
        {
            frame->query_target = sample_index;
            frame->query_pending = 1;
        }

        memset(&present_info, 0, sizeof(present_info));
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &benchmark->render_finished[image_index];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &benchmark->swapchain;
        present_info.pImageIndices = &image_index;
        present_info.pResults = &per_swapchain_result;
        QueryPerformanceCounter(&present_start);
        result = benchmark->vk.QueuePresentKHR(benchmark->queue, &present_info);
        QueryPerformanceCounter(&present_end);
        QueryPerformanceCounter(&frame_end);

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            if (per_swapchain_result == VK_SUCCESS ||
                    per_swapchain_result == VK_SUBOPTIMAL_KHR)
                accepted_present = 1;
            else if (per_swapchain_result == VK_ERROR_OUT_OF_DATE_KHR)
                needs_recreate = 1;
            else
            {
                fprintf(stderr, "Per-swapchain present failed: %d\n", per_swapchain_result);
                return 0;
            }
        }
        else if (result == VK_ERROR_OUT_OF_DATE_KHR)
            needs_recreate = 1;
        else
        {
            fprintf(stderr, "vkQueuePresentKHR failed: %d\n", result);
            return 0;
        }
        if (result == VK_SUBOPTIMAL_KHR || per_swapchain_result == VK_SUBOPTIMAL_KHR)
            needs_recreate = 1;

        if (accepted_present)
        {
            if (sample_index != INVALID_SAMPLE)
            {
                benchmark->present_ns[sample_index] = qpc_delta_ns(
                        present_start.QuadPart, present_end.QuadPart, frequency.QuadPart);
                benchmark->frame_ns[sample_index] = qpc_delta_ns(
                        frame_start.QuadPart, frame_end.QuadPart, frequency.QuadPart);
            }
            ++successful;
            frame_index = (frame_index + 1) % FRAME_CONTEXT_COUNT;
        }
        else if (benchmark->timestamps_available)
            frame->query_target = INVALID_SAMPLE;

        if (needs_recreate)
        {
            if (++recreations > MAX_RECREATIONS || !recreate_swapchain(benchmark))
                return 0;
        }
    }

    {
        VkResult result = benchmark->vk.DeviceWaitIdle(benchmark->device);
        if (result != VK_SUCCESS)
        {
            fprintf(stderr, "vkDeviceWaitIdle after benchmark failed: %d\n", result);
            return 0;
        }
    }
    return drain_queries(benchmark);
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static struct statistics calculate_statistics(uint64_t *values, uint32_t count)
{
    struct statistics statistics;
    long double sum = 0.0L;
    uint32_t i;

    memset(&statistics, 0, sizeof(statistics));
    for (i = 0; i < count; ++i) sum += values[i];
    qsort(values, count, sizeof(values[0]), compare_u64);
    statistics.mean = (double)(sum / count);
    statistics.p50 = values[((uint64_t)50 * count + 99) / 100 - 1];
    statistics.p95 = values[((uint64_t)95 * count + 99) / 100 - 1];
    statistics.p99 = values[((uint64_t)99 * count + 99) / 100 - 1];
    return statistics;
}

static void print_json_string(const char *string)
{
    const unsigned char *cursor = (const unsigned char *)string;

    putchar('"');
    while (*cursor)
    {
        switch (*cursor)
        {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*cursor < 0x20) printf("\\u%04x", *cursor);
                else putchar(*cursor);
                break;
        }
        ++cursor;
    }
    putchar('"');
}

static const char *format_name(VkFormat format)
{
    if (format == VK_FORMAT_B8G8R8A8_SRGB) return "VK_FORMAT_B8G8R8A8_SRGB";
    if (format == VK_FORMAT_R8G8B8A8_SRGB) return "VK_FORMAT_R8G8B8A8_SRGB";
    return "unknown";
}

static const char *present_mode_name(VkPresentModeKHR mode)
{
    if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return "VK_PRESENT_MODE_IMMEDIATE_KHR";
    if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return "VK_PRESENT_MODE_MAILBOX_KHR";
    if (mode == VK_PRESENT_MODE_FIFO_KHR) return "VK_PRESENT_MODE_FIFO_KHR";
    return "other";
}

static void print_results(struct benchmark *benchmark, uint32_t measured_count)
{
    struct statistics present, frame, gpu;
    uint32_t gpu_count = 0, i;
    char provider[MAX_PATH];

    present = calculate_statistics(benchmark->present_ns, measured_count);
    frame = calculate_statistics(benchmark->frame_ns, measured_count);
    for (i = 0; i < measured_count; ++i)
        if (benchmark->gpu_valid[i])
            benchmark->gpu_ns[gpu_count++] = benchmark->gpu_ns[i];
    memset(&gpu, 0, sizeof(gpu));
    if (gpu_count) gpu = calculate_statistics(benchmark->gpu_ns, gpu_count);

    if (!GetModuleFileNameA(benchmark->vulkan_module, provider, ARRAY_SIZE(provider)))
        strcpy(provider, "vulkan-1.dll");
    provider[ARRAY_SIZE(provider) - 1] = 0;

    fputs("{\"provider\":", stdout);
    print_json_string(provider);
    fputs(",\"device\":", stdout);
    print_json_string(benchmark->properties.deviceName);
    fputs(",\"format\":", stdout);
    print_json_string(format_name(benchmark->format));
    fputs(",\"present_mode\":", stdout);
    print_json_string(present_mode_name(benchmark->present_mode));
    printf(",\"count\":%u", measured_count);
    printf(",\"present_cpu_ns_mean\":%.3f,\"present_cpu_ns_p50\":%" PRIu64
            ",\"present_cpu_ns_p95\":%" PRIu64 ",\"present_cpu_ns_p99\":%" PRIu64,
            present.mean, present.p50, present.p95, present.p99);
    printf(",\"full_frame_cpu_ns_mean\":%.3f,\"full_frame_cpu_ns_p50\":%" PRIu64
            ",\"full_frame_cpu_ns_p95\":%" PRIu64 ",\"full_frame_cpu_ns_p99\":%" PRIu64,
            frame.mean, frame.p50, frame.p95, frame.p99);
    if (gpu_count)
        printf(",\"gpu_clear_ns_available\":true,\"gpu_clear_ns_count\":%u"
                ",\"gpu_clear_ns_mean\":%.3f,\"gpu_clear_ns_p50\":%" PRIu64
                ",\"gpu_clear_ns_p95\":%" PRIu64 ",\"gpu_clear_ns_p99\":%" PRIu64,
                gpu_count, gpu.mean, gpu.p50, gpu.p95, gpu.p99);
    else
        fputs(",\"gpu_clear_ns_available\":false,\"gpu_clear_ns_count\":0"
                ",\"gpu_clear_ns_mean\":null,\"gpu_clear_ns_p50\":null"
                ",\"gpu_clear_ns_p95\":null,\"gpu_clear_ns_p99\":null", stdout);
    fputs("}\n", stdout);
}

static int parse_count(const char *text, uint32_t maximum, uint32_t *value)
{
    char *end;
    unsigned long long parsed;

    if (!text[0] || text[0] == '-') return 0;
    parsed = strtoull(text, &end, 10);
    if (*end || !parsed || parsed > maximum) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "usage: %s [--warmup N] [--frames N]\n", program);
}

static void cleanup_benchmark(struct benchmark *benchmark)
{
    uint32_t i;

    if (benchmark->device && benchmark->vk.DeviceWaitIdle)
        benchmark->vk.DeviceWaitIdle(benchmark->device);
    if (benchmark->device && benchmark->query_pool && benchmark->vk.DestroyQueryPool)
        benchmark->vk.DestroyQueryPool(benchmark->device, benchmark->query_pool, NULL);
    if (benchmark->device && benchmark->vk.DestroySemaphore && benchmark->vk.DestroyFence)
    {
        for (i = 0; i < FRAME_CONTEXT_COUNT; ++i)
        {
            if (benchmark->frames[i].image_available)
                benchmark->vk.DestroySemaphore(benchmark->device,
                        benchmark->frames[i].image_available, NULL);
            if (benchmark->frames[i].fence)
                benchmark->vk.DestroyFence(benchmark->device,
                        benchmark->frames[i].fence, NULL);
        }
    }
    if (benchmark->device && benchmark->vk.DestroySwapchainKHR &&
            benchmark->vk.DestroySemaphore)
        destroy_swapchain_objects(benchmark);
    if (benchmark->device && benchmark->command_pool && benchmark->vk.DestroyCommandPool)
        benchmark->vk.DestroyCommandPool(benchmark->device, benchmark->command_pool, NULL);
    if (benchmark->device && benchmark->vk.DestroyDevice)
        benchmark->vk.DestroyDevice(benchmark->device, NULL);
    if (benchmark->instance && benchmark->surface && benchmark->vk.DestroySurfaceKHR)
        benchmark->vk.DestroySurfaceKHR(benchmark->instance, benchmark->surface, NULL);
    if (benchmark->instance && benchmark->vk.DestroyInstance)
        benchmark->vk.DestroyInstance(benchmark->instance, NULL);
    if (benchmark->window) DestroyWindow(benchmark->window);
    if (benchmark->vulkan_module) FreeLibrary(benchmark->vulkan_module);
    free(benchmark->present_ns);
    free(benchmark->frame_ns);
    free(benchmark->gpu_ns);
    free(benchmark->gpu_valid);
}

int main(int argc, char **argv)
{
    struct benchmark benchmark;
    uint32_t warmup_count = 600, measured_count = 12000;
    int i, success = 0;

    memset(&benchmark, 0, sizeof(benchmark));
    for (i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--warmup") && i + 1 < argc)
        {
            if (!parse_count(argv[++i], MAX_WARMUP_FRAMES, &warmup_count))
            {
                fprintf(stderr, "Invalid --warmup count (1..%u)\n", MAX_WARMUP_FRAMES);
                goto done;
            }
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
        {
            if (!parse_count(argv[++i], MAX_MEASURED_FRAMES, &measured_count))
            {
                fprintf(stderr, "Invalid --frames count (1..%u)\n", MAX_MEASURED_FRAMES);
                goto done;
            }
        }
        else if (!strcmp(argv[i], "--help"))
        {
            print_usage(argv[0]);
            success = 1;
            goto done;
        }
        else
        {
            print_usage(argv[0]);
            goto done;
        }
    }

    benchmark.measured_capacity = measured_count;
    benchmark.present_ns = (uint64_t *)calloc(measured_count, sizeof(*benchmark.present_ns));
    benchmark.frame_ns = (uint64_t *)calloc(measured_count, sizeof(*benchmark.frame_ns));
    benchmark.gpu_ns = (uint64_t *)calloc(measured_count, sizeof(*benchmark.gpu_ns));
    benchmark.gpu_valid = (unsigned char *)calloc(measured_count, sizeof(*benchmark.gpu_valid));
    if (!benchmark.present_ns || !benchmark.frame_ns || !benchmark.gpu_ns || !benchmark.gpu_valid)
    {
        fprintf(stderr, "Allocating bounded benchmark sample buffers failed\n");
        goto done;
    }

    benchmark.app_instance = GetModuleHandleW(NULL);
    if (!benchmark.app_instance || !create_benchmark_window(&benchmark) ||
            !load_global_api(&benchmark) || !create_instance(&benchmark) ||
            !create_surface(&benchmark) || !select_physical_device(&benchmark) ||
            !create_device(&benchmark) || !create_command_and_sync_objects(&benchmark) ||
            !create_swapchain(&benchmark) ||
            !run_benchmark(&benchmark, warmup_count, measured_count))
        goto done;

    print_results(&benchmark, measured_count);
    success = 1;

done:
    cleanup_benchmark(&benchmark);
    return success ? 0 : 1;
}
