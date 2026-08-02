/*
 * Native macOS EDR capability and presentation probe.
 *
 * This program deliberately uses only public AppKit, CoreGraphics, Metal,
 * and QuartzCore interfaces.  Headroom values are dimensionless color
 * component multipliers; they are not luminance measurements in nits.
 *
 * Build without ARC.  All owned Objective-C and Core Foundation objects are
 * released explicitly so this can be compiled with Wine's MRC toolchain.
 */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreGraphics/CGDirectDisplayMetal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *const unavailable = @"unavailable";

typedef struct
{
    BOOL json;
    BOOL pattern;
    NSTimeInterval pattern_seconds;
    BOOL display_id_set;
    CGDirectDisplayID display_id;
} probe_options;

typedef struct
{
    float rect[4];
    float color[4];
} pattern_patch;

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s [--json] [--pattern [SECONDS]] [--seconds SECONDS] "
            "[--display CG_DISPLAY_ID]\n",
            program);
}

static BOOL parse_double(const char *text, double *value)
{
    char *end;
    double parsed;

    errno = 0;
    end = NULL;
    parsed = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(parsed)) return NO;
    *value = parsed;
    return YES;
}

static BOOL parse_display_id(const char *text, CGDirectDisplayID *value)
{
    char *end;
    unsigned long long parsed;

    errno = 0;
    end = NULL;
    parsed = strtoull(text, &end, 0);
    if (errno || end == text || *end || parsed > UINT32_MAX || parsed == kCGNullDirectDisplay)
        return NO;
    *value = (CGDirectDisplayID)parsed;
    return YES;
}

static int parse_options(int argc, const char *argv[], probe_options *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    options->pattern_seconds = 5.0;

    for (i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--json"))
        {
            options->json = YES;
        }
        else if (!strcmp(argv[i], "--pattern"))
        {
            double seconds;

            options->pattern = YES;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                if (!parse_double(argv[++i], &seconds))
                {
                    fprintf(stderr, "invalid --pattern duration: %s\n", argv[i]);
                    return 2;
                }
                options->pattern_seconds = seconds;
            }
        }
        else if (!strcmp(argv[i], "--seconds"))
        {
            double seconds;

            if (++i >= argc || !parse_double(argv[i], &seconds))
            {
                fprintf(stderr, "--seconds requires a finite number\n");
                return 2;
            }
            options->pattern_seconds = seconds;
        }
        else if (!strcmp(argv[i], "--display"))
        {
            if (++i >= argc || !parse_display_id(argv[i], &options->display_id))
            {
                fprintf(stderr, "--display requires a nonzero 32-bit CGDirectDisplayID\n");
                return 2;
            }
            options->display_id_set = YES;
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(stdout, argv[0]);
            return -1;
        }
        else
        {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (options->pattern_seconds < 0.25 || options->pattern_seconds > 30.0)
    {
        fprintf(stderr, "pattern duration must be between 0.25 and 30 seconds\n");
        return 2;
    }
    if (!options->pattern && options->display_id_set)
    {
        fprintf(stderr, "--display is only meaningful with --pattern\n");
        return 2;
    }
    return 0;
}

static CGDirectDisplayID screen_display_id(NSScreen *screen)
{
#if defined(MAC_OS_VERSION_26_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_26_0
    if (@available(macOS 26.0, *)) return [screen CGDirectDisplayID];
#endif

    /* NSScreenNumber is the documented device-description key on older SDKs. */
    id value = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    return [value respondsToSelector:@selector(unsignedIntValue)] ? [value unsignedIntValue]
                                                                    : kCGNullDirectDisplay;
}

static NSInteger compare_screens(id left, id right, void *context)
{
    CGDirectDisplayID left_id = screen_display_id((NSScreen *)left);
    CGDirectDisplayID right_id = screen_display_id((NSScreen *)right);
    (void)context;

    if (left_id < right_id) return NSOrderedAscending;
    if (left_id > right_id) return NSOrderedDescending;
    return NSOrderedSame;
}

static NSInteger compare_metal_devices(id left, id right, void *context)
{
    uint64_t left_id = [(id<MTLDevice>)left registryID];
    uint64_t right_id = [(id<MTLDevice>)right registryID];
    (void)context;

    if (left_id < right_id) return NSOrderedAscending;
    if (left_id > right_id) return NSOrderedDescending;
    return NSOrderedSame;
}

static NSString *color_space_model_name(NSColorSpaceModel model)
{
    switch (model)
    {
        case NSColorSpaceModelGray: return @"gray";
        case NSColorSpaceModelRGB: return @"rgb";
        case NSColorSpaceModelCMYK: return @"cmyk";
        case NSColorSpaceModelLAB: return @"lab";
        case NSColorSpaceModelDeviceN: return @"device_n";
        case NSColorSpaceModelIndexed: return @"indexed";
        case NSColorSpaceModelPatterned: return @"patterned";
        case NSColorSpaceModelUnknown: return @"unknown";
    }
    return @"unknown";
}

static NSString *cg_color_space_model_name(CGColorSpaceModel model)
{
    switch (model)
    {
        case kCGColorSpaceModelMonochrome: return @"monochrome";
        case kCGColorSpaceModelRGB: return @"rgb";
        case kCGColorSpaceModelCMYK: return @"cmyk";
        case kCGColorSpaceModelLab: return @"lab";
        case kCGColorSpaceModelDeviceN: return @"device_n";
        case kCGColorSpaceModelIndexed: return @"indexed";
        case kCGColorSpaceModelPattern: return @"pattern";
        case kCGColorSpaceModelXYZ: return @"xyz";
        case kCGColorSpaceModelUnknown: return @"unknown";
    }
    return @"unknown";
}

static NSString *device_location_name(MTLDeviceLocation location)
{
    switch (location)
    {
        case MTLDeviceLocationBuiltIn: return @"built_in";
        case MTLDeviceLocationSlot: return @"slot";
        case MTLDeviceLocationExternal: return @"external";
        case MTLDeviceLocationUnspecified: return @"unspecified";
    }
    return @"unknown";
}

static NSString *command_buffer_status_name(MTLCommandBufferStatus status)
{
    switch (status)
    {
        case MTLCommandBufferStatusNotEnqueued: return @"not_enqueued";
        case MTLCommandBufferStatusEnqueued: return @"enqueued";
        case MTLCommandBufferStatusCommitted: return @"committed";
        case MTLCommandBufferStatusScheduled: return @"scheduled";
        case MTLCommandBufferStatusCompleted: return @"completed";
        case MTLCommandBufferStatusError: return @"error";
    }
    return @"unknown";
}

static id finite_number_or_unavailable(CGFloat value, BOOL *structural_ok)
{
    if (!isfinite((double)value) || value < 0.0)
    {
        *structural_ok = NO;
        return unavailable;
    }
    return [NSNumber numberWithDouble:(double)value];
}

static NSMutableDictionary *metal_device_dictionary(id<MTLDevice> device)
{
    NSMutableDictionary *result = [NSMutableDictionary dictionary];

    [result setObject:[NSNumber numberWithUnsignedLongLong:[device registryID]] forKey:@"registry_id"];
    [result setObject:([device name] ?: unavailable) forKey:@"name"];
    [result setObject:[NSNumber numberWithBool:[device isLowPower]] forKey:@"low_power"];
    [result setObject:[NSNumber numberWithBool:[device isHeadless]] forKey:@"headless"];
    [result setObject:[NSNumber numberWithBool:[device isRemovable]] forKey:@"removable"];

    if (@available(macOS 10.15, *))
    {
        [result setObject:[NSNumber numberWithBool:[device hasUnifiedMemory]] forKey:@"unified_memory"];
        [result setObject:device_location_name([device location]) forKey:@"location"];
        [result setObject:[NSNumber numberWithUnsignedLong:[device locationNumber]]
                   forKey:@"location_number"];
    }
    else
    {
        [result setObject:unavailable forKey:@"unified_memory"];
        [result setObject:unavailable forKey:@"location"];
        [result setObject:unavailable forKey:@"location_number"];
    }
    return result;
}

static NSMutableDictionary *screen_dictionary(NSScreen *screen, BOOL *structural_ok)
{
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    CGDirectDisplayID display_id = screen_display_id(screen);
    NSColorSpace *color_space = [screen colorSpace];
    NSInteger bits_per_pixel = NSBitsPerPixelFromDepth([screen depth]);
    NSInteger bits_per_sample = NSBitsPerSampleFromDepth([screen depth]);
    NSString *localized_name = nil;
    NSString *cg_name = nil;

    if (@available(macOS 10.15, *)) localized_name = [screen localizedName];

    [result setObject:(display_id ? [NSNumber numberWithUnsignedInt:display_id] : unavailable)
               forKey:@"display_id"];
    [result setObject:(localized_name ?: unavailable) forKey:@"name"];
    [result setObject:(bits_per_pixel > 0 ? [NSNumber numberWithInteger:bits_per_pixel] : unavailable)
               forKey:@"bits_per_pixel"];
    [result setObject:(bits_per_sample > 0 ? [NSNumber numberWithInteger:bits_per_sample] : unavailable)
               forKey:@"bits_per_sample"];

    if (!display_id) *structural_ok = NO;

    if (color_space)
    {
        CGColorSpaceRef cg_color_space = [color_space CGColorSpace];
        if (cg_color_space)
        {
            CFStringRef name = CGColorSpaceGetName(cg_color_space);
            if (name) cg_name = (NSString *)name;
        }
        [result setObject:([color_space localizedName] ?: unavailable)
                   forKey:@"color_space_localized_name"];
        [result setObject:(cg_name ?: unavailable) forKey:@"color_space_name"];
        [result setObject:color_space_model_name([color_space colorSpaceModel])
                   forKey:@"color_space_model"];
    }
    else
    {
        [result setObject:unavailable forKey:@"color_space_localized_name"];
        [result setObject:unavailable forKey:@"color_space_name"];
        [result setObject:unavailable forKey:@"color_space_model"];
    }

    if (@available(macOS 10.11, *))
    {
        [result setObject:finite_number_or_unavailable(
                              [screen maximumExtendedDynamicRangeColorComponentValue], structural_ok)
                   forKey:@"edr_headroom_current"];
    }
    else
    {
        [result setObject:unavailable forKey:@"edr_headroom_current"];
    }

    if (@available(macOS 10.15, *))
    {
        [result setObject:finite_number_or_unavailable(
                              [screen maximumPotentialExtendedDynamicRangeColorComponentValue], structural_ok)
                   forKey:@"edr_headroom_potential"];
        [result setObject:finite_number_or_unavailable(
                              [screen maximumReferenceExtendedDynamicRangeColorComponentValue], structural_ok)
                   forKey:@"edr_headroom_reference"];
    }
    else
    {
        [result setObject:unavailable forKey:@"edr_headroom_potential"];
        [result setObject:unavailable forKey:@"edr_headroom_reference"];
    }

    /* These cannot be derived from EDR component multipliers. */
    [result setObject:unavailable forKey:@"physical_luminance_nits"];
    [result setObject:unavailable forKey:@"color_primaries"];

    id<MTLDevice> display_device = nil;
    if (@available(macOS 10.11, *))
    {
        if (display_id) display_device = CGDirectDisplayCopyCurrentMetalDevice(display_id);
    }
    if (display_device)
    {
        [result setObject:[NSNumber numberWithUnsignedLongLong:[display_device registryID]]
                   forKey:@"metal_device_registry_id"];
        [result setObject:([display_device name] ?: unavailable) forKey:@"metal_device_name"];
        [result setObject:@"CGDirectDisplayCopyCurrentMetalDevice"
                   forKey:@"metal_device_association_source"];
        [display_device release];
    }
    else
    {
        [result setObject:unavailable forKey:@"metal_device_registry_id"];
        [result setObject:unavailable forKey:@"metal_device_name"];
        [result setObject:unavailable forKey:@"metal_device_association_source"];
    }

    return result;
}

static NSMutableDictionary *validate_cg_color_space(CFStringRef requested_name,
                                                     NSString *label,
                                                     BOOL api_available,
                                                     BOOL *structural_ok)
{
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    CGColorSpaceRef color_space = NULL;

    [result setObject:label forKey:@"requested"];
    [result setObject:[NSNumber numberWithBool:api_available] forKey:@"api_available"];
    if (!api_available)
    {
        [result setObject:unavailable forKey:@"created"];
        [result setObject:unavailable forKey:@"actual_name"];
        [result setObject:unavailable forKey:@"model"];
        return result;
    }

    color_space = CGColorSpaceCreateWithName(requested_name);
    [result setObject:[NSNumber numberWithBool:(color_space != NULL)] forKey:@"created"];
    if (!color_space)
    {
        *structural_ok = NO;
        [result setObject:unavailable forKey:@"actual_name"];
        [result setObject:unavailable forKey:@"model"];
        return result;
    }

    CFStringRef actual_name = CGColorSpaceGetName(color_space);
    [result setObject:(actual_name ? (NSString *)actual_name : unavailable) forKey:@"actual_name"];
    [result setObject:cg_color_space_model_name(CGColorSpaceGetModel(color_space)) forKey:@"model"];
    CGColorSpaceRelease(color_space);
    return result;
}

static void pump_appkit_events_until(NSDate *deadline)
{
    while ([deadline timeIntervalSinceNow] > 0.0)
    {
        NSTimeInterval slice = MIN(0.02, [deadline timeIntervalSinceNow]);
        NSDate *slice_deadline = [NSDate dateWithTimeIntervalSinceNow:MAX(0.0, slice)];
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:slice_deadline
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event) [NSApp sendEvent:event];
        [NSApp updateWindows];
    }
}

static NSScreen *find_pattern_screen(NSArray *screens, const probe_options *options)
{
    if (!options->display_id_set) return [NSScreen mainScreen] ?: [screens firstObject];

    for (NSScreen *screen in screens)
    {
        if (screen_display_id(screen) == options->display_id) return screen;
    }
    return nil;
}

static void fill_pattern_patches(pattern_patch patches[6], float edr_level)
{
    static const float rects[6][4] = {
        {0.02f, 0.03f, 0.32f, 0.47f},
        {0.35f, 0.03f, 0.65f, 0.47f},
        {0.68f, 0.03f, 0.98f, 0.47f},
        {0.02f, 0.53f, 0.32f, 0.97f},
        {0.35f, 0.53f, 0.65f, 0.97f},
        {0.68f, 0.53f, 0.98f, 0.97f},
    };
    static const float base_colors[6][4] = {
        {0.18f, 0.18f, 0.18f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 0.05f, 0.05f, 1.0f},
        {0.05f, 1.0f, 0.05f, 1.0f},
        {0.05f, 0.05f, 1.0f, 1.0f},
    };
    NSUInteger i;

    for (i = 0; i < 6; ++i)
    {
        memcpy(patches[i].rect, rects[i], sizeof(patches[i].rect));
        memcpy(patches[i].color, base_colors[i], sizeof(patches[i].color));
    }

    /* Every component above 1.0 is bounded solely by public potential headroom. */
    patches[2].color[0] = patches[2].color[1] = patches[2].color[2] = edr_level;
    patches[3].color[0] = edr_level;
    patches[4].color[1] = edr_level;
    patches[5].color[2] = edr_level;
}

static NSMutableDictionary *run_pattern(NSArray *screens,
                                        const probe_options *options,
                                        BOOL *structural_ok)
{
    static NSString *const shader_source =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct Patch { float4 rect; float4 color; };\n"
         "struct VertexOut { float4 position [[position]]; float4 color; };\n"
         "vertex VertexOut patch_vertex(uint vertex_id [[vertex_id]],\n"
         "                              uint instance_id [[instance_id]],\n"
         "                              constant Patch *patches [[buffer(0)]]) {\n"
         "  const float2 corners[4] = { float2(0.0, 0.0), float2(1.0, 0.0),\n"
         "                              float2(0.0, 1.0), float2(1.0, 1.0) };\n"
         "  Patch patch = patches[instance_id];\n"
         "  float2 point = mix(patch.rect.xy, patch.rect.zw, corners[vertex_id]);\n"
         "  VertexOut out;\n"
         "  out.position = float4(point.x * 2.0 - 1.0, 1.0 - point.y * 2.0, 0.0, 1.0);\n"
         "  out.color = patch.color;\n"
         "  return out;\n"
         "}\n"
         "fragment float4 patch_fragment(VertexOut in [[stage_in]]) { return in.color; }\n";

    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    NSScreen *screen = find_pattern_screen(screens, options);
    CGDirectDisplayID display_id;
    CGFloat potential_headroom;
    float edr_level;
    NSRect visible_frame;
    NSRect window_frame;
    NSWindow *window = nil;
    NSView *view = nil;
    CAMetalLayer *layer = nil;
    CGColorSpaceRef layer_color_space = NULL;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> vertex_function = nil;
    id<MTLFunction> fragment_function = nil;
    MTLRenderPipelineDescriptor *pipeline_descriptor = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLBuffer> patch_buffer = nil;
    id<CAMetalDrawable> drawable = nil;
    id<MTLCommandBuffer> command_buffer = nil;
    NSError *error = nil;
    BOOL drawable_acquired = NO;
    BOOL command_committed = NO;
    id presented_value = unavailable;
    __block _Atomic(bool) presented_callback;

    atomic_init(&presented_callback, false);

    [result setObject:[NSNumber numberWithBool:YES] forKey:@"requested"];
    [result setObject:[NSNumber numberWithDouble:options->pattern_seconds] forKey:@"duration_seconds"];
    [result setObject:unavailable forKey:@"display_id"];
    [result setObject:unavailable forKey:@"potential_headroom"];
    [result setObject:unavailable forKey:@"edr_patch_level"];
    [result setObject:unavailable forKey:@"metal_device_registry_id"];
    [result setObject:[NSNumber numberWithBool:NO] forKey:@"drawable_acquired"];
    [result setObject:[NSNumber numberWithBool:NO] forKey:@"command_buffer_committed"];
    [result setObject:unavailable forKey:@"command_buffer_status"];
    [result setObject:unavailable forKey:@"presented"];
    [result setObject:unavailable forKey:@"current_headroom_after_request"];

    if (!screen)
    {
        [result setObject:@"requested display was not found" forKey:@"error"];
        *structural_ok = NO;
        return result;
    }

    display_id = screen_display_id(screen);
    [result setObject:[NSNumber numberWithUnsignedInt:display_id] forKey:@"display_id"];

    if (!display_id)
    {
        [result setObject:@"screen has no CGDirectDisplayID" forKey:@"error"];
        *structural_ok = NO;
        return result;
    }

    if (@available(macOS 10.15, *))
    {
        potential_headroom = [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
    }
    else
    {
        [result setObject:@"potential EDR headroom API is unavailable" forKey:@"error"];
        *structural_ok = NO;
        return result;
    }

    if (!isfinite((double)potential_headroom) || potential_headroom < 0.0)
    {
        [result setObject:@"potential EDR headroom is structurally invalid" forKey:@"error"];
        *structural_ok = NO;
        return result;
    }

    /* A non-EDR screen cannot legally receive components above 1.0. */
    edr_level = potential_headroom > 1.0 ? (float)potential_headroom : 1.0f;
    [result setObject:[NSNumber numberWithDouble:(double)potential_headroom]
               forKey:@"potential_headroom"];
    [result setObject:(potential_headroom > 1.0 ? [NSNumber numberWithFloat:edr_level] : unavailable)
               forKey:@"edr_patch_level"];

    device = CGDirectDisplayCopyCurrentMetalDevice(display_id);
    if (!device)
    {
        [result setObject:@"display has no current Metal device association" forKey:@"error"];
        *structural_ok = NO;
        return result;
    }
    [result setObject:[NSNumber numberWithUnsignedLongLong:[device registryID]]
               forKey:@"metal_device_registry_id"];

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    visible_frame = [screen visibleFrame];
    window_frame.size.width = MIN(900.0, MAX(480.0, visible_frame.size.width * 0.70));
    window_frame.size.height = MIN(520.0, MAX(320.0, visible_frame.size.height * 0.55));
    window_frame.origin.x = NSMidX(visible_frame) - window_frame.size.width * 0.5;
    window_frame.origin.y = NSMidY(visible_frame) - window_frame.size.height * 0.5;

    window = [[NSWindow alloc] initWithContentRect:window_frame
                                         styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                           backing:NSBackingStoreBuffered
                                             defer:NO
                                            screen:screen];
    view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, window_frame.size.width,
                                                    window_frame.size.height)];
    layer = [[CAMetalLayer alloc] init];
    layer_color_space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    if (!window || !view || !layer || !layer_color_space)
    {
        [result setObject:@"failed to create pattern window, view, layer, or color space"
                   forKey:@"error"];
        *structural_ok = NO;
        goto cleanup;
    }

    [window setTitle:@"Switchyard macOS EDR Public-API Probe"];
    [window setReleasedWhenClosed:NO];
    [view setWantsLayer:YES];
    [layer setDevice:device];
    [layer setPixelFormat:MTLPixelFormatRGBA16Float];
    [layer setFramebufferOnly:YES];
    [layer setOpaque:YES];
    [layer setColorspace:layer_color_space];
    [layer setWantsExtendedDynamicRangeContent:YES];
    [layer setAllowsNextDrawableTimeout:YES];
    [layer setContentsScale:[screen backingScaleFactor]];
    [layer setFrame:[view bounds]];
    [layer setDrawableSize:CGSizeMake(NSWidth([view bounds]) * [screen backingScaleFactor],
                                     NSHeight([view bounds]) * [screen backingScaleFactor])];
    [view setLayer:layer];
    [window setContentView:view];
    [window makeKeyAndOrderFront:nil];
    [window orderFrontRegardless];
    if (@available(macOS 14.0, *)) [NSApp activate];
    [window displayIfNeeded];
    [CATransaction flush];

    /* Give WindowServer a short, bounded opportunity to attach the layer. */
    pump_appkit_events_until([NSDate dateWithTimeIntervalSinceNow:0.10]);

    queue = [device newCommandQueue];
    library = [device newLibraryWithSource:shader_source options:nil error:&error];
    vertex_function = [library newFunctionWithName:@"patch_vertex"];
    fragment_function = [library newFunctionWithName:@"patch_fragment"];
    pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    if (!queue || !library || !vertex_function || !fragment_function || !pipeline_descriptor)
    {
        [result setObject:(error ? [error localizedDescription] : @"failed to create Metal shader objects")
                   forKey:@"error"];
        *structural_ok = NO;
        goto cleanup;
    }

    [pipeline_descriptor setVertexFunction:vertex_function];
    [pipeline_descriptor setFragmentFunction:fragment_function];
    [[pipeline_descriptor.colorAttachments objectAtIndexedSubscript:0]
        setPixelFormat:MTLPixelFormatRGBA16Float];
    pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
    if (!pipeline)
    {
        [result setObject:(error ? [error localizedDescription] : @"failed to create Metal pipeline")
                   forKey:@"error"];
        *structural_ok = NO;
        goto cleanup;
    }

    pattern_patch patches[6];
    fill_pattern_patches(patches, edr_level);
    patch_buffer = [device newBufferWithBytes:patches
                                      length:sizeof(patches)
                                     options:MTLResourceStorageModeShared];
    drawable = [[layer nextDrawable] retain];
    drawable_acquired = drawable != nil;
    [result setObject:[NSNumber numberWithBool:drawable_acquired] forKey:@"drawable_acquired"];
    if (!patch_buffer || !drawable)
    {
        [result setObject:@"failed to allocate a patch buffer or acquire a drawable" forKey:@"error"];
        *structural_ok = NO;
        goto cleanup;
    }

    if (@available(macOS 10.15.4, *))
    {
        [drawable addPresentedHandler:^(id<MTLDrawable> presented_drawable) {
            (void)presented_drawable;
            atomic_store_explicit(&presented_callback, true, memory_order_release);
        }];
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor *attachment =
        [pass.colorAttachments objectAtIndexedSubscript:0];
    [attachment setTexture:[drawable texture]];
    [attachment setLoadAction:MTLLoadActionClear];
    [attachment setStoreAction:MTLStoreActionStore];
    [attachment setClearColor:MTLClearColorMake(0.0, 0.0, 0.0, 1.0)];

    command_buffer = [[queue commandBuffer] retain];
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!command_buffer || !encoder)
    {
        [result setObject:@"failed to create a Metal command buffer or render encoder" forKey:@"error"];
        *structural_ok = NO;
        goto cleanup;
    }

    MTLViewport viewport = {0.0, 0.0, (double)[drawable texture].width,
                            (double)[drawable texture].height, 0.0, 1.0};
    [encoder setViewport:viewport];
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:patch_buffer offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:0
                vertexCount:4
              instanceCount:6];
    [encoder endEncoding];
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
    command_committed = YES;
    [result setObject:[NSNumber numberWithBool:YES] forKey:@"command_buffer_committed"];

    pump_appkit_events_until([NSDate dateWithTimeIntervalSinceNow:options->pattern_seconds]);

    [result setObject:command_buffer_status_name([command_buffer status])
               forKey:@"command_buffer_status"];
    if ([command_buffer status] == MTLCommandBufferStatusError)
    {
        [result setObject:([[command_buffer error] localizedDescription] ?: @"Metal command failed")
                   forKey:@"error"];
        *structural_ok = NO;
    }
    else if ([command_buffer status] != MTLCommandBufferStatusCompleted)
    {
        [result setObject:@"Metal command did not complete within the bounded pattern interval"
                   forKey:@"error"];
        *structural_ok = NO;
    }

    if (@available(macOS 10.15.4, *))
    {
        BOOL was_presented = atomic_load_explicit(&presented_callback, memory_order_acquire) ||
                             [drawable presentedTime] > 0.0;
        presented_value = [NSNumber numberWithBool:was_presented];
        if (![presented_value boolValue]) *structural_ok = NO;
    }
    [result setObject:presented_value forKey:@"presented"];

    if (@available(macOS 10.11, *))
    {
        [result setObject:finite_number_or_unavailable(
                              [screen maximumExtendedDynamicRangeColorComponentValue], structural_ok)
                   forKey:@"current_headroom_after_request"];
    }
    else
    {
        [result setObject:unavailable forKey:@"current_headroom_after_request"];
    }

cleanup:
    if (![result objectForKey:@"drawable_acquired"])
        [result setObject:[NSNumber numberWithBool:drawable_acquired] forKey:@"drawable_acquired"];
    if (![result objectForKey:@"command_buffer_committed"])
        [result setObject:[NSNumber numberWithBool:command_committed]
                   forKey:@"command_buffer_committed"];
    if (![result objectForKey:@"command_buffer_status"])
        [result setObject:(command_buffer ? command_buffer_status_name([command_buffer status]) : unavailable)
                   forKey:@"command_buffer_status"];
    if (![result objectForKey:@"presented"])
        [result setObject:presented_value forKey:@"presented"];
    if (![result objectForKey:@"current_headroom_after_request"])
        [result setObject:unavailable forKey:@"current_headroom_after_request"];

    [window orderOut:nil];
    [window close];
    [command_buffer release];
    [drawable release];
    [patch_buffer release];
    [pipeline release];
    [pipeline_descriptor release];
    [fragment_function release];
    [vertex_function release];
    [library release];
    [queue release];
    [view setLayer:nil];
    [layer release];
    if (layer_color_space) CGColorSpaceRelease(layer_color_space);
    [view release];
    [window release];
    [device release];
    return result;
}

static NSMutableDictionary *build_report(const probe_options *options, BOOL *structural_ok)
{
    NSMutableDictionary *root = [NSMutableDictionary dictionary];
    NSArray *screens = [[NSScreen screens] sortedArrayUsingFunction:compare_screens context:NULL];
    NSMutableArray *screen_reports = [NSMutableArray array];
    NSMutableSet *display_ids = [NSMutableSet set];
    NSArray *copied_devices = MTLCopyAllDevices();
    NSArray *devices = [copied_devices sortedArrayUsingFunction:compare_metal_devices context:NULL];
    NSMutableArray *device_reports = [NSMutableArray array];
    NSMutableArray *color_space_reports = [NSMutableArray array];

    *structural_ok = [screens count] > 0;

    for (NSScreen *screen in screens)
    {
        CGDirectDisplayID display_id = screen_display_id(screen);
        NSNumber *display_number = [NSNumber numberWithUnsignedInt:display_id];
        if (!display_id || [display_ids containsObject:display_number]) *structural_ok = NO;
        if (display_id) [display_ids addObject:display_number];
        [screen_reports addObject:screen_dictionary(screen, structural_ok)];
    }

    for (id<MTLDevice> device in devices)
        [device_reports addObject:metal_device_dictionary(device)];

    [color_space_reports addObject:validate_cg_color_space(kCGColorSpaceExtendedLinearSRGB,
                                                           @"kCGColorSpaceExtendedLinearSRGB",
                                                           YES, structural_ok)];
    if (@available(macOS 11.0, *))
    {
        [color_space_reports addObject:validate_cg_color_space(kCGColorSpaceITUR_2100_PQ,
                                                               @"kCGColorSpaceITUR_2100_PQ",
                                                               YES, structural_ok)];
    }
    else
    {
        [color_space_reports addObject:validate_cg_color_space(NULL,
                                                               @"kCGColorSpaceITUR_2100_PQ",
                                                               NO, structural_ok)];
    }

    [root setObject:[NSNumber numberWithInteger:1] forKey:@"schema_version"];
    [root setObject:@"public AppKit/CoreGraphics/Metal/QuartzCore APIs only"
              forKey:@"api_policy"];
    [root setObject:@"dimensionless color-component multiplier" forKey:@"edr_headroom_units"];
    [root setObject:screen_reports forKey:@"screens"];
    [root setObject:device_reports forKey:@"metal_devices"];
    [root setObject:color_space_reports forKey:@"color_space_validation"];

    if (options->pattern)
    {
        [root setObject:run_pattern(screens, options, structural_ok) forKey:@"pattern"];
    }
    else
    {
        [root setObject:[NSDictionary dictionaryWithObject:[NSNumber numberWithBool:NO]
                                                    forKey:@"requested"]
                  forKey:@"pattern"];
    }

    [root setObject:[NSNumber numberWithBool:*structural_ok] forKey:@"structural_ok"];
    [copied_devices release];
    return root;
}

static BOOL print_json(NSDictionary *report)
{
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:report
                                                   options:NSJSONWritingSortedKeys
                                                     error:&error];
    if (!data)
    {
        fprintf(stderr, "failed to serialize probe JSON: %s\n",
                [[error localizedDescription] UTF8String]);
        return NO;
    }
    fwrite([data bytes], 1, [data length], stdout);
    fputc('\n', stdout);
    return YES;
}

static void print_value(id value)
{
    if (!value)
        printf("unavailable");
    else if ([value isKindOfClass:[NSNumber class]])
        printf("%s", [[value stringValue] UTF8String]);
    else
        printf("%s", [[value description] UTF8String]);
}

static void print_text(NSDictionary *report)
{
    printf("macOS EDR public-API probe (schema 1)\n");
    printf("EDR headroom units: dimensionless color-component multiplier\n");

    for (NSDictionary *screen in [report objectForKey:@"screens"])
    {
        printf("screen display_id="); print_value([screen objectForKey:@"display_id"]);
        printf(" name=\""); print_value([screen objectForKey:@"name"]); printf("\"\n");
        printf("  depth bits_per_pixel="); print_value([screen objectForKey:@"bits_per_pixel"]);
        printf(" bits_per_sample="); print_value([screen objectForKey:@"bits_per_sample"]); printf("\n");
        printf("  color_space name=\""); print_value([screen objectForKey:@"color_space_name"]);
        printf("\" localized=\""); print_value([screen objectForKey:@"color_space_localized_name"]);
        printf("\" model="); print_value([screen objectForKey:@"color_space_model"]); printf("\n");
        printf("  edr current="); print_value([screen objectForKey:@"edr_headroom_current"]);
        printf(" potential="); print_value([screen objectForKey:@"edr_headroom_potential"]);
        printf(" reference="); print_value([screen objectForKey:@"edr_headroom_reference"]); printf("\n");
        printf("  physical_luminance_nits=unavailable color_primaries=unavailable\n");
        printf("  metal registry_id="); print_value([screen objectForKey:@"metal_device_registry_id"]);
        printf(" name=\""); print_value([screen objectForKey:@"metal_device_name"]);
        printf("\" association="); print_value([screen objectForKey:@"metal_device_association_source"]);
        printf("\n");
    }

    for (NSDictionary *device in [report objectForKey:@"metal_devices"])
    {
        printf("metal_device registry_id="); print_value([device objectForKey:@"registry_id"]);
        printf(" name=\""); print_value([device objectForKey:@"name"]);
        printf("\" location="); print_value([device objectForKey:@"location"]);
        printf(" location_number="); print_value([device objectForKey:@"location_number"]);
        printf(" low_power="); print_value([device objectForKey:@"low_power"]);
        printf(" headless="); print_value([device objectForKey:@"headless"]);
        printf(" removable="); print_value([device objectForKey:@"removable"]);
        printf(" unified_memory="); print_value([device objectForKey:@"unified_memory"]); printf("\n");
    }

    for (NSDictionary *validation in [report objectForKey:@"color_space_validation"])
    {
        printf("color_space_validation requested="); print_value([validation objectForKey:@"requested"]);
        printf(" api_available="); print_value([validation objectForKey:@"api_available"]);
        printf(" created="); print_value([validation objectForKey:@"created"]);
        printf(" actual_name=\""); print_value([validation objectForKey:@"actual_name"]);
        printf("\" model="); print_value([validation objectForKey:@"model"]); printf("\n");
    }

    NSDictionary *pattern = [report objectForKey:@"pattern"];
    if ([[pattern objectForKey:@"requested"] boolValue])
    {
        printf("pattern display_id="); print_value([pattern objectForKey:@"display_id"]);
        printf(" edr_patch_level="); print_value([pattern objectForKey:@"edr_patch_level"]);
        printf(" drawable_acquired="); print_value([pattern objectForKey:@"drawable_acquired"]);
        printf(" command_buffer_status="); print_value([pattern objectForKey:@"command_buffer_status"]);
        printf(" presented="); print_value([pattern objectForKey:@"presented"]);
        printf(" current_headroom_after_request=");
        print_value([pattern objectForKey:@"current_headroom_after_request"]); printf("\n");
        if ([pattern objectForKey:@"error"])
        {
            printf("pattern_error="); print_value([pattern objectForKey:@"error"]); printf("\n");
        }
    }

    printf("structural_ok="); print_value([report objectForKey:@"structural_ok"]); printf("\n");
}

int main(int argc, const char *argv[])
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    probe_options options;
    BOOL structural_ok;
    int parse_result = parse_options(argc, argv, &options);
    int result;

    if (parse_result)
    {
        [pool drain];
        return parse_result < 0 ? 0 : parse_result;
    }

    [NSApplication sharedApplication];
    NSDictionary *report = build_report(&options, &structural_ok);
    result = structural_ok ? 0 : 1;

    if (options.json)
    {
        if (!print_json(report)) result = 1;
    }
    else
    {
        print_text(report);
    }

    [pool drain];
    return result;
}
