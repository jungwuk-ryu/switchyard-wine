/*
 * Bounded DXMT D3D11 acceptance client.
 *
 * The client validates the loaded provider image, clears a swap-chain render
 * target, exercises multi-page dynamic WRITE_DISCARD buffer and texture
 * mappings, reads the known pixels back through a staging texture, and
 * presents the frame.  The shell harness additionally proves that the native
 * arm64 winemetal Unix library and the selected native CPU-provider Unix
 * library were loaded and that DXMT emitted its device log.
 */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <psapi.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef IMAGE_FILE_MACHINE_ARM64EC
#define IMAGE_FILE_MACHINE_ARM64EC 0xa641
#endif

#define TEST_WIDTH 64
#define TEST_HEIGHT 64
#define DYNAMIC_BUFFER_SIZE (257 * 1024 + 64)
#define DYNAMIC_TEXTURE_WIDTH 257
#define DYNAMIC_TEXTURE_HEIGHT 17
#define DYNAMIC_TEXTURE_ROW_SIZE (DYNAMIC_TEXTURE_WIDTH * 4)
#define INSPECTION_NONCE_LENGTH 36

static int inspection_nonce_is_valid(const char *nonce)
{
    SIZE_T index;

    if (strlen(nonce) != INSPECTION_NONCE_LENGTH) return 0;
    for (index = 0; index < INSPECTION_NONCE_LENGTH; ++index)
    {
        char character = nonce[index];

        if (index == 8 || index == 13 || index == 18 || index == 23)
        {
            if (character != '-') return 0;
        }
        else if (!((character >= '0' && character <= '9') ||
                   (character >= 'A' && character <= 'F')))
            return 0;
    }
    return 1;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int range_is_inside(SIZE_T offset, SIZE_T length, SIZE_T total)
{
    return offset <= total && length <= total - offset;
}

static int contains_dxmt_marker(const BYTE *data, SIZE_T size)
{
    static const BYTE marker[] = {'D', 'X', 'M', 'T'};
    SIZE_T index;

    if (size < sizeof(marker)) return 0;
    for (index = 0; index <= size - sizeof(marker); ++index)
        if (!memcmp(data + index, marker, sizeof(marker))) return 1;
    return 0;
}

#ifdef _WIN64
static int image_is_arm64ec(const BYTE *base, const IMAGE_NT_HEADERS64 *nt)
{
    const IMAGE_DATA_DIRECTORY *directory;
    const BYTE *load_config, *metadata, *code_map;
    ULONGLONG metadata_address;
    DWORD load_config_size, metadata_version, code_map_rva, code_map_count;
    DWORD index;
    SIZE_T image_size = nt->OptionalHeader.SizeOfImage;

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    if (directory->Size < 0xd0 || !range_is_inside(directory->VirtualAddress, 0xd0, image_size))
        return 0;
    load_config = base + directory->VirtualAddress;
    memcpy(&load_config_size, load_config, sizeof(load_config_size));
    if (load_config_size < 0xd0) return 0;
    memcpy(&metadata_address, load_config + 0xc8, sizeof(metadata_address));
    if (metadata_address < (ULONGLONG)(ULONG_PTR)base) return 0;
    if (!range_is_inside((SIZE_T)(metadata_address - (ULONGLONG)(ULONG_PTR)base), 12, image_size))
        return 0;
    metadata = (const BYTE *)(ULONG_PTR)metadata_address;
    memcpy(&metadata_version, metadata, sizeof(metadata_version));
    memcpy(&code_map_rva, metadata + 4, sizeof(code_map_rva));
    memcpy(&code_map_count, metadata + 8, sizeof(code_map_count));
    if ((metadata_version != 1 && metadata_version != 2) ||
        !code_map_count || code_map_count > image_size / 8 ||
        !range_is_inside(code_map_rva, (SIZE_T)code_map_count * 8, image_size))
        return 0;
    code_map = base + code_map_rva;
    for (index = 0; index < code_map_count; ++index)
    {
        DWORD start;
        memcpy(&start, code_map + index * 8, sizeof(start));
        if (start & 1) return 1;
    }
    return 0;
}
#endif

static int validate_dxmt_module(void)
{
    HMODULE module = GetModuleHandleW(L"d3d11.dll");
    MODULEINFO module_info;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_SECTION_HEADER *section;
    const BYTE *base;
    SIZE_T image_size, nt_offset, section_offset, section_bytes;
    WORD index;
    int marker_found = 0;

    if (!module)
    {
        fprintf(stderr, "D3D11 module is not loaded.\n");
        return 0;
    }
    memset(&module_info, 0, sizeof(module_info));
    if (!GetModuleInformation(GetCurrentProcess(), module, &module_info, sizeof(module_info)) ||
        !module_info.lpBaseOfDll || module_info.SizeOfImage < sizeof(*dos))
    {
        fprintf(stderr, "Cannot determine the loaded D3D11 image bounds: %lu.\n",
                GetLastError());
        return 0;
    }
    base = module_info.lpBaseOfDll;
    image_size = module_info.SizeOfImage;
    dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
    {
        fprintf(stderr, "Loaded D3D11 module has no valid DOS header.\n");
        return 0;
    }
    nt_offset = dos->e_lfanew;
    if (!range_is_inside(nt_offset, FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader), image_size))
    {
        fprintf(stderr, "Loaded D3D11 module has an out-of-range PE header.\n");
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS *)(base + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        fprintf(stderr, "Loaded D3D11 module has no valid PE header.\n");
        return 0;
    }
    if (nt->FileHeader.SizeOfOptionalHeader < sizeof(nt->OptionalHeader) ||
        !range_is_inside(nt_offset, FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
                        nt->FileHeader.SizeOfOptionalHeader, image_size) ||
        !nt->OptionalHeader.SizeOfImage || nt->OptionalHeader.SizeOfImage > image_size)
    {
        fprintf(stderr, "Loaded D3D11 module has an invalid optional header.\n");
        return 0;
    }
    image_size = nt->OptionalHeader.SizeOfImage;
#ifdef _WIN64
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64EC &&
        (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
         !image_is_arm64ec(base, (const IMAGE_NT_HEADERS64 *)nt)))
    {
        fprintf(stderr, "Loaded x86_64 D3D11 module is not ARM64EC (machine %#x).\n",
                (unsigned int)nt->FileHeader.Machine);
        return 0;
    }
#else
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
    {
        fprintf(stderr, "Loaded i386 D3D11 module has machine %#x.\n",
                (unsigned int)nt->FileHeader.Machine);
        return 0;
    }
#endif

    section = IMAGE_FIRST_SECTION(nt);
    section_offset = (const BYTE *)section - base;
    if (nt->FileHeader.NumberOfSections > image_size / sizeof(*section))
    {
        fprintf(stderr, "Loaded D3D11 module has too many PE sections.\n");
        return 0;
    }
    section_bytes = (SIZE_T)nt->FileHeader.NumberOfSections * sizeof(*section);
    if (!range_is_inside(section_offset, section_bytes, image_size))
    {
        fprintf(stderr, "Loaded D3D11 module has an out-of-range section table.\n");
        return 0;
    }
    for (index = 0; index < nt->FileHeader.NumberOfSections; ++index)
    {
        SIZE_T offset = section[index].VirtualAddress;
        SIZE_T size = section[index].Misc.VirtualSize;

        if (!(section[index].Characteristics & IMAGE_SCN_MEM_READ) ||
            !range_is_inside(offset, size, image_size))
            continue;
        if (contains_dxmt_marker(base + offset, size))
        {
            marker_found = 1;
            break;
        }
    }
    if (!marker_found)
    {
        fprintf(stderr, "Loaded D3D11 module has no DXMT identity marker.\n");
        return 0;
    }
    printf("Loaded DXMT D3D11 provider machine %#x.\n",
           (unsigned int)nt->FileHeader.Machine);
    return 1;
}

static int byte_near(BYTE actual, BYTE expected)
{
    int difference = (int)actual - (int)expected;
    if (difference < 0) difference = -difference;
    return difference <= 3;
}

int main(int argc, char **argv)
{
    static const WCHAR class_name[] = L"SwitchyardDXMTD3D11Acceptance";
    static const float clear_color[] = {0.25f, 0.50f, 0.75f, 1.0f};
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };
    ID3D11RenderTargetView *render_target = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11Texture2D *readback = NULL;
    ID3D11Texture2D *dynamic_texture = NULL;
    ID3D11Buffer *dynamic_buffer = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Device *device = NULL;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D_FEATURE_LEVEL feature_level;
    DXGI_SWAP_CHAIN_DESC swapchain_desc;
    WNDCLASSW window_class;
    HWND window = NULL;
    HRESULT result;
    unsigned int sample;
    SIZE_T offset;
    const char *inspection_nonce = NULL;
    int status = 1, wait_for_image_inspection = 0;

    if (argc == 3 && !strcmp(argv[1], "--wait-for-loaded-image-inspection") &&
        inspection_nonce_is_valid(argv[2]))
    {
        wait_for_image_inspection = 1;
        inspection_nonce = argv[2];
    }
    else if (argc != 1)
    {
        fprintf(stderr, "usage: %s [--wait-for-loaded-image-inspection NONCE]\n", argv[0]);
        return 2;
    }

    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        fprintf(stderr, "RegisterClassW failed: %lu.\n", GetLastError());
        return 1;
    }
    window = CreateWindowW(class_name, L"Switchyard DXMT D3D11 acceptance",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           320, 240, NULL, NULL, window_class.hInstance, NULL);
    if (!window)
    {
        fprintf(stderr, "CreateWindowW failed: %lu.\n", GetLastError());
        goto done;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    memset(&swapchain_desc, 0, sizeof(swapchain_desc));
    swapchain_desc.BufferDesc.Width = TEST_WIDTH;
    swapchain_desc.BufferDesc.Height = TEST_HEIGHT;
    swapchain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = 2;
    swapchain_desc.OutputWindow = window;
    swapchain_desc.Windowed = TRUE;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &swapchain_desc,
            &swapchain, &device, &feature_level, &context);
    if (result == E_INVALIDARG)
        result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                0, levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION,
                &swapchain_desc, &swapchain, &device, &feature_level, &context);
    if (FAILED(result))
    {
        fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    if (!swapchain || !device || !context)
    {
        fprintf(stderr, "D3D11CreateDeviceAndSwapChain returned incomplete interfaces.\n");
        goto done;
    }
    if (!validate_dxmt_module()) goto done;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.ByteWidth = DYNAMIC_BUFFER_SIZE;
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = ID3D11Device_CreateBuffer(device, &buffer_desc, NULL, &dynamic_buffer);
    if (FAILED(result))
    {
        fprintf(stderr, "Creating the D3D11 dynamic buffer failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    if (!dynamic_buffer)
    {
        fprintf(stderr, "Creating the D3D11 dynamic buffer returned no interface.\n");
        goto done;
    }
    memset(&mapped, 0, sizeof(mapped));
    result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)dynamic_buffer, 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result))
    {
        fprintf(stderr, "Mapping the D3D11 dynamic buffer for WRITE_DISCARD failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    if (!mapped.pData)
    {
        fprintf(stderr, "Mapping the D3D11 dynamic buffer returned no writable address.\n");
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)dynamic_buffer, 0);
        goto done;
    }
    for (offset = 0; offset < DYNAMIC_BUFFER_SIZE; ++offset)
        ((BYTE *)mapped.pData)[offset] = (BYTE)((offset * 131 + 17) & 0xff);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)dynamic_buffer, 0);
    ID3D11Buffer_Release(dynamic_buffer);
    dynamic_buffer = NULL;
    printf("DXMT D3D11 dynamic WRITE_DISCARD/unmap/release passed for %u bytes.\n",
           (unsigned int)DYNAMIC_BUFFER_SIZE);

    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.Width = DYNAMIC_TEXTURE_WIDTH;
    texture_desc.Height = DYNAMIC_TEXTURE_HEIGHT;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DYNAMIC;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = ID3D11Device_CreateTexture2D(device, &texture_desc, NULL, &dynamic_texture);
    if (FAILED(result))
    {
        fprintf(stderr, "Creating the D3D11 dynamic texture failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    if (!dynamic_texture)
    {
        fprintf(stderr, "Creating the D3D11 dynamic texture returned no interface.\n");
        goto done;
    }
    memset(&mapped, 0, sizeof(mapped));
    result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)dynamic_texture, 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result))
    {
        fprintf(stderr, "Mapping the D3D11 dynamic texture for WRITE_DISCARD failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    if (!mapped.pData || mapped.RowPitch < DYNAMIC_TEXTURE_ROW_SIZE)
    {
        fprintf(stderr, "Mapping the D3D11 dynamic texture returned invalid bounds.\n");
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)dynamic_texture, 0);
        goto done;
    }
    for (sample = 0; sample < DYNAMIC_TEXTURE_HEIGHT; ++sample)
        for (offset = 0; offset < DYNAMIC_TEXTURE_ROW_SIZE; ++offset)
            ((BYTE *)mapped.pData)[sample * mapped.RowPitch + offset] =
                (BYTE)((sample * 29 + offset * 17 + 3) & 0xff);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)dynamic_texture, 0);
    ID3D11Texture2D_Release(dynamic_texture);
    dynamic_texture = NULL;
    printf("DXMT D3D11 dynamic texture WRITE_DISCARD/unmap/release passed for %ux%u pixels.\n",
           (unsigned int)DYNAMIC_TEXTURE_WIDTH, (unsigned int)DYNAMIC_TEXTURE_HEIGHT);

    result = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D,
                                      (void **)&back_buffer);
    if (FAILED(result) || !back_buffer ||
        FAILED(result = ID3D11Device_CreateRenderTargetView(
            device, (ID3D11Resource *)back_buffer, NULL, &render_target)) ||
        !render_target)
    {
        fprintf(stderr, "Creating the D3D11 render target failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &render_target, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(context, render_target, clear_color);

    ID3D11Texture2D_GetDesc(back_buffer, &texture_desc);
    texture_desc.Usage = D3D11_USAGE_STAGING;
    texture_desc.BindFlags = 0;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texture_desc.MiscFlags = 0;
    result = ID3D11Device_CreateTexture2D(device, &texture_desc, NULL, &readback);
    if (FAILED(result))
    {
        fprintf(stderr, "Creating the D3D11 readback texture failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    if (!readback)
    {
        fprintf(stderr, "Creating the D3D11 readback texture returned no interface.\n");
        goto done;
    }
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback,
                                     (ID3D11Resource *)back_buffer);
    ID3D11DeviceContext_Flush(context);
    memset(&mapped, 0, sizeof(mapped));
    result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0,
                                    D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        fprintf(stderr, "Mapping the D3D11 readback texture failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    if (!mapped.pData || mapped.RowPitch < TEST_WIDTH * 4
#ifndef _WIN64
        || mapped.RowPitch > SIZE_MAX / TEST_HEIGHT
#endif
       )
    {
        fprintf(stderr, "Mapping the D3D11 readback texture returned invalid bounds.\n");
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
        goto done;
    }
    for (sample = 0; sample < 4; ++sample)
    {
        static const unsigned int x[] = {0, TEST_WIDTH - 1, 0, TEST_WIDTH / 2};
        static const unsigned int y[] = {0, 0, TEST_HEIGHT - 1, TEST_HEIGHT / 2};
        const BYTE *pixel = (const BYTE *)mapped.pData + y[sample] * mapped.RowPitch + x[sample] * 4;

        if (!byte_near(pixel[0], 64) || !byte_near(pixel[1], 128) ||
            !byte_near(pixel[2], 191) || !byte_near(pixel[3], 255))
        {
            fprintf(stderr, "DXMT readback mismatch at %u,%u: %u %u %u %u.\n",
                    x[sample], y[sample], (unsigned int)pixel[0],
                    (unsigned int)pixel[1], (unsigned int)pixel[2],
                    (unsigned int)pixel[3]);
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
            goto done;
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);

    result = IDXGISwapChain_Present(swapchain, 0, 0);
    if (FAILED(result))
    {
        fprintf(stderr, "DXMT swap-chain present failed: %#lx.\n",
                (unsigned long)result);
        goto done;
    }
    printf("DXMT D3D11 render/readback/present passed at feature level %#x.\n",
           (unsigned int)feature_level);
    if (wait_for_image_inspection)
    {
        printf("DXMT loaded-image inspection ready: %s\n", inspection_nonce);
        if (fflush(stdout) == EOF)
        {
            fprintf(stderr, "Flushing the loaded-image inspection marker failed.\n");
            goto done;
        }
        if (getchar() != '\n')
        {
            fprintf(stderr, "Loaded-image inspection received an invalid release.\n");
            goto done;
        }
    }
    status = 0;

done:
    if (dynamic_buffer) ID3D11Buffer_Release(dynamic_buffer);
    if (dynamic_texture) ID3D11Texture2D_Release(dynamic_texture);
    if (readback) ID3D11Texture2D_Release(readback);
    if (render_target) ID3D11RenderTargetView_Release(render_target);
    if (back_buffer) ID3D11Texture2D_Release(back_buffer);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    if (swapchain) IDXGISwapChain_Release(swapchain);
    if (window) DestroyWindow(window);
    UnregisterClassW(class_name, window_class.hInstance);
    return status;
}
