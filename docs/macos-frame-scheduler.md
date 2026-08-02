# macOS frame scheduling

This document defines the contracts for the macOS frame scheduler.  It covers
the Cocoa window redraw path, hosted OpenGL
IOSurfaces, native Metal swapchains, and the DXGI presentation queues which
feed those paths.

## Layering

The scheduler has two deliberately separate responsibilities:

1. A display timeline records monotonic Core Video output timestamps and the
   most recently observed interval for each display.  Cocoa redraws and hosted
   WGL surfaces consume the same timeline when they live in the same process.
   A process without a live Cocoa display link uses the current public
   CoreGraphics display mode as a prediction source.
2. The DXGI and graphics backends retain ownership of queue depth and producer
   back-pressure.  The scheduler neither consumes nor signals DXGI frame
   latency objects.  D3D11 reaches the scheduler only when wined3d ultimately
   swaps a winemac WGL surface.  Wine's D3D12 Vulkan backend retains its worker,
   semaphore, fence, and `vkQueuePresentKHR` ordering.  D3DMetal and MoltenVK
   retain ownership of their `CAMetalLayer` drawables.

This is one scheduling model, not one forced Apple callback API.  It shares a
timeline and deadline policy where Wine owns presentation, while preserving
the native producer contract where an external backend owns presentation.

## Timing invariants

- All deadlines and display timestamps use Mach absolute time.  No wall-clock
  value participates in ordering.
- Nanoseconds, display rates, and Mach ticks are converted with saturating
  multiply/divide operations.  Addition and interval multiplication saturate
  instead of wrapping.
- A reserved deadline is phase anchored.  The next reservation is calculated
  from the prior reserved deadline, never from the time at which the previous
  frame happened to complete.  A predictive Core Video phase seeds a new
  generation, but later callbacks cannot shorten an already-reserved interval.
  Long-running drift is therefore bounded by timestamp accuracy rather than
  accumulated work time.
- The first frame after creation, resume, display migration, timing-generation
  change, or a long discontinuity is not delayed.
- A missed deadline does not cause a burst of catch-up presents.  The late
  producer may present once immediately; while holding its per-target producer
  lock it reserves the first strictly future phase for the next producer.
- A predicted wait greater than the discontinuity limit is discarded.  This
  handles sleep, debugger stops, stopped display links, and stale hotplug state
  without a long sleep or busy polling.
- `mach_wait_until()` is called at most once for a reservation.  Resize,
  suspend, surface replacement, and teardown locks are not held while waiting.

For a swap interval greater than zero, the effective interval is the display
interval multiplied by the requested swap interval.  The optional
`ForeignSurfaceMaxFrameRate` period is a lower bound on that interval.  A swap
interval of zero and a disabled maximum rate do not add scheduler delay.
Wine's macOS driver historically treats a negative adaptive-tear interval as
its overflow-safe magnitude; the foreign path preserves that behavior.
When a non-integral maximum-rate period dominates display synchronization, it
uses the scheduler's reserved phase rather than being re-anchored to every
display callback; the configured maximum can therefore never be exceeded.

Hosted foreign surfaces bound any single uninterruptible Mach wait to one
second.  An application-supplied WGL interval which requests a lower cadence is
clamped to one hertz on this path.  This explicit resource boundary prevents a
corrupt or hostile interval from blocking surface teardown for minutes or
years; ordinary intervals and every supported `ForeignSurfaceMaxFrameRate`
value (1 through 1000) retain their requested cadence.

## State transitions

- Display migration is selected by maximum intersection with the Win32 window
  rectangle.  Ties retain the current display, then prefer the main display.
- Display configuration notifications invalidate cached timelines even when a
  display ID is unchanged.  This covers mode and refresh-rate changes as well
  as hotplug.  Cached foreign-window geometry is also revalidated periodically
  because a GPU helper can be in a different process from the Cocoa host.
- Predictive Core Video output host timestamps seed the display phase.  The
  timestamp's nominal refresh period and measured rate scalar supply the
  current interval; consecutive-host deltas are used only when Core Video does
  not provide that hint.  Non-monotonic, implausible, or stale samples are
  rejected and fall back to the last valid mode period, then 60 Hz.  This
  avoids treating a missed callback as an actual variable-refresh transition.
- Invisible, minimized, empty-shaped, off-active-space, and occluded Cocoa
  windows leave their display link.  A zero-sized foreign target is suspended
  and preserves its last complete IOSurface generation.  Resume resets its
  deadline before the next present.
- System sleep stops Cocoa display links.  Wake and display-configuration
  changes recreate their Core Video source and invalidate accumulated phase.
  Background-but-visible windows may continue to draw; fully hidden or
  occluded windows do not keep a display link alive.

## Lock and callback ownership

The required lock order is:

```text
foreign_surface_mutex (registry only)

pacing_mutex -> present_mutex
```

`foreign_surface_mutex` is never nested with a target mutex.  `pacing_mutex`
serializes producers and is the only lock held across a deadline wait.
`present_mutex` protects the published backing generation, suspension flag,
present ring index, and Cocoa layer pointer.  IOSurface allocation and binding
construction remain outside it.

Display timeline publication is a lock-free sequence snapshot.  The Core Video
callback performs no allocation, logging, Objective-C retain/release, AppKit
call, or target lock acquisition.  At most one main-queue redraw block per
display link may be pending, preventing callback re-entry and unbounded queue
growth.  Window-set mutation and AppKit drawing remain on the main thread.

The display timeline registry uses fixed process-lifetime slots, so readers can
retain a slot pointer without a callback-time UAF or reclamation lock.  Final
unregister advances the timing generation and publishes a zero period before
reuse.  Consumers also validate the snapshot's display ID and period, so a
stale pointer falls back instead of consuming the next display assigned to
that slot.

`foreign_surface_target` owns its scheduler state and its Cocoa IOSurface
layer.  Registry ownership and drawable references are unchanged.  The final
drawable detach removes registry ownership before target destruction; the
Cocoa layer is invalidated before its pending surface is released.

DXGI lock ownership is intentionally unchanged.  In particular, no macOS
scheduler wait occurs while holding the D3D12 worker critical section, no
scheduler callback signals the frame-latency semaphore, and the public
waitable handle is still signaled only when the backend present operation
returns its token.

## Apple API boundaries and fallbacks

`CVDisplayLink` remains the public display clock for AppKit and OpenGL-backed
content.  It supplies display-specific host timestamps without taking
ownership of a Metal drawable.

`CAMetalDisplayLink` is available starting in macOS 14, which matches the
Switchyard dependency build target, but its update object owns a
`CAMetalDrawable`.  Switchyard hands local and remote `CAMetalLayer` instances
to D3DMetal or MoltenVK, whose command queues already obtain and present those
drawables.  Creating a second drawable owner would change queue depth, frame
latency, and shutdown ordering.  Consequently it is not installed on those
layers.  Native `CAMetalLayer` display synchronization and drawable
back-pressure are the validated fallback, with `CVDisplayLink` used only for
Wine-owned Cocoa redraw timing.

There is no public macOS API which promises or reports direct scan-out
eligibility for an arbitrary `CALayer`/`CAContext` tree.  Opaque fullscreen,
pixel format, scaling, overlays, color management, capture state, and the
WindowServer compositor all affect that private decision.  Switchyard does not
claim eligibility and does not use private APIs to force it.  Eligible opaque
fullscreen Metal content is left for Core Animation and WindowServer to
optimize automatically; all other cases use the same composited fallback.

## Verification contract

The focused scheduler tests cover saturating timing conversions, extreme
deadlines, missed-frame catch-up, long discontinuities, generation changes,
display migration, suspend/resume, and concurrent timeline publication.  DXGI
tests continue to cover waitable-object duplication, maximum-latency changes,
non-waitable producer blocking, and `DXGI_PRESENT_DO_NOT_WAIT`.  Switchyard's
D3DMetal D3D11/D3D12 and Chromium GPU-probe smoke tests cover the native Metal
ownership boundary.  The D3D12 smoke performs 240 flip-discard presents with a
frame-latency handle, bounded GPU fences, minimize/restore, and `ResizeBuffers`.
The hosted compositor tests cover attach/present/coalesce, resize, stale serial
completion, detach, and shutdown.

Performance evidence must record the exact source revision, display mode,
power state, prefix, runtime, and scenario.  Report p50/p95/p99 present interval
or present-to-completion proxy, process wakeups per second, idle CPU after the
two-second redraw idle threshold, and a present-to-waitable/fence completion
latency proxy.  Host tools which cannot observe physical scan-out are labeled
as proxies rather than input-to-photon measurements.
