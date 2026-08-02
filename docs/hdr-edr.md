# HDR, EDR, and D3D12 capability policy

Switchyard exposes HDR only when the active display, Wine presentation driver,
and graphics provider agree on one exact format and colour-space pair. A deep
colour display by itself is not sufficient. Unknown measurements remain
unknown, and an unavailable provider path fails instead of accepting state that
cannot reach the display.

## Display information and DXGI units

winemac collects a separate record for every `NSScreen`/Core Graphics display.
The sources are used in this order:

1. `NSScreen.deviceDescription[NSDeviceBitsPerSample]` supplies bits per colour
   component when AppKit publishes it.
2. `CGDisplayCopyColorSpace()` and exact public `CGColorSpace` names identify a
   known transfer function and gamut. A nonstandard display profile maps to
   `DXGI_COLOR_SPACE_CUSTOM`; it is not guessed from a similar name.
3. Public IOKit `IODisplayEDID` data supplies CIE 1931 xy primaries, white point,
   CTA-861 HDR EOTFs, and CTA luminance codes after header, length, block, and
   checksum validation.
4. `ColorSyncProfileCreateWithDisplayID()` and public ICC data fill fields that
   EDID did not provide. ICC `rXYZ`, `gXYZ`, `bXYZ`, and `wtpt` values are used
   only when their native chromaticities can be recovered; a D50 profile
   connection-space value without an invertible `chad` matrix is not reported.
   ICC `lumi` is the measured display white and therefore fills only full-frame
   white luminance.
5. `NSScreen` publishes current, potential, and reference EDR headroom as ratios
   to platform SDR reference white. Those ratios are capability/state data, not
   absolute luminance, and are never converted to nits.

`DXGI_OUTPUT_DESC1` primaries and white point are CIE 1931 xy values. Its three
luminance fields are cd/m2: black level, peak white, and sustained full-frame
white. CTA values retain those meanings. An unavailable field remains zero,
its internal validity bit remains clear, and the output colour space remains
`CUSTOM` unless an exact DXGI encoding is known. In particular, the public
macOS APIs do not publish the absolute system SDR-white setting, so
`DISPLAYCONFIG_SDR_WHITE_LEVEL` returns `ERROR_NOT_SUPPORTED` instead of the
commonly assumed 80-nit/1000 value.

Potential EDR headroom is the non-circular public capability probe. Current EDR
headroom is live state, but AppKit commonly reports 1.0 until at least one layer
sets `wantsExtendedDynamicRangeContent`; using it for initial capability
discovery would prevent such a layer from ever being enabled. On macOS 10.15
and newer, the swapchain path therefore requires potential headroom above 1.0
and then asks the layer to enter EDR. On macOS 10.11 through 10.14, where the
potential property is unavailable, current headroom is the conservative
fallback. Screen, colour-profile, wake, and window-screen notifications repeat
the public probe and invalidate an active HDR swapchain when it no longer
succeeds. macOS exposes no separate system HDR-switch property, so
`advancedColorEnabled` follows this EDR availability rather than waiting for
the request-dependent current value to rise.

## Presentation capability matrix

| API/provider | SDR sRGB | scRGB | HDR10 PQ and metadata | Policy owner |
| --- | --- | --- | --- | --- |
| D3D11 with builtin wined3d | Yes | No | No | Wine; no explicit provider hook exists |
| D3D12 with builtin vkd3d/Vulkan WSI | Yes | Conditional | Conditional | Vulkan extension, surface-format enumeration, and winemac hook probes |
| Direct Wine Vulkan WSI | Yes | Conditional | Conditional | Host Vulkan provider plus winemac hook probes |
| GPTK/D3DMetal replacement | Provider-defined | Provider-defined | Provider-defined | The selected closed provider; Wine does not override or infer it |
| Offscreen/remote Metal surface | Yes | No | No | winemac rejects non-SDR configuration |

The builtin D3D12 path requires the following exact combinations:

- SDR: the provider's mapped image format with
  `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`;
- scRGB: `DXGI_FORMAT_R16G16B16A16_FLOAT`,
  `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`, `MTLPixelFormatRGBA16Float`, and
  `kCGColorSpaceExtendedLinearSRGB`;
- HDR10: `DXGI_FORMAT_R10G10B10A2_UNORM`,
  `VK_COLOR_SPACE_HDR10_ST2084_EXT`, the provider's RGB10A2 normalized Metal
  format, and `kCGColorSpaceITUR_2100_PQ`.

`VK_EXT_swapchain_colorspace` and the enumerated surface format establish colour
space support. `SetHDRMetaData` additionally requires a callable
`VK_EXT_hdr_metadata` device entry point. winemac configures only a verified
`CAMetalLayer`; MoltenVK remains the owner of its pixel format. HDR10 metadata
uses public `CAEDRMetadata` with the normalized-buffer optical scale of 10,000
cd/m2. A value of 100 is appropriate to display-linear floating-point content,
not to the normalized PQ swapchain.

SDR is a strict fast path. A layer that has never entered HDR is left unchanged,
including its original colour space and provider pixel format. Returning from
HDR clears EDR metadata and `wantsExtendedDynamicRangeContent` and restores the
retained SDR colour space. CEF and other ordinary desktop-composited windows do
not enter this path unless their swapchain explicitly selects a non-SDR colour
space.

## DXGI state and error contract

- `CheckColorSpaceSupport` returns `E_INVALIDARG` for a null result pointer.
  Otherwise it returns `S_OK` and sets `PRESENT` only when the current format,
  provider surface list, and display hook all support the requested space.
  Unknown enumeration values produce zero flags.
- `SetColorSpace1` returns `E_INVALIDARG` for a structurally incompatible or
  unavailable colour space. A supported transition drains pending presents,
  recreates the backend swapchain transactionally, and restores the previous
  backend state if creation fails.
- `SetHDRMetaData(NONE)` requires size zero and a null pointer. Clearing active
  PQ metadata recreates the backend swapchain because Vulkan's metadata command
  has no clear operation; failure restores the previous swapchain and metadata.
- `SetHDRMetaData(HDR10)` requires exactly `sizeof(DXGI_HDR_METADATA_HDR10)` and
  a non-null pointer. Chromaticities, PQ limits, and known min/max relationships
  are validated. Zero is retained as the standards-defined unknown value.
  Structurally valid metadata on an SDR or unsupported provider path returns
  `DXGI_ERROR_INVALID_CALL`.
- Unknown metadata types and every other type/size/pointer mismatch return
  `E_INVALIDARG`. No path dereferences an input before these checks.

The metadata cache is bounded to one fixed-size HDR10 record per swapchain.
The Vulkan boundary refuses HDR-metadata and present lists above 4,096 entries.
The WOW64 thunk converts metadata in fixed 32-element stack chunks, so accepted
calls have bounded stack use and no attacker-controlled conversion allocation.
It validates metadata before the host call, validates handles before
use, and pins every surface/swapchain while calling the host and display
provider. Surface-format enumeration is likewise capped at 4,096 provider
entries and rejects a count that grows beyond the allocated snapshot.
The one Vulkan structure currently permitted after `VkHdrMetadataEXT`,
`VkHdrVividDynamicMetadataHUAWEI`, is converted and forwarded synchronously;
winemac consumes only the base static HDR10 fields. The application-owned
dynamic payload is never retained, and only the base static record is restored
after a swapchain recreation. Unknown or longer extension chains are rejected
at the thunk boundary instead of being partly interpreted.

## Display and swapchain lifecycle

AppKit screen-parameter, screen-colour-space, wake, and window-screen-change
notifications invalidate display topology and the live Metal colour
configuration. The notification path is coalesced and observers are removed
during teardown. Resize, fullscreen transitions, colour-space changes, and
swapchain replacement are serialized with present work. A short surface
reservation pins the configured swapchain but is released before any provider
call that can synchronously marshal to AppKit. Terminal destroy states remove
registry visibility before callback-capable host teardown and defer object
release until all operation and swapchain pins drain. The D3D12 worker
reservation drains pending presents before backend destruction and blocks
concurrent resize or colour mutation.

Every present takes one bounded, nonblocking lifecycle reservation so teardown
cannot invalidate a surface during the host present callback. Only non-SDR
swapchains call the per-present Objective-C display validator. If a window moves
to a display that cannot satisfy the active configuration, present returns
`VK_ERROR_OUT_OF_DATE_KHR`, forcing normal swapchain recreation. After warm-up,
the SDR path performs no colour-space query, Objective-C validation, allocation,
or synchronous main-thread visit.

## D3D12 feature matrix

Apple GPU family names are observational and never imply a D3D12 feature. The
builtin provider derives support from the Vulkan physical-device feature and
extension structures that vkd3d can execute end to end. The closed D3DMetal
provider answers its own `CheckFeatureSupport`; Wine neither patches its binary
nor replaces its answers.

For builtin vkd3d, ray tracing, VRS, mesh shaders, sampler feedback, enhanced
barriers, advanced texture operations, and the other unimplemented OPTIONS5-18
clusters report their explicit not-supported tier or false value. OPTIONS8
unaligned block textures is the sole unconditional positive in that range,
because Vulkan does not impose the D3D12 block-alignment restriction. Cross-
feature invariants prevent dependent mesh/VRS/render-pass flags from becoming
true while their base tier is unavailable.

The provider accepts the existing single-operation indirect draw, indexed-draw,
and dispatch signatures. The end-to-end closure added here exercises uncounted
indirect draw results, timestamp heap creation, command recording, resolve,
queue submission, readback and frequency reporting, and local fence creation,
queue signal, wait, and completed-value observation. Count-buffer indirect draw
is covered only after a recording probe succeeds; a provider without
`VK_KHR_draw_indirect_count` is skipped rather than treated as completing that
subfeature. Invalid command-signature stride/count, null output pointers,
unsupported timestamp queues, unknown fence flags, and shared fence requests
fail at their owning API boundary. Shared/cross-adapter fences remain
unadvertised until the provider has matching create/open handle support.

## Build and automated validation

Use an out-of-tree build with tests enabled. The focused validation set is:

```sh
gmake -j8
gmake dlls/winemac.drv/tests/display_color
./dlls/winemac.drv/tests/display_color
./switchyard/tests/display_color_parser_test.sh
gmake dlls/dxgi/tests/check dlls/d3d12/tests/check
```

`make` can replace `gmake` on hosts whose make is GNU make. Switchyard's macOS
dependency environment must remain visible to build tools; the Apple
`/usr/bin/make` process-launch path can discard `DYLD_LIBRARY_PATH` under SIP,
so the validated macOS procedure uses Homebrew GNU make.

The native parser test includes deterministic EDID/CTA and ICC vectors,
chromatic adaptation, malformed lengths/checksums/offsets, HDR10 serialization
and semantic validation, independent fake-display merging, bounded pseudo-fuzz
inputs, and ASan/UBSan when the compiler supports them. The DXGI tests cover
colour-space enumeration, nulls, type/size/state errors, valid HDR10 metadata,
and standards-defined all-unknown metadata. D3D12 tests cover OPTIONS5-18 size
and dependency invariants plus the full indirect/timestamp/fence flows.

For lifecycle qualification, loop create/present/resize/destroy while moving a
window between an EDR and SDR display, toggling fullscreen, changing display
brightness/profile, sleeping and waking, and hot-plugging the display. Run the
loop under ASan/UBSan where the macOS Wine configuration supports them, repeat
under TSan in a separate build, and inspect Leaks/Allocations for retained
`CGColorSpace`, `ColorSyncProfile`, observer, Metal layer, and swapchain objects.
TSan and Metal validation must not be mixed in a single evidentiary run when the
provider itself is not TSan-clean.

## Hardware and performance qualification

Hardware qualification requires an EDR-capable screen whose potential headroom
is above 1.0 and a provider that enumerates the exact HDR surface formats. Render
SDR reference white, 1-10 EDR linear steps, BT.2020 primaries, PQ code-value
ramps, 1,000/4,000/10,000-nit clipping patches, and full-frame versus
small-window peaks. Confirm that moving the window to an SDR-only display, or
making the potential-headroom probe unavailable through system/display state,
produces out-of-date/recreation behavior rather than an SDR-clamped success claim. Record
the active display profile, brightness, current/potential headroom, provider,
format, metadata, and macOS version.

Screenshots and ordinary screen capture are not optical HDR measurements: the
window server or capture API may tone map, clamp, convert colour space, or omit
display peak behavior. Use screenshots only to check layout and relative ramp
ordering. Absolute white, peak luminance, black level, and sustained full-frame
output require a calibrated HDR meter or a trusted reference monitor workflow.

Measure candidate and baseline builds with identical prefix, provider, display,
resolution, and power state. Run at least 12,000 SDR presents after 600 warm-up
frames and report mean/p95 CPU present time, GPU frame time, allocations after
warm-up, and main-thread stalls. The acceptance limit is no more than 5 percent
SDR CPU regression unless the absolute delta is below 20 microseconds, no new
steady-state allocation, and no colour-validation main-thread stall above 1 ms.
Profile scRGB and HDR10 separately with Time Profiler, Allocations, and Metal
System Trace; their 8-byte and 4-byte pixels and Core Animation tone mapping are
reported bandwidth/costs, not charged to the SDR fast-path result. Repeated
identical metadata must not drain pending presents.

## Availability and fallback

- macOS 10.11: `wantsExtendedDynamicRangeContent` and current EDR headroom;
- macOS 10.12: extended-linear sRGB colour space for scRGB;
- macOS 10.15: potential/reference headroom and `CAEDRMetadata` on
  `CAMetalLayer`;
- macOS 11: public BT.2100 PQ/HLG colour spaces and ColorSync PQ/HLG profile
  classification.

Compile-time guards and `@available` checks cover every public API. Older
systems, missing colour spaces, missing Vulkan extensions, unavailable display
measurements, non-Metal layers, and offscreen surfaces retain the SDR path and
reject HDR. They do not receive replacement constants or inferred capability.

## Recorded validation on Apple M5 Pro

The 2026-08-03 qualification host ran macOS 26.5.2 on an Apple M5 Pro with its
built-in Retina XDR display. The public probe reported potential EDR headroom
16.0. A visible extended-linear-sRGB `RGBA16Float` reference pattern contained
0.18 linear grey, SDR white 1.0, EDR white 16.0, and red/green/blue EDR patches.
The drawable was acquired, the Metal command completed, the presentation
callback fired, and current headroom rose from 1.0 before the request to
15.7959 while the layer was active. This proves public-API EDR layer activation
and submission on this display; it is not an optical luminance calibration.
An Instruments Metal System Trace of the same pattern recorded one application
command-buffer submission with one encoder, 439.75 microseconds submission
duration and 411.08 microseconds encoder duration, a 1,800 x 1,040
`RGBA16Float` CAMetalLayer drawable of 14.48 MiB, 15.36 MiB peak reported Metal
allocation, a present request, and no application command-buffer error. These
are the observable render/allocation costs. The trace does not expose the
WindowServer/Core Animation tone-mapping bytes, so colour-conversion bandwidth
cannot be separated from compositor work with this public trace and remains a
release-lab measurement rather than an invented derived value.

The same probe deliberately reported these unavailable rather than fabricating
them: absolute physical luminance, display primaries, and an exact public
`CGColorSpace` name for the active `Color LCD` profile. AppKit reported an
8-bit backing sample depth; that is a window backing property and must not be
described as native panel precision. No calibrated HDR meter or second HDR/SDR
display was available, so absolute nits, black level, sustained full-frame
peak, optical SDR-white tracking, physical hotplug/migration, and multi-display
tone mapping remain external validation blockers. Screen capture was not used
as a substitute for those measurements.

The out-of-tree candidate build completed for both PE architectures. The native
parser suite passed five repeated ASan/UBSan runs, a separate TSan run, and
Clang static analysis. The EDR probe compiled with both the current SDK and the
macOS 15.4 SDK under `-Werror`. Candidate DXGI tests completed with zero
failures (x86-64: 115 executed/55 skipped; x86: 97 executed/55 skipped). D3D12
tests completed with zero failures but only three non-device tests executed in
each architecture; 23 were skipped because MoltenVK 1.4.1 could not create the
required D3D12 feature-level-11 device. Provider diagnostics identify the exact
missing requirements: `viewportSubPixelBits`, geometry shaders, pipeline
statistics queries, shader cull distance, depth-clip enable, and transform
feedback/stream output. The ExecuteIndirect/timestamp/fence device flows
therefore require a conformant Vulkan or native Windows provider for final
runtime qualification; they are not claimed as executed on this MoltenVK host.
The x86-64 D3D11 conformance executable reached feature level 9_3 but retained
provider-dependent format/resource failures and extensive higher-feature-level
skips. Five repeated runs against current `main` and the rebased candidate all
returned status 5. The current-main baseline reported 6, 1, 2, 1, and 7
failures; the candidate reported 3, 1, 5, 1, and 1. Both sets contained only
the same three categories: device creation returned `E_INVALIDARG`, and
swapchain formats 0x1c and 0x1d returned `E_INVALIDARG` at feature levels 9_1
through 9_3. The nondeterministic count is therefore recorded as an existing
builtin-wined3d/Apple-OpenGL provider limitation, not converted into a pass
claim or a fixed failure count for the HDR work.

The post-rebase SDR present benchmark alternated five 12,000-frame current-main
baseline and candidate runs after 600 warm-up frames, using Apple M5 Pro,
BGRA8 sRGB, and immediate present. The baseline was built from an independent
archive of current-main revision `2af920ece8a806644c55ba6f48f5a84154002281`.
Median-of-run present p50 was 55.0 microseconds for baseline and 55.5
microseconds for candidate: +0.5 microseconds, +0.91 percent, within both the
5-percent and absolute-20-microsecond policies. Median p95 was 176.2 versus
165.0 microseconds; median GPU clear timestamp was 4.599 versus 4.531
microseconds. Median maximum RSS was 184,844,288 versus 184,508,416 bytes and
median peak footprint was 253,883,432 versus 253,903,840 bytes. No
post-warm-up allocation is performed by the new SDR colour path; the small
process-level footprint difference is below the measurement granularity.
`switchyard/tests/vulkan_present_benchmark.sh` records the JSON and
`/usr/bin/time -l` inputs used for this decision. A normal runner smoke test and
a forced one-second timeout test verified status 124 and no remaining process
whose command line referenced the unique benchmark executable. Another task's
spatial-audio soak was active during the post-rebase run; the alternating
baseline/candidate arms shared that background load, so the relative regression
decision is retained but the absolute timings are not presented as idle-host
measurements.

## Threat-oriented and lifetime review

The review treated application metadata, WOW64 pointers, Vulkan object handles,
provider enumeration counts, EDID/ICC bytes, display callbacks, and teardown
reentrancy as hostile boundaries. The implementation validates every DXGI
type/size/pointer combination before dereference, caps EDID at 32 KiB, ICC
profiles at 16 MiB, ICC tag tables at 4,096 entries, display enumeration at
1,024 entries, and Vulkan lists at 4,096 entries. Length additions and tag
ranges are checked before use. The HDR conversion path uses fixed stack chunks
and has no unbounded cache or attacker-sized allocation.

Surface registry removal precedes callback-capable provider destruction.
Per-operation and per-swapchain pins prevent UAF, the mutex is never held across
an AppKit/provider callback, and terminal destroy state prevents resurrection.
The review traced concurrent present, colour change, resize, fullscreen,
swapchain replacement, display invalidation, sleep/wake, and shutdown error
unwinding for leaked retains, double release, lock inversion, callback
reentrancy, and busy waiting. An independent general reviewer and a separate
concurrency reviewer found no remaining issue after fixes for XRandR monitor
initialization, old-SDK selector guarding, and Vulkan HDR-Vivid extension-chain
conversion. `leaks --atExit` was also run against the visible EDR probe. macOS
restricted writable-memory inspection, but the readable report attributed
18,816 bytes to three AppIntents/LinkServices `NSXPCConnection` root cycles and
contained no probe, CAMetalLayer, Metal, or ColorSync allocation stack. That is
not sufficient to clear the full Wine process. An Instruments Allocations run,
a whole-Wine TSan build, multi-display physical stress, WindowServer conversion
bandwidth measurement, and an optical HDR meter run remain necessary
release-lab checks; the standalone parser sanitizers, Metal trace, limited
`leaks` report, and process-level RSS/footprint benchmark are substitutes only
for the portions they exercise.
