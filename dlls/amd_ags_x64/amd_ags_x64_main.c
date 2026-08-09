#if 0
#pragma makedep arm64ec_x64
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/asm.h"

#define COBJMACROS
#include "initguid.h"
#include "d3d11.h"
#include "d3d12.h"

#include "dxgi1_6.h"

#include "dxvk_interfaces.h"

#include "amd_ags.h"

WINE_DEFAULT_DEBUG_CHANNEL(amd_ags);

static const char driver_version[] = "99.19.02-230831a-396538C-AMD-Software-Adrenalin-Edition";
static const char radeon_version[] = "99.10.2";

static BOOL amd_driver_extensions_available(void)
{
    const char *capability = getenv("SWITCHYARD_GPU_AMD_AGS_EXTENSIONS");
    const char *backend = getenv("SWITCHYARD_GPU_BACKEND");

    if (capability)
        return !strcmp(capability, "1");
    return !backend || strcmp(backend, "d3dmetal");
}

enum amd_ags_version
{
    AMD_AGS_VERSION_4_0_3,
    AMD_AGS_VERSION_5_0_5,
    AMD_AGS_VERSION_5_1_1,
    AMD_AGS_VERSION_5_2_0,
    AMD_AGS_VERSION_5_3_0,
    AMD_AGS_VERSION_5_4_0,
    AMD_AGS_VERSION_5_4_1,
    AMD_AGS_VERSION_5_4_2,
    AMD_AGS_VERSION_6_0_0,
    AMD_AGS_VERSION_6_1_0,

    AMD_AGS_VERSION_COUNT
};

static const struct
{
    unsigned int ags_min_public_version;
    unsigned int ags_max_public_version;
    unsigned int device_size;
    unsigned int dx11_returned_params_size;
    int max_asicFamily;
}
amd_ags_info[AMD_AGS_VERSION_COUNT] =
{
    {AGS_MAKE_VERSION(3, 0, 0), AGS_MAKE_VERSION(4, 0, 3), sizeof(AGSDeviceInfo_511), sizeof(AGSDX11ReturnedParams_511), 0},
    {AGS_MAKE_VERSION(5, 0, 0), AGS_MAKE_VERSION(5, 0, 6), sizeof(AGSDeviceInfo_511), sizeof(AGSDX11ReturnedParams_511), 0},
    {AGS_MAKE_VERSION(5, 1, 1), AGS_MAKE_VERSION(5, 1, 1), sizeof(AGSDeviceInfo_511), sizeof(AGSDX11ReturnedParams_511), 0},
    {AGS_MAKE_VERSION(5, 2, 0), AGS_MAKE_VERSION(5, 2, 1), sizeof(AGSDeviceInfo_520), sizeof(AGSDX11ReturnedParams_520), 0},
    {AGS_MAKE_VERSION(5, 3, 0), AGS_MAKE_VERSION(5, 3, 0), sizeof(AGSDeviceInfo_520), sizeof(AGSDX11ReturnedParams_520), 0},
    {AGS_MAKE_VERSION(5, 4, 0), AGS_MAKE_VERSION(5, 4, 0), sizeof(AGSDeviceInfo_540), sizeof(AGSDX11ReturnedParams_520), AsicFamily_RDNA},
    {AGS_MAKE_VERSION(5, 4, 1), AGS_MAKE_VERSION(5, 4, 1), sizeof(AGSDeviceInfo_541), sizeof(AGSDX11ReturnedParams_520), AsicFamily_RDNA},
    {AGS_MAKE_VERSION(5, 4, 2), AGS_MAKE_VERSION(5, 4, 2), sizeof(AGSDeviceInfo_542), sizeof(AGSDX11ReturnedParams_520), AsicFamily_RDNA},
    {AGS_MAKE_VERSION(6, 0, 0), AGS_MAKE_VERSION(6, 0, 1), sizeof(AGSDeviceInfo_600), sizeof(AGSDX11ReturnedParams_600), AsicFamily_RDNA2},
    {AGS_MAKE_VERSION(6, 1, 0), AGS_MAKE_VERSION(6, 2, 0), sizeof(AGSDeviceInfo_600), sizeof(AGSDX11ReturnedParams_600), AsicFamily_RDNA3},
};

#define DEF_FIELD(name) {DEVICE_FIELD_##name, {offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_511, name), \
        offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_520, name), \
        offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), \
        offsetof(AGSDeviceInfo_600, name), offsetof(AGSDeviceInfo_600, name)}}
#define DEF_FIELD_520_BELOW(name) {DEVICE_FIELD_##name, {offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_511, name), \
        offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_520, name), \
        offsetof(AGSDeviceInfo_520, name), -1, \
        -1, -1, -1, -1}}
#define DEF_FIELD_520_UP(name) {DEVICE_FIELD_##name, {-1, -1, -1, offsetof(AGSDeviceInfo_520, name), \
        offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), \
        offsetof(AGSDeviceInfo_600, name), offsetof(AGSDeviceInfo_600, name)}}
#define DEF_FIELD_540_UP(name) {DEVICE_FIELD_##name, {-1, -1, -1, -1, \
        -1, offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), \
        offsetof(AGSDeviceInfo_600, name), offsetof(AGSDeviceInfo_600, name)}}
#define DEF_FIELD_540_600(name) {DEVICE_FIELD_##name, {-1, -1, -1, -1, \
        -1, offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), \
        -1, -1}}
#define DEF_FIELD_600_BELOW(name) {DEVICE_FIELD_##name, {offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_511, name), \
        offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_520, name), \
        offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), \
        -1, -1}}

#define DEVICE_FIELD_adapterString 0
#define DEVICE_FIELD_architectureVersion 1
#define DEVICE_FIELD_asicFamily 2
#define DEVICE_FIELD_vendorId 3
#define DEVICE_FIELD_deviceId 4
#define DEVICE_FIELD_isPrimaryDevice 5
#define DEVICE_FIELD_localMemoryInBytes 6
#define DEVICE_FIELD_numDisplays 7
#define DEVICE_FIELD_displays 8
#define DEVICE_FIELD_isAPU 9

#define DEVICE_FIELD_numCUs 10
#define DEVICE_FIELD_coreClock 11
#define DEVICE_FIELD_memoryClock 12
#define DEVICE_FIELD_teraFlops 13
#define DEVICE_FIELD_numWGPs 14
#define DEVICE_FIELD_numROPs 15
#define DEVICE_FIELD_memoryBandwidth 16

static const struct
{
    unsigned int field_index;
    int offset[AMD_AGS_VERSION_COUNT];
}
device_struct_fields[] =
{
    DEF_FIELD(adapterString),
    DEF_FIELD_520_BELOW(architectureVersion),
    DEF_FIELD_540_UP(asicFamily),
    DEF_FIELD(vendorId),
    DEF_FIELD(deviceId),
    DEF_FIELD_600_BELOW(isPrimaryDevice),
    DEF_FIELD(localMemoryInBytes),
    DEF_FIELD(numDisplays),
    DEF_FIELD(displays),
    DEF_FIELD_540_600(isAPU),
    DEF_FIELD(numCUs),
    DEF_FIELD(coreClock),
    DEF_FIELD(memoryClock),
    DEF_FIELD(teraFlops),
    DEF_FIELD_540_UP(numWGPs),
    DEF_FIELD_520_UP(numROPs),
    DEF_FIELD_520_UP(memoryBandwidth),
};

#undef DEF_FIELD

#define GET_DEVICE_FIELD_ADDR(device, name, type, version) \
        (device_struct_fields[DEVICE_FIELD_##name].offset[version] == -1 ? NULL \
        : (type *)((BYTE *)device + device_struct_fields[DEVICE_FIELD_##name].offset[version]))

#define SET_DEVICE_FIELD(device, name, type, version, value) { \
        type *addr; \
        if ((addr = GET_DEVICE_FIELD_ADDR(device, name, type, version))) \
            *addr = value; \
    }

struct AGSContext
{
    enum amd_ags_version version;
    unsigned int device_count;
    struct AGSDeviceInfo *devices;
    VkPhysicalDeviceProperties *properties;
    VkPhysicalDeviceMemoryProperties *memory_properties;
    ID3D11DeviceContext *d3d11_context;
    AGSDX11ExtensionsSupported_600 extensions;
    unsigned int public_version;
};

static HMODULE hd3d11, hd3d12;
static typeof(D3D12CreateDevice) *pD3D12CreateDevice;
static typeof(D3D11CreateDevice) *pD3D11CreateDevice;
static typeof(D3D11CreateDeviceAndSwapChain) *pD3D11CreateDeviceAndSwapChain;

#define AGS_VER_MAJOR(ver) ((ver) >> 22)
#define AGS_VER_MINOR(ver) (((ver) >> 12) & ((1 << 10) - 1))
#define AGS_VER_PATCH(ver) ((ver) & ((1 << 12) - 1))

static const char *debugstr_agsversion(unsigned int ags_version)
{
    return wine_dbg_sprintf("%d.%d.%d", AGS_VER_MAJOR(ags_version), AGS_VER_MINOR(ags_version), AGS_VER_PATCH(ags_version));
}

static BOOL load_d3d12_functions(void)
{
    if (hd3d12)
        return TRUE;

    if (!(hd3d12 = LoadLibraryA("d3d12.dll")))
        return FALSE;

    pD3D12CreateDevice = (void *)GetProcAddress(hd3d12, "D3D12CreateDevice");
    if (!pD3D12CreateDevice)
    {
        ERR("D3D12CreateDevice is not exported by d3d12.dll.\n");
        FreeLibrary(hd3d12);
        hd3d12 = NULL;
        return FALSE;
    }
    return TRUE;
}

static BOOL load_d3d11_functions(void)
{
    if (hd3d11)
        return TRUE;

    if (!(hd3d11 = LoadLibraryA("d3d11.dll")))
        return FALSE;

    pD3D11CreateDevice = (void *)GetProcAddress(hd3d11, "D3D11CreateDevice");
    pD3D11CreateDeviceAndSwapChain = (void *)GetProcAddress(hd3d11, "D3D11CreateDeviceAndSwapChain");
    if (!pD3D11CreateDevice || !pD3D11CreateDeviceAndSwapChain)
    {
        ERR("Required D3D11 device creation exports are missing.\n");
        FreeLibrary(hd3d11);
        hd3d11 = NULL;
        pD3D11CreateDevice = NULL;
        pD3D11CreateDeviceAndSwapChain = NULL;
        return FALSE;
    }
    return TRUE;
}

static AGSReturnCode vk_get_physical_device_properties(unsigned int *out_count,
        VkPhysicalDeviceProperties **out, VkPhysicalDeviceMemoryProperties **out_memory)
{
    VkPhysicalDeviceProperties *properties = NULL;
    VkPhysicalDeviceMemoryProperties *memory_properties = NULL;
    VkPhysicalDevice *vk_physical_devices = NULL;
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkInstanceCreateInfo create_info;
    AGSReturnCode ret = AGS_SUCCESS;
    uint32_t accepted_count, count, i;
    VkResult vr;

    *out = NULL;
    *out_memory = NULL;
    *out_count = 0;

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if ((vr = vkCreateInstance(&create_info, NULL, &vk_instance)) < 0)
    {
        WARN("Failed to create Vulkan instance, vr %d.\n", vr);
        goto done;
    }

    if ((vr = vkEnumeratePhysicalDevices(vk_instance, &count, NULL)) < 0)
    {
        WARN("Failed to enumerate devices, vr %d.\n", vr);
        goto done;
    }

    if (!count)
        goto done;

    if (!(vk_physical_devices = calloc(count, sizeof(*vk_physical_devices))))
    {
        WARN("Failed to allocate memory.\n");
        ret = AGS_OUT_OF_MEMORY;
        goto done;
    }

    if ((vr = vkEnumeratePhysicalDevices(vk_instance, &count, vk_physical_devices)) < 0)
    {
        WARN("Failed to enumerate devices, vr %d.\n", vr);
        goto done;
    }
    if (!count)
        goto done;

    if (!(properties = calloc(count, sizeof(*properties))))
    {
        WARN("Failed to allocate memory.\n");
        ret = AGS_OUT_OF_MEMORY;
        goto done;
    }

    if (!(memory_properties = calloc(count, sizeof(*memory_properties))))
    {
        WARN("Failed to allocate memory.\n");
        free(properties);
        properties = NULL;
        ret = AGS_OUT_OF_MEMORY;
        goto done;
    }

    accepted_count = 0;

    for (i = 0; i < count; ++i)
    {
        VkPhysicalDeviceProperties property;

        vkGetPhysicalDeviceProperties(vk_physical_devices[i], &property);
        if (property.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                && property.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            TRACE("Skipping device type %d.\n", property.deviceType);
            continue;
        }
        properties[accepted_count] = property;
        vkGetPhysicalDeviceMemoryProperties(vk_physical_devices[i], &memory_properties[accepted_count]);
        ++accepted_count;
    }

    if (accepted_count)
    {
        *out_count = accepted_count;
        *out = properties;
        *out_memory = memory_properties;
    }
    else
    {
        free(properties);
        free(memory_properties);
    }

done:
    free(vk_physical_devices);
    if (vk_instance)
        vkDestroyInstance(vk_instance, NULL);
    return ret;
}

static enum amd_ags_version get_version_number(int ags_version)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(amd_ags_info); i++)
        if (ags_version >= amd_ags_info[i].ags_min_public_version && ags_version <= amd_ags_info[i].ags_max_public_version)
        {
            TRACE("Found AGS v%s (internal %d).\n", debugstr_agsversion(ags_version), i);
            return i;
        }
    ERR("Unknown ags_version %s, using 5.4.1.\n", debugstr_agsversion(ags_version));
    return AMD_AGS_VERSION_5_4_1;
}

static BOOL get_ags_version_from_resource(const WCHAR *filename, enum amd_ags_version *ret, int *public_version)
{
    DWORD infosize;
    void *infobuf;
    void *val;
    UINT vallen;
    VS_FIXEDFILEINFO *info;
    UINT16 major, minor, patch;

    infosize = GetFileVersionInfoSizeW(filename, NULL);
    if (!infosize)
    {
        TRACE("File version info not found, err %u.\n", GetLastError());
        return FALSE;
    }

    if (!(infobuf = malloc(infosize)))
    {
        ERR("Failed to allocate memory.\n");
        return FALSE;
    }

    if (!GetFileVersionInfoW(filename, 0, infosize, infobuf))
    {
        ERR("GetFileVersionInfoW failed, err %u.\n", GetLastError());
        free(infobuf);
        return FALSE;
    }

    if (!VerQueryValueW(infobuf, L"\\", &val, &vallen) || (vallen != sizeof(VS_FIXEDFILEINFO)))
    {
        ERR("Version value not found, err %u.\n", GetLastError());
        free(infobuf);
        return FALSE;
    }

    info = val;
    major = info->dwFileVersionMS >> 16;
    minor = info->dwFileVersionMS;
    patch = info->dwFileVersionLS >> 16;
    *public_version = AGS_MAKE_VERSION(major, minor, patch);
    TRACE("Found amd_ags_x64.dll v%d.%d.%d\n", major, minor, patch);
    *ret = get_version_number(*public_version);
    free(infobuf);
    return TRUE;
}

struct mapped_pe_image
{
    const BYTE *data;
    SIZE_T size;
    SIZE_T sections_offset;
    WORD section_count;
    DWORD size_of_headers;
    IMAGE_DATA_DIRECTORY exports;
};

static const void *mapped_file_range(const struct mapped_pe_image *image, SIZE_T offset, SIZE_T size)
{
    if (offset > image->size || size > image->size - offset)
        return NULL;
    return image->data + offset;
}

static BOOL mapped_file_read(const struct mapped_pe_image *image, SIZE_T offset, void *buffer, SIZE_T size)
{
    const void *ptr = mapped_file_range(image, offset, size);

    if (!ptr)
        return FALSE;
    memcpy(buffer, ptr, size);
    return TRUE;
}

static BOOL mapped_pe_image_init(struct mapped_pe_image *image, const void *data, SIZE_T size)
{
    IMAGE_OPTIONAL_HEADER32 optional32;
    IMAGE_OPTIONAL_HEADER64 optional64;
    IMAGE_FILE_HEADER file_header;
    IMAGE_DOS_HEADER dos;
    const void *optional;
    SIZE_T optional_offset, sections_size;
    DWORD signature;
    WORD magic;

    memset(image, 0, sizeof(*image));
    image->data = data;
    image->size = size;

    if (!mapped_file_read(image, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return FALSE;
    if (!mapped_file_read(image, dos.e_lfanew, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE)
        return FALSE;
    if ((SIZE_T)dos.e_lfanew > image->size - sizeof(signature)
            || !mapped_file_read(image, dos.e_lfanew + sizeof(signature), &file_header, sizeof(file_header)))
        return FALSE;

    optional_offset = dos.e_lfanew + sizeof(signature) + sizeof(file_header);
    if (!(optional = mapped_file_range(image, optional_offset, file_header.SizeOfOptionalHeader))
            || file_header.SizeOfOptionalHeader < sizeof(magic))
        return FALSE;
    memcpy(&magic, optional, sizeof(magic));

    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        if (file_header.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory[1]))
            return FALSE;
        memset(&optional32, 0, sizeof(optional32));
        memcpy(&optional32, optional, min(sizeof(optional32), file_header.SizeOfOptionalHeader));
        if (optional32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return FALSE;
        image->size_of_headers = optional32.SizeOfHeaders;
        image->exports = optional32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        if (file_header.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory[1]))
            return FALSE;
        memset(&optional64, 0, sizeof(optional64));
        memcpy(&optional64, optional, min(sizeof(optional64), file_header.SizeOfOptionalHeader));
        if (optional64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return FALSE;
        image->size_of_headers = optional64.SizeOfHeaders;
        image->exports = optional64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else
        return FALSE;

    /* The Windows loader supports at most 96 sections in an image. */
    if (!file_header.NumberOfSections || file_header.NumberOfSections > 96)
        return FALSE;
    image->sections_offset = optional_offset + file_header.SizeOfOptionalHeader;
    sections_size = (SIZE_T)file_header.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!mapped_file_range(image, image->sections_offset, sections_size))
        return FALSE;
    image->section_count = file_header.NumberOfSections;
    return TRUE;
}

static const void *mapped_pe_image_rva(const struct mapped_pe_image *image, DWORD rva, SIZE_T size)
{
    IMAGE_SECTION_HEADER section;
    SIZE_T delta, offset;
    unsigned int i;

    if (rva < image->size_of_headers)
    {
        if (size > image->size_of_headers - rva)
            return NULL;
        return mapped_file_range(image, rva, size);
    }

    for (i = 0; i < image->section_count; ++i)
    {
        if (!mapped_file_read(image, image->sections_offset + i * sizeof(section), &section, sizeof(section)))
            return NULL;
        if (rva < section.VirtualAddress)
            continue;
        delta = rva - section.VirtualAddress;
        if (delta > section.SizeOfRawData || size > section.SizeOfRawData - delta)
            continue;
        if ((SIZE_T)section.PointerToRawData > ~(SIZE_T)0 - delta)
            return NULL;
        offset = section.PointerToRawData + delta;
        return mapped_file_range(image, offset, size);
    }
    return NULL;
}

enum known_ags_export
{
    AGS_EXPORT_GET_DRIVER_VERSION_INFO = 1u << 0,
    AGS_EXPORT_SET_CROSSFIRE_MODE      = 1u << 1,
    AGS_EXPORT_DRIVER_EXTENSIONS_INIT  = 1u << 2,
    AGS_EXPORT_GET_EYEFINITY_INFO      = 1u << 3,
    AGS_EXPORT_DX11_INIT               = 1u << 4,
};

static unsigned int image_get_known_ags_exports(const void *data, SIZE_T size)
{
    static const struct
    {
        const char *name;
        SIZE_T size;
        unsigned int flag;
    }
    known_exports[] =
    {
        {"agsGetDriverVersionInfo", sizeof("agsGetDriverVersionInfo"), AGS_EXPORT_GET_DRIVER_VERSION_INFO},
        {"agsDriverExtensions_SetCrossfireMode", sizeof("agsDriverExtensions_SetCrossfireMode"),
                AGS_EXPORT_SET_CROSSFIRE_MODE},
        {"agsDriverExtensions_Init", sizeof("agsDriverExtensions_Init"), AGS_EXPORT_DRIVER_EXTENSIONS_INIT},
        {"agsGetEyefinityConfigInfo", sizeof("agsGetEyefinityConfigInfo"), AGS_EXPORT_GET_EYEFINITY_INFO},
        {"agsDriverExtensionsDX11_Init", sizeof("agsDriverExtensionsDX11_Init"), AGS_EXPORT_DX11_INIT},
    };
    IMAGE_EXPORT_DIRECTORY exports;
    struct mapped_pe_image image;
    const BYTE *names;
    unsigned int flags = 0, i, j;
    SIZE_T names_size;
    DWORD name_rva;

    if (!mapped_pe_image_init(&image, data, size) || image.exports.Size < sizeof(exports)
            || !(data = mapped_pe_image_rva(&image, image.exports.VirtualAddress, sizeof(exports))))
        return 0;
    memcpy(&exports, data, sizeof(exports));

    if (exports.NumberOfNames > image.size / sizeof(DWORD))
        return 0;
    names_size = (SIZE_T)exports.NumberOfNames * sizeof(DWORD);
    if (!(names = mapped_pe_image_rva(&image, exports.AddressOfNames, names_size)))
        return 0;

    for (i = 0; i < exports.NumberOfNames; ++i)
    {
        memcpy(&name_rva, names + i * sizeof(name_rva), sizeof(name_rva));
        for (j = 0; j < ARRAY_SIZE(known_exports); ++j)
        {
            const char *name = mapped_pe_image_rva(&image, name_rva, known_exports[j].size);
            if (name && !memcmp(name, known_exports[j].name, known_exports[j].size))
            {
                flags |= known_exports[j].flag;
                break;
            }
        }
    }
    return flags;
}

static enum amd_ags_version guess_version_from_exports(const WCHAR *filename, int *ags_version)
{
    HANDLE file = INVALID_HANDLE_VALUE, mapping = NULL;
    enum amd_ags_version ret = AMD_AGS_VERSION_5_4_1;
    const void *module = NULL;
    LARGE_INTEGER file_size;
    unsigned int exports;

    /* Known DLL versions without version info:
     *  - An update to AGS 5.4.1 included an amd_ags_x64.dll with no file version info;
     *  - CoD: Modern Warfare Remastered (2017) ships dll without version info which is version 5.0.1
     *    (not tagged in AGSSDK history), compatible with 5.0.5.
     *
     * Map the image as data instead of using LoadLibraryW(), since loading the app-provided
     * native DLL would execute its DllMain even when Wine selected the builtin AGS module.
     */
    file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        goto done;
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0
            || (ULONGLONG)file_size.QuadPart > (ULONGLONG)~(SIZE_T)0)
        goto done;
    if (!(mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL)))
        goto done;
    if (!(module = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)))
        goto done;
    exports = image_get_known_ags_exports(module, file_size.QuadPart);

    if (exports & AGS_EXPORT_GET_DRIVER_VERSION_INFO)
    {
        /* agsGetDriverVersionInfo existed somewhere before 3.1.1, there is no SDK history in github before 3.1.1. */
        TRACE("agsGetDriverVersionInfo found.\n");
        *ags_version = AGS_MAKE_VERSION(3, 0, 0);
        ret = AMD_AGS_VERSION_4_0_3;
        goto done;
    }

    if (exports & AGS_EXPORT_SET_CROSSFIRE_MODE)
    {
        /* agsDriverExtensions_SetCrossfireMode was deprecated in 3.2.0 */
        TRACE("agsDriverExtensions_SetCrossfireMode found.\n");
        *ags_version = AGS_MAKE_VERSION(3, 1, 1);
        ret = AMD_AGS_VERSION_4_0_3;
        goto done;
    }
    if (exports & AGS_EXPORT_DRIVER_EXTENSIONS_INIT)
    {
        /* agsGetEyefinityConfigInfo was deprecated in 4.0.0 */
        TRACE("agsDriverExtensions_Init found.\n");
        *ags_version = AGS_MAKE_VERSION(3, 2, 2);
        ret = AMD_AGS_VERSION_4_0_3;
        goto done;
    }
    if (exports & AGS_EXPORT_GET_EYEFINITY_INFO)
    {
        /* agsGetEyefinityConfigInfo was deprecated in 5.0.0 */
        TRACE("agsGetEyefinityConfigInfo found.\n");
        ret = AMD_AGS_VERSION_4_0_3;
        goto done;
    }
    if (exports & AGS_EXPORT_DX11_INIT)
    {
        /* agsDriverExtensionsDX11_Init was deprecated in 5.3.0 */
        TRACE("agsDriverExtensionsDX11_Init found.\n");
        ret = AMD_AGS_VERSION_5_0_5;
        goto done;
    }
    TRACE("Returning 5.4.1.\n");

done:
    if (module) UnmapViewOfFile(module);
    if (mapping) CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ret;
}

static BOOL is_wine_builtin_file(const WCHAR *filename)
{
    static const char builtin_signature[] = "Wine builtin DLL";
    BYTE buffer[sizeof(IMAGE_DOS_HEADER) + sizeof(builtin_signature)];
    HANDLE file;
    DWORD read;

    file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;

    read = 0;
    ReadFile(file, buffer, sizeof(buffer), &read, NULL);
    CloseHandle(file);

    return read == sizeof(buffer)
            && !memcmp(buffer + sizeof(IMAGE_DOS_HEADER), builtin_signature, sizeof(builtin_signature));
}

static enum amd_ags_version determine_ags_version(int *ags_version)
{
    /* AMD AGS is not binary compatible between versions (even minor versions), and the game
     * does not request a specific version when calling agsInit().
     * Checking the version of amd_ags_x64.dll shipped with the game is the only way to
     * determine what version the game was built against.
     */
    enum amd_ags_version ret = AMD_AGS_VERSION_5_4_1;
    WCHAR dllname[MAX_PATH], temp_path[MAX_PATH], temp_name[MAX_PATH];
    HMODULE module;
    DWORD size;

    TRACE("*ags_version %#x.\n", *ags_version);

    if (*ags_version)
        return get_version_number(*ags_version);

    *temp_name = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (const WCHAR *)determine_ags_version, &module))
    {
        ERR("GetModuleHandleExW failed.\n");
        goto done;
    }
    if (!(size = GetModuleFileNameW(module, dllname, ARRAY_SIZE(dllname)))
            || size == ARRAY_SIZE(dllname))
    {
        ERR("GetModuleFileNameW failed.\n");
        goto done;
    }
    if (is_wine_builtin_file(dllname))
    {
        TRACE("The loaded module %s has no native AGS version metadata, using the compatibility default.\n",
                debugstr_w(dllname));
        goto done;
    }
    if (!GetTempPathW(MAX_PATH, temp_path) || !GetTempFileNameW(temp_path, L"tmp", 0, temp_name))
    {
        ERR("Failed getting temp file name.\n");
        goto done;
    }
    if (!CopyFileW(dllname, temp_name, FALSE))
    {
        ERR("Failed to copy file.\n");
        goto done;
    }

    if (get_ags_version_from_resource(temp_name, &ret, ags_version))
        goto done;

    ret = guess_version_from_exports(temp_name, ags_version);

done:
    if (!*ags_version)
        *ags_version = amd_ags_info[ret].ags_max_public_version;

    if (*temp_name)
        DeleteFileW(temp_name);

    TRACE("Using AGS v%s (internal %d) interface\n", debugstr_agsversion(*ags_version), ret);
    return ret;
}

struct monitor_enum_context_600
{
    const char *adapter_name;
    AGSDisplayInfo_600 **ret_displays;
    int *ret_display_count;
    IDXGIFactory1 *dxgi_factory;
};

static void create_dxgi_factory(HMODULE *hdxgi, IDXGIFactory1 **factory)
{
    typeof(CreateDXGIFactory1) *pCreateDXGIFactory1;

    *hdxgi = NULL;
    *factory = NULL;

    if (!(*hdxgi = LoadLibraryW(L"dxgi.dll")))
    {
        ERR("Could not load dxgi.dll.\n");
        return;
    }

    if (!(pCreateDXGIFactory1 = (void *)GetProcAddress(*hdxgi, "CreateDXGIFactory1")))
    {
        ERR("Could not find CreateDXGIFactory1.\n");
        return;
    }

    if (FAILED(pCreateDXGIFactory1(&IID_IDXGIFactory1, (void**)factory)))
        return;
}

static void release_dxgi_factory(HMODULE hdxgi, IDXGIFactory1 *factory)
{
    if (factory)
        IDXGIFactory1_Release(factory);
    if (hdxgi)
        FreeLibrary(hdxgi);
}

static void fill_chroma_info(AGSDisplayInfo_600 *info, struct monitor_enum_context_600 *c, HMONITOR monitor)
{
    DXGI_OUTPUT_DESC1 output_desc;
    IDXGIAdapter1 *adapter;
    IDXGIOutput6 *output6;
    IDXGIOutput *output;
    BOOL found = FALSE;
    unsigned int i, j;
    HRESULT hr;

    if (!c->dxgi_factory)
        return;

    i = 0;
    while (!found && (SUCCEEDED(IDXGIFactory1_EnumAdapters1(c->dxgi_factory, i++, &adapter))))
    {
        j = 0;
        while (SUCCEEDED(IDXGIAdapter1_EnumOutputs(adapter, j++, &output)))
        {
            hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void**)&output6);
            IDXGIOutput_Release(output);
            if (FAILED(hr))
            {
                WARN("Failed to query IDXGIOutput6.\n");
                continue;
            }
            hr = IDXGIOutput6_GetDesc1(output6, &output_desc);
            IDXGIOutput6_Release(output6);

            if (FAILED(hr) || output_desc.Monitor != monitor)
                continue;
            found = TRUE;

            TRACE("output_desc.ColorSpace %#x.\n", output_desc.ColorSpace);
            if (output_desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            {
                TRACE("Reporting monitor %s as HDR10 supported.\n", debugstr_a(info->displayDeviceName));
                info->HDR10 = 1;
            }

            info->chromaticityRedX = output_desc.RedPrimary[0];
            info->chromaticityRedY = output_desc.RedPrimary[1];
            info->chromaticityGreenX = output_desc.GreenPrimary[0];
            info->chromaticityGreenY = output_desc.GreenPrimary[1];
            info->chromaticityBlueX = output_desc.BluePrimary[0];
            info->chromaticityBlueY = output_desc.BluePrimary[1];
            info->chromaticityWhitePointX = output_desc.WhitePoint[0];
            info->chromaticityWhitePointY = output_desc.WhitePoint[1];

            TRACE("chromacity: (%.6lf, %.6lf) (%.6lf, %.6lf) (%.6lf, %.6lf).\n", info->chromaticityRedX,
                    info->chromaticityRedY, info->chromaticityGreenX, info->chromaticityGreenY, info->chromaticityBlueX,
                    info->chromaticityBlueY);

            info->screenDiffuseReflectance = 0;
            info->screenSpecularReflectance = 0;

            info->minLuminance = output_desc.MinLuminance;
            info->maxLuminance = output_desc.MaxLuminance;
            info->avgLuminance = output_desc.MaxFullFrameLuminance;
        }
        IDXGIAdapter1_Release(adapter);
    }

    if (!found)
        WARN("dxgi output not found.\n");
}

static BOOL WINAPI monitor_enum_proc_600(HMONITOR hmonitor, HDC hdc, RECT *rect, LPARAM context)
{
    struct monitor_enum_context_600 *c = (struct monitor_enum_context_600 *)context;
    MONITORINFOEXA monitor_info;
    AGSDisplayInfo_600 *new_alloc;
    DISPLAY_DEVICEA device;
    AGSDisplayInfo_600 *info;
    unsigned int i, mode;
    DEVMODEA dev_mode;


    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoA(hmonitor, (MONITORINFO *)&monitor_info);
    TRACE("monitor_info.szDevice %s.\n", debugstr_a(monitor_info.szDevice));

    device.cb = sizeof(device);
    i = 0;
    while (EnumDisplayDevicesA(NULL, i, &device, 0))
    {
        TRACE("device.DeviceName %s, device.DeviceString %s.\n", debugstr_a(device.DeviceName), debugstr_a(device.DeviceString));
        ++i;
        if (strcmp(device.DeviceString, c->adapter_name) || strcmp(device.DeviceName, monitor_info.szDevice))
            continue;

        if (*c->ret_display_count)
        {
            if (!(new_alloc = realloc(*c->ret_displays, sizeof(*new_alloc) * (*c->ret_display_count + 1))))
            {
                ERR("No memory.");
                return FALSE;
            }
            *c->ret_displays = new_alloc;
        }
        else if (!(*c->ret_displays = malloc(sizeof(**c->ret_displays))))
        {
            ERR("No memory.");
            return FALSE;
        }
        info = &(*c->ret_displays)[*c->ret_display_count];
        memset(info, 0, sizeof(*info));
        strcpy(info->displayDeviceName, device.DeviceName);
        if (EnumDisplayDevicesA(info->displayDeviceName, 0, &device, 0))
        {
            strcpy(info->name, device.DeviceString);
        }
        else
        {
            ERR("Could not get monitor name for device %s.\n", debugstr_a(info->displayDeviceName));
            strcpy(info->name, "Unknown");
        }
        if (monitor_info.dwFlags & MONITORINFOF_PRIMARY)
            info->isPrimaryDisplay = 1;

        mode = 0;
        memset(&dev_mode, 0, sizeof(dev_mode));
        dev_mode.dmSize = sizeof(dev_mode);
        while (EnumDisplaySettingsExA(monitor_info.szDevice, mode, &dev_mode, EDS_RAWMODE))
        {
            ++mode;
            if (dev_mode.dmPelsWidth > info->maxResolutionX)
                info->maxResolutionX = dev_mode.dmPelsWidth;
            if (dev_mode.dmPelsHeight > info->maxResolutionY)
                info->maxResolutionY = dev_mode.dmPelsHeight;
            if (dev_mode.dmDisplayFrequency > info->maxRefreshRate)
                info->maxRefreshRate = dev_mode.dmDisplayFrequency;
            memset(&dev_mode, 0, sizeof(dev_mode));
            dev_mode.dmSize = sizeof(dev_mode);
        }

        info->eyefinityGridCoordX = -1;
        info->eyefinityGridCoordY = -1;

        info->currentResolution.offsetX = monitor_info.rcMonitor.left;
        info->currentResolution.offsetY = monitor_info.rcMonitor.top;
        info->currentResolution.width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
        info->currentResolution.height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
        info->visibleResolution = info->currentResolution;

        memset(&dev_mode, 0, sizeof(dev_mode));
        dev_mode.dmSize = sizeof(dev_mode);

        if (EnumDisplaySettingsExA(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &dev_mode, EDS_RAWMODE))
            info->currentRefreshRate = dev_mode.dmDisplayFrequency;
        else
            ERR("Could not get current display settings.\n");

        fill_chroma_info(info, c, hmonitor);

        ++*c->ret_display_count;

        TRACE("Added display %s for %s.\n", debugstr_a(monitor_info.szDevice), debugstr_a(c->adapter_name));
    }

    return TRUE;
}

static void init_device_displays_600(const char *adapter_name, AGSDisplayInfo_600 **ret_displays, int *ret_display_count)
{
    struct monitor_enum_context_600 context;
    HMODULE hdxgi = NULL;

    TRACE("adapter_name %s.\n", debugstr_a(adapter_name));

    *ret_displays = NULL;
    *ret_display_count = 0;
    context.adapter_name = adapter_name;
    context.ret_displays = ret_displays;
    context.ret_display_count = ret_display_count;
    context.dxgi_factory = NULL;

    /*
     * GPTK 3 can deadlock while recursively creating a D3DMetal DXGI factory
     * from AGS initialization. The HDR metadata is optional, so keep Win32
     * display enumeration but avoid a second graphics-runtime initialization.
     */
    if (getenv("SWITCHYARD_GPTK_PATH"))
        TRACE("Skipping optional DXGI display metadata under GPTK.\n");
    else
        create_dxgi_factory(&hdxgi, &context.dxgi_factory);

    EnumDisplayMonitors(NULL, NULL, monitor_enum_proc_600, (LPARAM)&context);
    release_dxgi_factory(hdxgi, context.dxgi_factory);
}

static void init_device_displays_511(const char *adapter_name, AGSDisplayInfo_511 **ret_displays, int *ret_display_count)
{
    AGSDisplayInfo_600 *displays = NULL;
    int display_count = 0;
    int i;
    *ret_displays = NULL;
    *ret_display_count = 0;

    init_device_displays_600(adapter_name, &displays, &display_count);

    if ((*ret_displays = malloc(sizeof(**ret_displays) * display_count)))
    {
        for (i = 0; i < display_count; i++)
        {
            memcpy(&(*ret_displays)[i], &displays[i], sizeof(AGSDisplayInfo_511));
        }
        *ret_display_count = display_count;
    }

    free(displays);
}

static int hide_apu(void)
{
    static int cached = -1;

    if (cached == -1)
    {
        const char *s;

        cached = ((s = getenv("WINE_HIDE_APU"))) && *s != '0';
        if (cached)
            FIXME("hack: hiding APU.\n");
    }
    return cached;
}

static AGSReturnCode init_ags_context(AGSContext *context, int ags_version)
{
    AGSReturnCode ret;
    unsigned int i, j;
    BYTE *device;

    memset(context, 0, sizeof(*context));

    context->version = determine_ags_version(&ags_version);
    context->public_version = ags_version;

    ret = vk_get_physical_device_properties(&context->device_count, &context->properties, &context->memory_properties);
    if (ret != AGS_SUCCESS || !context->device_count)
        return ret;

    assert(context->version < AMD_AGS_VERSION_COUNT);

    if (!(context->devices = calloc(context->device_count, amd_ags_info[context->version].device_size)))
    {
        WARN("Failed to allocate memory.\n");
        free(context->properties);
        free(context->memory_properties);
        return AGS_OUT_OF_MEMORY;
    }

    device = (BYTE *)context->devices;
    for (i = 0; i < context->device_count; ++i)
    {
        const VkPhysicalDeviceProperties *vk_properties = &context->properties[i];
        const VkPhysicalDeviceMemoryProperties *vk_memory_properties = &context->memory_properties[i];
        struct AGSDeviceInfo_600 *device_600 = (struct AGSDeviceInfo_600 *)device;
        VkDeviceSize local_memory_size = 0;

        for (j = 0; j < vk_memory_properties->memoryHeapCount; j++)
        {
            if (vk_memory_properties->memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                local_memory_size = vk_memory_properties->memoryHeaps[j].size;
                break;
            }
        }

        TRACE("device %s, type %d, %04x:%04x, reporting local memory size 0x%s bytes\n",
                debugstr_a(vk_properties->deviceName), vk_properties->deviceType,
                vk_properties->vendorID, vk_properties->deviceID, wine_dbgstr_longlong(local_memory_size));

        /*
         * Preserve the underlying Vulkan device identity. D3DMetal's synthetic
         * AMD DXGI identity must not make AGS advertise AMD hardware or driver
         * extensions that the host device does not actually implement.
         */
        SET_DEVICE_FIELD(device, adapterString, const char *, context->version, vk_properties->deviceName);
        SET_DEVICE_FIELD(device, vendorId, int, context->version, vk_properties->vendorID);
        SET_DEVICE_FIELD(device, deviceId, int, context->version, vk_properties->deviceID);
        if (vk_properties->vendorID == 0x1002)
        {
            SET_DEVICE_FIELD(device, architectureVersion, ArchitectureVersion, context->version, ArchitectureVersion_GCN);
            /*
             * The DRM helper used by Proton is not available on macOS.
             * Keep the conservative fallback rather than inventing clock or
             * topology data for the D3DMetal compatibility adapter.
             */
            SET_DEVICE_FIELD(device, asicFamily, AsicFamily, context->version, AsicFamily_GCN4);
            if (vk_properties->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && !hide_apu())
            {
                if (context->version >= AMD_AGS_VERSION_6_0_0)
                    device_600->isAPU = 1;
                else
                    SET_DEVICE_FIELD(device, isAPU, int, context->version, 1);
            }
        }
        SET_DEVICE_FIELD(device, localMemoryInBytes, ULONG64, context->version, local_memory_size);
        if (!i)
        {
            if (context->version >= AMD_AGS_VERSION_6_0_0)
            {
                // This is a bitfield now... Nice...
                device_600->isPrimaryDevice = 1;
            }
            else
            {
                SET_DEVICE_FIELD(device, isPrimaryDevice, int, context->version, 1);
            }
        }

        if (context->version >= AMD_AGS_VERSION_6_0_0)
        {
            init_device_displays_600(vk_properties->deviceName,
                    GET_DEVICE_FIELD_ADDR(device, displays, AGSDisplayInfo_600 *, context->version),
                    GET_DEVICE_FIELD_ADDR(device, numDisplays, int, context->version));
        }
        else
        {
            init_device_displays_511(vk_properties->deviceName,
                    GET_DEVICE_FIELD_ADDR(device, displays, AGSDisplayInfo_511 *, context->version),
                    GET_DEVICE_FIELD_ADDR(device, numDisplays, int, context->version));
        }

        device += amd_ags_info[context->version].device_size;
    }

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsInit(AGSContext **context, const AGSConfiguration *config, AGSGPUInfo_511 *gpu_info)
{
    struct AGSContext *object;
    AGSReturnCode ret;

    TRACE("context %p, config %p, gpu_info %p.\n", context, config, gpu_info);

    if (!context)
        return AGS_INVALID_ARGS;

    if (config)
        FIXME("Ignoring config %p.\n", config);

    if (!(object = malloc(sizeof(*object))))
        return AGS_OUT_OF_MEMORY;

    if ((ret = init_ags_context(object, 0)) != AGS_SUCCESS)
    {
        free(object);
        return ret;
    }

    if (object->public_version <= AGS_MAKE_VERSION(3, 0, 0))
    {
        WARN("Detected pre-historic AGS version.\n");
        goto done;
    }
    else if (object->public_version <= AGS_MAKE_VERSION(3, 1, 1))
    {
        /* Unfortunately it doesn't look sanely possible to distinguish 3.1.1 and 3.1.0 versions, while in
         * 3.1.0 radeonSoftwareVersion was present, removed in 3.1.1 and brought back in 3.2.2. */
        struct AGSDeviceInfo_511 *devices = (struct AGSDeviceInfo_511 *)object->devices, *device;
        /* config parameter was added in 3.2.0, so gpu_info is actually the second parameter. */
        struct AGSGPUInfo_311 *info = (struct AGSGPUInfo_311 *)config;
        unsigned int i;

        if (!info)
            goto done;

        TRACE("filling AGSGPUInfo_311.\n");
        if (!object->device_count)
        {
            ERR("No devices.\n");
            agsDeInit(object);
            return AGS_FAILURE;
        }

        for (i = 0; i < object->device_count; ++i)
            if (devices[i].isPrimaryDevice)
                break;
        if (i == object->device_count)
        {
            WARN("No primary device, using first.\n");
            i = 0;
        }
        device = &devices[i];
        memset(info, 0, sizeof(*info));
        info->adapterString = device->adapterString;
        info->deviceId = device->deviceId;
        info->revisionId = device->revisionId;
        info->driverVersion = driver_version;
        info->iNumCUs = device->numCUs;
        info->iCoreClock = device->coreClock;
        info->iMemoryClock = device->memoryClock;
        info->fTFlops = device->teraFlops;
    }
    else if (object->public_version <= AGS_MAKE_VERSION(3, 2, 2))
    {
        /* Unfortunately it doesn't look sanely possible to distinguish 3.2.2 and 3.2.0 versions, while in
         * 3.2.2 radeonSoftwareVersion was added in the middle of the structure. So fill the shorter one
         * to avoid out of bound write. */
        struct AGSDeviceInfo_511 *devices = (struct AGSDeviceInfo_511 *)object->devices, *device;
        struct AGSGPUInfo_320 *info = (struct AGSGPUInfo_320 *)gpu_info;
        unsigned int i;

        if (!gpu_info)
            goto done;

        TRACE("filling AGSGPUInfo_320.\n");
        if (!object->device_count)
        {
            ERR("No devices.\n");
            agsDeInit(object);
            return AGS_FAILURE;
        }

        for (i = 0; i < object->device_count; ++i)
            if (devices[i].isPrimaryDevice)
                break;
        if (i == object->device_count)
        {
            WARN("No primary device, using first.\n");
            i = 0;
        }
        device = &devices[i];
        memset(info, 0, sizeof(*info));
        info->agsVersionMajor = AGS_VER_MAJOR(object->public_version);
        info->agsVersionMinor = AGS_VER_MINOR(object->public_version);
        info->agsVersionPatch = AGS_VER_PATCH(object->public_version);
        info->architectureVersion = device->architectureVersion;
        info->adapterString = device->adapterString;
        info->deviceId = device->deviceId;
        info->revisionId = device->revisionId;
        info->driverVersion = driver_version;
        info->iNumCUs = device->numCUs;
        info->iCoreClock = device->coreClock;
        info->iMemoryClock = device->memoryClock;
        info->fTFlops = device->teraFlops;
    }
    else if (object->version <= AMD_AGS_VERSION_4_0_3)
    {
        struct AGSDeviceInfo_511 *devices = (struct AGSDeviceInfo_511 *)object->devices, *device;
        struct AGSGPUInfo_403 *info = (struct AGSGPUInfo_403 *)gpu_info;
        unsigned int i;

        if (!gpu_info)
            goto done;

        if (!object->device_count)
        {
            ERR("No devices.\n");
            agsDeInit(object);
            return AGS_FAILURE;
        }

        for (i = 0; i < object->device_count; ++i)
            if (devices[i].isPrimaryDevice)
                break;
        if (i == object->device_count)
        {
            WARN("No primary device, using first.\n");
            i = 0;
        }
        device = &devices[i];
        memset(info, 0, sizeof(*info));
        info->agsVersionMajor = AGS_VER_MAJOR(object->public_version);
        info->agsVersionMinor = AGS_VER_MINOR(object->public_version);
        info->agsVersionPatch = AGS_VER_PATCH(object->public_version);
        info->architectureVersion = device->architectureVersion;
        info->adapterString = device->adapterString;
        info->deviceId = device->deviceId;
        info->revisionId = device->revisionId;
        info->driverVersion = driver_version;
        info->radeonSoftwareVersion  = radeon_version;
        info->iNumCUs = device->numCUs;
        info->iCoreClock = device->coreClock;
        info->iMemoryClock = device->memoryClock;
        info->fTFlops = device->teraFlops;
    }
    else
    {
        if (!gpu_info)
            goto done;

        memset(gpu_info, 0, sizeof(*gpu_info));
        gpu_info->agsVersionMajor = AGS_VER_MAJOR(object->public_version);
        gpu_info->agsVersionMinor = AGS_VER_MINOR(object->public_version);
        gpu_info->agsVersionPatch = AGS_VER_PATCH(object->public_version);
        gpu_info->driverVersion = driver_version;
        gpu_info->radeonSoftwareVersion  = radeon_version;
        gpu_info->numDevices = object->device_count;
        gpu_info->devices = object->devices;
    }

done:
    TRACE("Created context %p.\n", object);

    *context = object;

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsInitialize(int ags_version, const AGSConfiguration *config, AGSContext **context, AGSGPUInfo_600 *gpu_info)
{
    struct AGSContext *object;
    AGSReturnCode ret;

    TRACE("ags_verison %d, context %p, config %p, gpu_info %p.\n", ags_version, context, config, gpu_info);

    if (!context)
        return AGS_INVALID_ARGS;

    if (config)
        FIXME("Ignoring config %p.\n", config);

    if (!(object = malloc(sizeof(*object))))
        return AGS_OUT_OF_MEMORY;

    if ((ret = init_ags_context(object, ags_version)) != AGS_SUCCESS)
    {
        free(object);
        return ret;
    }

    if (gpu_info)
    {
        memset(gpu_info, 0, sizeof(*gpu_info));
        gpu_info->driverVersion = driver_version;
        gpu_info->radeonSoftwareVersion  = radeon_version;
        gpu_info->numDevices = object->device_count;
        gpu_info->devices = object->devices;
    }

    TRACE("Created context %p.\n", object);

    *context = object;

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDeInit(AGSContext *context)
{
    return agsDeInitialize(context);
}

AGSReturnCode WINAPI agsDeInitialize(AGSContext *context)
{
    unsigned int i;
    BYTE *device;

    TRACE("context %p.\n", context);

    if (!context)
        return AGS_SUCCESS;

    if (context->d3d11_context)
    {
        ID3D11DeviceContext_Release(context->d3d11_context);
        context->d3d11_context = NULL;
    }
    free(context->memory_properties);
    free(context->properties);
    device = (BYTE *)context->devices;
    for (i = 0; i < context->device_count; ++i)
    {
        free(*GET_DEVICE_FIELD_ADDR(device, displays, void *, context->version));
        device += amd_ags_info[context->version].device_size;
    }
    free(context->devices);
    free(context);

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsGetTotalGPUCount(AGSContext *context, int *numGPUs)
{
    TRACE("context %p, numGPUs %p.\n", context, numGPUs);

    *numGPUs = context->device_count;
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsGetGPUMemorySize( AGSContext *context, int gpuIndex, long long *sizeInBytes )
{
    struct AGSDeviceInfo_511 *device = &((struct AGSDeviceInfo_511 *)context->devices)[gpuIndex];

    TRACE("context %p, gpuIndex %d, sizeInBytes %p.\n", context, gpuIndex, sizeInBytes);

    if ((unsigned)gpuIndex >= context->device_count)
        return AGS_INVALID_ARGS;

    *sizeInBytes = device->localMemoryInBytes;
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsGetEyefinityConfigInfo( AGSContext *context, int displayIndex, AGSEyefinityInfo *eyefinityInfo,
        int *numDisplaysInfo, AGSDisplayInfo_403 *displaysInfo )
{
    struct AGSDeviceInfo_511 *devices;
    unsigned int i;

    TRACE("context %p, displayIndex %d, eyefinityInfo %p, numDisplaysInfo %p, displaysInfo %p\n",
            context, displayIndex, eyefinityInfo, numDisplaysInfo, displaysInfo);

    devices = (struct AGSDeviceInfo_511 *)context->devices;
    *numDisplaysInfo = 0;
    for (i = 0; i < context->device_count; ++i)
        *numDisplaysInfo += devices[i].numDisplays;

    if (!eyefinityInfo || !displaysInfo)
        return AGS_SUCCESS;

    /* displaysInfo is not filled in on Windows if Eyefinity is not enabled. */
    memset(eyefinityInfo, 0, sizeof(*eyefinityInfo));
    memset(displaysInfo, 0, *numDisplaysInfo * sizeof(*displaysInfo));

    return AGS_SUCCESS;
}

static DXGI_COLOR_SPACE_TYPE convert_ags_colorspace_506(AGSDisplaySettings_Mode_506 mode)
{
    switch (mode)
    {
        default:
            ERR("Unknown color space in AGS: %d.\n", mode);
        /* fallthrough */
        case Mode_506_SDR:
            TRACE("Setting Mode_506_SDR.\n");
            return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        case Mode_506_PQ:
            TRACE("Setting Mode_506_PQ.\n");
            return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        case Mode_506_scRGB:
            TRACE("Setting Mode_506_scRGB.\n");
            return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    }
}

static DXGI_COLOR_SPACE_TYPE convert_ags_colorspace_600(AGSDisplaySettings_Mode_600 mode)
{
    switch (mode)
    {
        default:
            ERR("Unknown color space in AGS: %d\n", mode);
        /* fallthrough */
        case Mode_600_SDR:
            TRACE("Setting Mode_600_SDR.\n");
            return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        case Mode_600_HDR10_PQ:
            TRACE("Setting Mode_600_HDR10_PQ.\n");
            return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        case Mode_600_HDR10_scRGB:
            TRACE("Setting Mode_600_HDR10_scRGB.\n");
            return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    }
}

static DXGI_HDR_METADATA_HDR10 convert_ags_metadata(const AGSDisplaySettings_600 *settings)
{
    DXGI_HDR_METADATA_HDR10 metadata;
    metadata.RedPrimary[0] = settings->chromaticityRedX * 50000;
    metadata.RedPrimary[1] = settings->chromaticityRedY * 50000;
    metadata.GreenPrimary[0] = settings->chromaticityGreenX * 50000;
    metadata.GreenPrimary[1] = settings->chromaticityGreenY * 50000;
    metadata.BluePrimary[0] = settings->chromaticityBlueX * 50000;
    metadata.BluePrimary[1] = settings->chromaticityBlueY * 50000;
    metadata.WhitePoint[0] = settings->chromaticityWhitePointX * 50000;
    metadata.WhitePoint[1] = settings->chromaticityWhitePointY * 50000;
    metadata.MaxMasteringLuminance = settings->maxLuminance;
    metadata.MinMasteringLuminance = settings->minLuminance / 0.0001f;
    metadata.MaxContentLightLevel = settings->maxContentLightLevel;
    metadata.MaxFrameAverageLightLevel = settings->maxFrameAverageLightLevel;
    return metadata;
}

AGSReturnCode WINAPI agsSetDisplayMode(AGSContext *context, int device_index, int display_index, const AGSDisplaySettings *settings)
{
    const AGSDisplaySettings_506 *settings506;
    const AGSDisplaySettings_600 *settings600;
    IDXGIVkInteropFactory1 *dxgi_interop = NULL;
    DXGI_COLOR_SPACE_TYPE colorspace;
    DXGI_HDR_METADATA_HDR10 metadata;
    AGSReturnCode ret = AGS_SUCCESS;
    IDXGIFactory1 *dxgi_factory = NULL;
    HMODULE hdxgi = NULL;

    TRACE("context %p device_index %d display_index %d settings %p\n", context, device_index,
          display_index, settings);

    if (!context || !settings)
        return AGS_INVALID_ARGS;

    settings506 = &settings->agsDisplaySettings506;
    settings600 = &settings->agsDisplaySettings600;

    create_dxgi_factory(&hdxgi, &dxgi_factory);
    if (!dxgi_factory)
        goto done;

    if (FAILED(IDXGIFactory1_QueryInterface(dxgi_factory, &IID_IDXGIVkInteropFactory1, (void**)&dxgi_interop)))
    {
        WARN("Failed to get IDXGIVkInteropFactory1.\n");
        goto done;
    }

    colorspace = context->version < AMD_AGS_VERSION_5_1_1
        ? convert_ags_colorspace_506(settings506->mode)
        : convert_ags_colorspace_600(settings600->mode);
    /* Settings 506, 511 and 600 are identical aside from enum order + use
     * of bitfield flags we do not use. */
    metadata = convert_ags_metadata(settings600);

    TRACE("chromacity: (%.6lf, %.6lf) (%.6lf, %.6lf) (%.6lf, %.6lf).\n", settings600->chromaticityRedX,
            settings600->chromaticityRedY, settings600->chromaticityGreenX, settings600->chromaticityGreenY,
            settings600->chromaticityBlueX, settings600->chromaticityBlueY);

    if (FAILED(IDXGIVkInteropFactory1_SetGlobalHDRState(dxgi_interop, colorspace, &metadata)))
        ret = AGS_DX_FAILURE;

done:
    if (dxgi_interop)
        IDXGIVkInteropFactory1_Release(dxgi_interop);
    release_dxgi_factory(hdxgi, dxgi_factory);
    return ret;
}

AGSReturnCode WINAPI agsGetCrossfireGPUCount(AGSContext *context, int *gpu_count)
{
    TRACE("context %p gpu_count %p stub!\n", context, gpu_count);

    if (!context || !gpu_count)
        return AGS_INVALID_ARGS;

    *gpu_count = 1;
    return AGS_SUCCESS;
}

struct AGSDriverVersionInfo
{
    char strDriverVersion[256];
    char strCatalystVersion[256];
    char strCatalystWebLink[256];
};

AGSReturnCode WINAPI agsGetDriverVersionInfo(AGSContext *context, struct AGSDriverVersionInfo *ver)
{
    TRACE("context %p, ver %p.\n", context, ver);

    if (!context || !ver)
        return AGS_INVALID_ARGS;

    strcpy(ver->strDriverVersion, driver_version);
    *ver->strCatalystVersion = 0;
    *ver->strCatalystWebLink = 0;
    return AGS_SUCCESS;
}

static void get_dx11_extensions_supported(ID3D11Device *device, AGSDX11ExtensionsSupported_600 *extensions)
{
    ID3D11VkExtDevice *ext_device;

    memset(extensions, 0, sizeof(*extensions));
    if (!amd_driver_extensions_available())
    {
        TRACE("AMD D3D11 driver extensions are unavailable for the selected graphics backend.\n");
        return;
    }

    if (FAILED(ID3D11Device_QueryInterface(device, &IID_ID3D11VkExtDevice, (void **)&ext_device)))
    {
        TRACE("No ID3D11VkExtDevice.\n");
        return;
    }

    extensions->depthBoundsTest = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_DEPTH_BOUNDS);
    extensions->uavOverlap = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_BARRIER_CONTROL);
    extensions->multiDrawIndirect = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_MULTI_DRAW_INDIRECT);
    extensions->multiDrawIndirectCountIndirect = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_MULTI_DRAW_INDIRECT_COUNT);
    extensions->UAVOverlapDeferredContexts = extensions->uavOverlap;

    ID3D11VkExtDevice_Release(ext_device);

    TRACE("extensions %#x.\n", *(unsigned int *)extensions);
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_CreateDevice( AGSContext* context,
        const AGSDX11DeviceCreationParams* creation_params, const AGSDX11ExtensionParams* extension_params,
        AGSDX11ReturnedParams* returned_params )
{
    const WCHAR *app_name = NULL, *engine_name = NULL;
    unsigned int app_version = 0, engine_version = 0;
    ID3D11DeviceContext *device_context;
    IDXGISwapChain *swapchain = NULL;
    D3D_FEATURE_LEVEL feature_level;
    ID3D11Device *device;
    HRESULT hr;

    if (!context || !creation_params || !returned_params)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    if (extension_params)
    {
        if (context->version < AMD_AGS_VERSION_5_2_0)
        {
            app_name = extension_params->agsDX11ExtensionParams511.pAppName;
            engine_name = extension_params->agsDX11ExtensionParams511.pEngineName;
            app_version = extension_params->agsDX11ExtensionParams511.appVersion;
            engine_version = extension_params->agsDX11ExtensionParams511.engineVersion;
        }
        else
        {
            app_name = extension_params->agsDX11ExtensionParams520.pAppName;
            engine_name = extension_params->agsDX11ExtensionParams520.pEngineName;
            app_version = extension_params->agsDX11ExtensionParams520.appVersion;
            engine_version = extension_params->agsDX11ExtensionParams520.engineVersion;
        }
    }

    TRACE("feature levels %u, pSwapChainDesc %p, app %s, engine %s %#x %#x.\n", creation_params->FeatureLevels,
            creation_params->pSwapChainDesc, debugstr_w(app_name), debugstr_w(engine_name),
            app_version, engine_version);

    if (!load_d3d11_functions())
    {
        ERR("Could not load d3d11.dll.\n");
        return AGS_MISSING_D3D_DLL;
    }
    memset( returned_params, 0, amd_ags_info[context->version].dx11_returned_params_size );
    if (creation_params->pSwapChainDesc)
    {
        hr = pD3D11CreateDeviceAndSwapChain(creation_params->pAdapter, creation_params->DriverType,
                creation_params->Software, creation_params->Flags, creation_params->pFeatureLevels,
                creation_params->FeatureLevels, creation_params->SDKVersion, creation_params->pSwapChainDesc,
                &swapchain, &device, &feature_level, &device_context);
    }
    else
    {
        hr = pD3D11CreateDevice(creation_params->pAdapter, creation_params->DriverType,
                creation_params->Software, creation_params->Flags, creation_params->pFeatureLevels,
                creation_params->FeatureLevels, creation_params->SDKVersion,
                &device, &feature_level, &device_context);
    }
    if (FAILED(hr))
    {
        ERR("Device creation failed, hr %#x.\n", hr);
        return AGS_DX_FAILURE;
    }

    get_dx11_extensions_supported(device, &context->extensions);

    if (context->version < AMD_AGS_VERSION_5_2_0)
    {
        AGSDX11ReturnedParams_511 *r = &returned_params->agsDX11ReturnedParams511;
        r->pDevice = device;
        r->pImmediateContext = device_context;
        r->pSwapChain = swapchain;
        r->FeatureLevel = feature_level;
        r->extensionsSupported = *(unsigned int *)&context->extensions;
    }
    else if (context->version < AMD_AGS_VERSION_6_0_0)
    {
        AGSDX11ReturnedParams_520 *r = &returned_params->agsDX11ReturnedParams520;
        r->pDevice = device;
        r->pImmediateContext = device_context;
        r->pSwapChain = swapchain;
        r->FeatureLevel = feature_level;
        r->extensionsSupported = *(unsigned int *)&context->extensions;
    }
    else
    {
        AGSDX11ReturnedParams_600 *r = &returned_params->agsDX11ReturnedParams600;
        r->pDevice = device;
        r->pImmediateContext = device_context;
        r->pSwapChain = swapchain;
        r->featureLevel = feature_level;
        r->extensionsSupported = context->extensions;
    }

    if (context->version < AMD_AGS_VERSION_5_3_0)
    {
        /* Later versions pass context to functions explicitly, no need to keep it. */
        if (context->d3d11_context)
            ID3D11DeviceContext_Release(context->d3d11_context);
        ID3D11DeviceContext_AddRef(device_context);
        context->d3d11_context = device_context;
    }

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_CreateDevice(AGSContext *context,
        const AGSDX12DeviceCreationParams *creation_params, const AGSDX12ExtensionParams *extension_params,
        AGSDX12ReturnedParams *returned_params)
{
    HRESULT hr;

    TRACE("context %p, creation_params %p, extension_params %p, returned_params %p.\n",
            context, creation_params, extension_params, returned_params);

    if (!context || !creation_params || !returned_params)
        return AGS_INVALID_ARGS;

    TRACE("feature level %#x, app %s, engine %s %#x %#x.\n", creation_params->FeatureLevel,
            debugstr_w(extension_params ? extension_params->pAppName : NULL),
            debugstr_w(extension_params ? extension_params->pEngineName : NULL),
            extension_params ? extension_params->appVersion : 0,
            extension_params ? extension_params->engineVersion : 0);

    if (!load_d3d12_functions())
    {
        ERR("Could not load d3d12.dll.\n");
        return AGS_MISSING_D3D_DLL;
    }

    memset(returned_params, 0, sizeof(*returned_params));
    if (FAILED(hr = pD3D12CreateDevice((IUnknown *)creation_params->pAdapter, creation_params->FeatureLevel,
            &creation_params->iid, (void **)&returned_params->pDevice)))
    {
        ERR("D3D12CreateDevice failed, hr %#x.\n", hr);
        return AGS_DX_FAILURE;
    }

    /* D3DMetal does not expose AMD driver extensions; the standard device is valid. */
    returned_params->extensionsSupported = 0;
    TRACE("Created d3d12 device %p.\n", returned_params->pDevice);

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_DestroyDevice(AGSContext* context, ID3D12Device* device, unsigned int* device_refs)
{
    ULONG ref_count;

    if (!device)
        return AGS_SUCCESS;

    ref_count = ID3D12Device_Release(device);
    if (device_refs)
        *device_refs = (unsigned int)ref_count;

    return AGS_SUCCESS;
}

AGSDriverVersionResult WINAPI agsCheckDriverVersion(const char* version_reported, unsigned int version_required)
{
    WARN("version_reported %s, version_required %d semi-stub.\n", debugstr_a(version_reported), version_required);

    return AGS_SOFTWAREVERSIONCHECK_OK;
}

int WINAPI agsGetVersionNumber(void)
{
    int public_version = 0;
    enum amd_ags_version version = determine_ags_version(&public_version);

    TRACE("version %s (internal %d).\n", debugstr_agsversion(public_version), version);

    return public_version;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_Init( AGSContext *context, ID3D11Device *device, unsigned int uavSlot, unsigned int *extensionsSupported )
{
    FIXME("context %p, device %p, uavSlot %u, extensionsSupported %p stub.\n", context, device, uavSlot, extensionsSupported);

    if (!context)
    {
        ERR("NULL context.\n");
        return AGS_INVALID_ARGS;
    }

    *extensionsSupported = 0;
    if (device)
    {
        if (context->version < AMD_AGS_VERSION_5_3_0)
        {
            /* Later versions pass context to functions explicitly, no need to keep it. */
            if (context->d3d11_context)
            {
                ID3D11DeviceContext_Release(context->d3d11_context);
                context->d3d11_context = NULL;
            }
            ID3D11Device_GetImmediateContext(device, &context->d3d11_context);
        }
        get_dx11_extensions_supported(device, &context->extensions);
        if (context->public_version <= AGS_MAKE_VERSION(3, 0, 0))
            *extensionsSupported = context->extensions.quadList | (context->extensions.uavOverlap << 1)
                    | (context->extensions.depthBoundsTest << 2) | (context->extensions.multiDrawIndirect << 3);
        else
            *extensionsSupported = *(unsigned int *)&context->extensions;
        TRACE("-> %#x.\n", *extensionsSupported);
    }

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensions_Init( AGSContext* context, ID3D11Device* device, unsigned int* extensionsSupported )
{
    TRACE("context %p, device %p, extensionsSupported %p.\n", context, device, extensionsSupported);

    return agsDriverExtensionsDX11_Init(context, device, ~0u, extensionsSupported);
}

AGSReturnCode WINAPI agsDriverExtensions_SetCrossfireMode(AGSContext *context, AGSCrossfireMode mode)
{
    FIXME("context %p, mode %d stub.\n", context, mode);

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_DeInit( AGSContext* context )
{
    TRACE("context %p.\n", context);

    if (context->d3d11_context)
    {
        ID3D11DeviceContext_Release(context->d3d11_context);
        context->d3d11_context = NULL;
    }

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensions_DeInit(AGSContext *context)
{
    return agsDriverExtensionsDX11_DeInit(context);
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_Init( AGSContext* context, ID3D12Device* device, unsigned int* extensionsSupported )
{
    FIXME("context %p, device %p, extensionsSupported %p stub.\n", context, device, extensionsSupported);

    if (!context || !device)
        return AGS_INVALID_ARGS;
    if (extensionsSupported)
        *extensionsSupported = 0;
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_DeInit( AGSContext* context )
{
    TRACE("context %p.\n", context);

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI  agsDriverExtensionsDX12_SetMarker( AGSContext *context, ID3D12GraphicsCommandList *command_list, const char *data)
{
    WARN("context %p, command_list %p, data %p stub.\n", context, command_list, data);

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_PushMarker( AGSContext *context, ID3D12GraphicsCommandList *command_list, const char* data)
{
    WARN("context %p, command_list %p, data %p stub.\n", context, command_list, data);

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_PopMarker(AGSContext *context, ID3D12GraphicsCommandList *command_list)
{
    WARN("context %p, command_list %p stub.\n", context, command_list);

    return AGS_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    TRACE("%p, %u, %p.\n", instance, reason, reserved);

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            break;
    }

    return TRUE;
}

#ifdef __x86_64__

C_ASSERT(AGS_INVALID_ARGS == 2);

static AGSReturnCode set_depth_bounds(AGSContext* context, ID3D11DeviceContext *dx_context, bool enabled,
        float min_depth, float max_depth)
{
    ID3D11VkExtContext *ext_context;

    if (!context->extensions.depthBoundsTest)
        return AGS_EXTENSION_NOT_SUPPORTED;

    if (FAILED(ID3D11DeviceContext_QueryInterface(dx_context, &IID_ID3D11VkExtContext, (void **)&ext_context)))
    {
        TRACE("No ID3D11VkExtContext.\n");
        return AGS_EXTENSION_NOT_SUPPORTED;
    }
    ID3D11VkExtContext_SetDepthBoundsTest(ext_context, enabled, min_depth, max_depth);
    ID3D11VkExtContext_Release(ext_context);
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_SetDepthBounds(AGSContext* context, bool enabled,
        float min_depth, float max_depth )
{
    TRACE("context %p, enabled %d, min_depth %f, max_depth %f.\n", context, enabled, min_depth, max_depth);

    if (!context || !context->d3d11_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    return set_depth_bounds(context, context->d3d11_context, enabled, min_depth, max_depth);
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_SetDepthBounds_530(AGSContext* context,
        ID3D11DeviceContext* dx_context, bool enabled, float min_depth, float max_depth )
{
    TRACE("context %p, dx_context %p, enabled %d, min_depth %f, max_depth %f.\n", context, dx_context, enabled,
            min_depth, max_depth);

    if (!context || !dx_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    return set_depth_bounds(context, dx_context, enabled, min_depth, max_depth);
}

AGSReturnCode WINAPI ags_invalid_context(void);

C_ASSERT(AMD_AGS_VERSION_5_3_0 == 4);
AGSReturnCode WINAPI ags_invalid_context(void)
{
    return AGS_INVALID_ARGS;
}

__ASM_GLOBAL_FUNC( DX11_SetDepthBounds_impl,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $4,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_SetDepthBounds") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_SetDepthBounds_530") )

static AGSReturnCode update_uav_overlap(AGSContext* context, ID3D11DeviceContext *dx_context, BOOL set)
{
    ID3D11VkExtContext *ext_context;

    if (!context->extensions.uavOverlap)
        return AGS_EXTENSION_NOT_SUPPORTED;

    if (FAILED(ID3D11DeviceContext_QueryInterface(dx_context, &IID_ID3D11VkExtContext, (void **)&ext_context)))
    {
        TRACE("No ID3D11VkExtContext.\n");
        return AGS_EXTENSION_NOT_SUPPORTED;
    }

    ID3D11VkExtContext_SetBarrierControl(ext_context, set ? D3D11_VK_BARRIER_CONTROL_IGNORE_WRITE_AFTER_WRITE : 0);
    ID3D11VkExtContext_Release(ext_context);
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_BeginUAVOverlap_520(AGSContext *context)
{
    TRACE("context %p.\n", context);

    if (!context || !context->d3d11_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    return update_uav_overlap(context, context->d3d11_context, TRUE);
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_BeginUAVOverlap(AGSContext *context, ID3D11DeviceContext *dx_context)
{
    TRACE("context %p, dx_context %p.\n", context, dx_context);

    if (!context || !dx_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    return update_uav_overlap(context, dx_context, TRUE);
}

C_ASSERT(AMD_AGS_VERSION_5_3_0 == 4);
__ASM_GLOBAL_FUNC( DX11_BeginUAVOverlap_impl,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $4,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_BeginUAVOverlap_520") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_BeginUAVOverlap") )

AGSReturnCode WINAPI agsDriverExtensionsDX11_EndUAVOverlap_520(AGSContext *context)
{
    TRACE("context %p.\n", context);

    if (!context || !context->d3d11_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    return update_uav_overlap(context, context->d3d11_context, FALSE);
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_EndUAVOverlap(AGSContext *context, ID3D11DeviceContext *dx_context)
{
    TRACE("context %p, dx_context %p.\n", context, dx_context);

    if (!context || !dx_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    return update_uav_overlap(context, dx_context, FALSE);
}

C_ASSERT(AMD_AGS_VERSION_5_3_0 == 4);
__ASM_GLOBAL_FUNC( DX11_EndUAVOverlap_impl,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $4,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_EndUAVOverlap_520") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_EndUAVOverlap") )

AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect(AGSContext *context, ID3D11DeviceContext *dx_context,
        unsigned int draw_count, ID3D11Buffer *buffer_for_args, unsigned int aligned_byte_offset_for_args,
        unsigned int byte_stride_for_args)
{
    ID3D11VkExtContext *ext_context;

    TRACE("context %p, dx_context %p, draw_count %u, buffer_for_args %p, aligned_byte_offset_for_args %u, byte_stride_for_args %u.\n",
            context, dx_context, draw_count, buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);

    if (!context || !dx_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    if (!context->extensions.multiDrawIndirect)
        return AGS_EXTENSION_NOT_SUPPORTED;

    if (FAILED(ID3D11DeviceContext_QueryInterface(dx_context, &IID_ID3D11VkExtContext, (void **)&ext_context)))
    {
        TRACE("No ID3D11VkExtContext.\n");
        return AGS_EXTENSION_NOT_SUPPORTED;
    }

    ID3D11VkExtContext_MultiDrawIndexedIndirect(ext_context, draw_count, buffer_for_args, aligned_byte_offset_for_args,
            byte_stride_for_args);
    ID3D11VkExtContext_Release(ext_context);
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect_520(AGSContext *context,
        unsigned int draw_count, ID3D11Buffer *buffer_for_args, unsigned int aligned_byte_offset_for_args,
        unsigned int byte_stride_for_args)
{
    if (!context || !context->d3d11_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }
    return agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect(context, context->d3d11_context, draw_count,
            buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);
}

C_ASSERT(AMD_AGS_VERSION_5_3_0 == 4);
__ASM_GLOBAL_FUNC( DX11_MultiDrawIndexedInstancedIndirect_impl,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $4,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect_520") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect") )


AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawInstancedIndirect(AGSContext *context, ID3D11DeviceContext *dx_context,
        unsigned int draw_count, ID3D11Buffer *buffer_for_args, unsigned int aligned_byte_offset_for_args,
        unsigned int byte_stride_for_args)
{
    ID3D11VkExtContext *ext_context;

    TRACE("context %p, dx_context %p, draw_count %u, buffer_for_args %p, aligned_byte_offset_for_args %u, byte_stride_for_args %u.\n",
            context, dx_context, draw_count, buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);

    if (!context || !dx_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    if (!context->extensions.multiDrawIndirect)
        return AGS_EXTENSION_NOT_SUPPORTED;

    if (FAILED(ID3D11DeviceContext_QueryInterface(dx_context, &IID_ID3D11VkExtContext, (void **)&ext_context)))
    {
        TRACE("No ID3D11VkExtContext.\n");
        return AGS_EXTENSION_NOT_SUPPORTED;
    }

    ID3D11VkExtContext_MultiDrawIndirect(ext_context, draw_count, buffer_for_args, aligned_byte_offset_for_args,
            byte_stride_for_args);
    ID3D11VkExtContext_Release(ext_context);
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawInstancedIndirect_520( AGSContext* context, unsigned int draw_count,
        ID3D11Buffer *buffer_for_args, unsigned int aligned_byte_offset_for_args, unsigned int byte_stride_for_args)
{
    if (!context || !context->d3d11_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }
    return agsDriverExtensionsDX11_MultiDrawInstancedIndirect(context, context->d3d11_context, draw_count,
            buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);
}

C_ASSERT(AMD_AGS_VERSION_5_3_0 == 4);
__ASM_GLOBAL_FUNC( DX11_MultiDrawInstancedIndirect_impl,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $4,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawInstancedIndirect_520") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawInstancedIndirect") )

static unsigned int get_max_draw_count(ID3D11Buffer *args, unsigned int offset, unsigned int stride, unsigned int size)
{
    D3D11_BUFFER_DESC desc;
    unsigned int count;

    ID3D11Buffer_GetDesc(args, &desc);
    if (offset >= desc.ByteWidth)
    {
        WARN("offset %u, buffer size %u.\n", offset, desc.ByteWidth);
        return 0;
    }
    count = (desc.ByteWidth - offset) / stride;
    if (desc.ByteWidth - offset - count * stride >= size)
        ++count;
    if (!count)
        WARN("zero count, buffer size %u, offset %u, stride %u, size %u.\n", desc.ByteWidth, offset, stride, size);
    return count;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirectCountIndirect(AGSContext *context, ID3D11DeviceContext *dx_context,
        ID3D11Buffer *buffer_for_draw_count, unsigned int aligned_byte_offset_for_draw_count, ID3D11Buffer *buffer_for_args,
        unsigned int aligned_byte_offset_for_args, unsigned int byte_stride_for_args)
{
    ID3D11VkExtContext *ext_context;
    unsigned int max_draw_count;

    TRACE("context %p, dx_context %p, count buffer %p, offset %u, args buffer %p, offset %u, stride %u.\n",
            context, dx_context, buffer_for_draw_count, aligned_byte_offset_for_draw_count, buffer_for_args,
            aligned_byte_offset_for_args, byte_stride_for_args);

    if (!context || !dx_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    if (!context->extensions.multiDrawIndirectCountIndirect)
        return AGS_EXTENSION_NOT_SUPPORTED;

    if (FAILED(ID3D11DeviceContext_QueryInterface(dx_context, &IID_ID3D11VkExtContext, (void **)&ext_context)))
    {
        TRACE("No ID3D11VkExtContext.\n");
        return AGS_EXTENSION_NOT_SUPPORTED;
    }

    max_draw_count = get_max_draw_count(buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args, sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS));
    ID3D11VkExtContext_MultiDrawIndexedIndirectCount(ext_context, max_draw_count, buffer_for_draw_count, aligned_byte_offset_for_draw_count,
            buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);
    ID3D11VkExtContext_Release(ext_context);
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirectCountIndirect_520(AGSContext *context,
        ID3D11Buffer *buffer_for_draw_count, unsigned int aligned_byte_offset_for_draw_count, ID3D11Buffer *buffer_for_args,
        unsigned int aligned_byte_offset_for_args, unsigned int byte_stride_for_args)
{
    if (!context || !context->d3d11_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }
    return agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirectCountIndirect(context, context->d3d11_context,
            buffer_for_draw_count, aligned_byte_offset_for_draw_count,
            buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);
}

C_ASSERT(AMD_AGS_VERSION_5_3_0 == 4);
__ASM_GLOBAL_FUNC( DX11_MultiDrawIndexedInstancedIndirectCountIndirect_impl,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $4,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirectCountIndirect_520") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirectCountIndirect") )


AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawInstancedIndirectCountIndirect(AGSContext *context, ID3D11DeviceContext *dx_context,
        ID3D11Buffer *buffer_for_draw_count, unsigned int aligned_byte_offset_for_draw_count, ID3D11Buffer *buffer_for_args,
        unsigned int aligned_byte_offset_for_args, unsigned int byte_stride_for_args)
{
    ID3D11VkExtContext *ext_context;
    unsigned int max_draw_count;

    TRACE("context %p, dx_context %p, count buffer %p, offset %u, args buffer %p, offset %u, stride %u.\n",
            context, dx_context, buffer_for_draw_count, aligned_byte_offset_for_draw_count, buffer_for_args,
            aligned_byte_offset_for_args, byte_stride_for_args);

    if (!context || !dx_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }

    if (!context->extensions.multiDrawIndirectCountIndirect)
        return AGS_EXTENSION_NOT_SUPPORTED;

    if (FAILED(ID3D11DeviceContext_QueryInterface(dx_context, &IID_ID3D11VkExtContext, (void **)&ext_context)))
    {
        TRACE("No ID3D11VkExtContext.\n");
        return AGS_EXTENSION_NOT_SUPPORTED;
    }

    max_draw_count = get_max_draw_count(buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args, sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS));
    ID3D11VkExtContext_MultiDrawIndirectCount(ext_context, max_draw_count, buffer_for_draw_count, aligned_byte_offset_for_draw_count,
            buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);
    ID3D11VkExtContext_Release(ext_context);
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_MultiDrawInstancedIndirectCountIndirect_520(AGSContext *context,
        ID3D11Buffer *buffer_for_draw_count, unsigned int aligned_byte_offset_for_draw_count, ID3D11Buffer *buffer_for_args,
        unsigned int aligned_byte_offset_for_args, unsigned int byte_stride_for_args)
{
    if (!context || !context->d3d11_context)
    {
        WARN("Invalid arguments.\n");
        return AGS_INVALID_ARGS;
    }
    return agsDriverExtensionsDX11_MultiDrawInstancedIndirectCountIndirect(context, context->d3d11_context,
            buffer_for_draw_count, aligned_byte_offset_for_draw_count,
            buffer_for_args, aligned_byte_offset_for_args, byte_stride_for_args);
}

C_ASSERT(AMD_AGS_VERSION_5_3_0 == 4);
__ASM_GLOBAL_FUNC( DX11_MultiDrawInstancedIndirectCountIndirect_impl,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $4,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawInstancedIndirectCountIndirect_520") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_MultiDrawInstancedIndirectCountIndirect") )

AGSReturnCode WINAPI agsDriverExtensionsDX11_DestroyDevice_520(AGSContext *context, ID3D11Device* device,
        unsigned int *device_ref, ID3D11DeviceContext *device_context,
        unsigned int *context_ref)
{
    ULONG ref;

    if (!context)
        return AGS_INVALID_ARGS;

    TRACE("context %p, device %p, device_ref %p, device_context %p, context_ref %p.\n",
            context, device, device_ref, device_context, context_ref);

    if (!device)
        return AGS_SUCCESS;

    if (context->d3d11_context)
    {
        ID3D11DeviceContext_Release(context->d3d11_context);
        context->d3d11_context = NULL;
    }

    ref = ID3D11Device_Release(device);
    if (device_ref)
        *device_ref = ref;

    if (!device_context)
        return AGS_SUCCESS;

    ref = ID3D11DeviceContext_Release(device_context);
    if (context_ref)
        *context_ref = ref;
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_DestroyDevice_511(AGSContext *context, ID3D11Device *device,
        unsigned int *references )
{
    TRACE("context %p, device %p, references %p.\n", context, device, references);

    return agsDriverExtensionsDX11_DestroyDevice_520(context, device, references, NULL, NULL);
}

C_ASSERT(AMD_AGS_VERSION_5_2_0 == 3);
__ASM_GLOBAL_FUNC( agsDriverExtensionsDX11_DestroyDevice,
                   "test %rcx,%rcx\n\t"
                   "jz " __ASM_NAME("ags_invalid_context") "\n\t"
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $3,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp "     __ASM_NAME("agsDriverExtensionsDX11_DestroyDevice_511") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_DestroyDevice_520") )
#endif

AGSReturnCode WINAPI agsDriverExtensionsDX11_SetDiskShaderCacheEnabled(AGSContext *context, int enable)
{
    FIXME("context %p, enable %d stub.\n", context, enable);
    return AGS_SUCCESS;
}
