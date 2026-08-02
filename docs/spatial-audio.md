# Spatial audio

Switchyard Wine implements `ISpatialAudioClient` and
`ISpatialAudioObjectRenderStream` on macOS with the public CoreAudio Spatial
Mixer Audio Unit. Static beds, non-spatial dialogue, and dynamic point sources
share one render timeline; dynamic objects are never folded into arbitrary bed
channels.

## Capability policy

`winecoreaudio.drv` resolves the concrete render endpoint and reads its current
nominal sample rate and buffer frame size. It creates a
`kAudioUnitSubType_SpatialMixer`, reads the unit's default input-element count,
reserves input zero for the ambience bed, and limits the remainder to the
driver's bounded 64-PointSource real-time design. Before returning a nonzero
count, it configures and initializes a complete 48 kHz object graph for that
endpoint. If the Audio Unit element count is larger than the endpoint can
initialize, a bounded binary search finds the highest viable PointSource count.
A failed one-object probe reports zero dynamic objects while preserving the
real endpoint timing; it never advertises a speculative count.

The capability snapshot includes an opaque generation derived from the device
identity, buffer frame size, nominal sample rate, output channel count, and
channel mask. Stream creation repeats the query and rejects a stale snapshot.
Device listeners are installed before graph construction, and activation
repeats the snapshot after initialization and drains already-queued property
notifications, closing the query/listener registration window.
The processing quantum is the endpoint buffer duration converted to the
object format with one common rounding path. Other Wine audio backends report
zero dynamic objects and retain their existing spatial-stream fallback.

The implementation exposes neither Dolby Atmos nor motion/head-tracking
support. Head tracking is explicitly disabled. The mixer uses Apple's
`UseOutputType` algorithm, with the public HRTFHQ or vector-panning algorithm
only when the unit rejects that setting. Personalized HRTF selection is left
in Apple's automatic mode where the public API provides it; this does not
claim a Windows head-tracking capability.

## Object contract and transport

The private interleaved float transport contains, in order:

1. the requested static bed plus the front-left/front-right channels needed to
   keep a valid speaker bed;
2. one private dry-dialogue channel; and
3. one private mono channel for every dynamic slot reserved at activation.

Only the bed and dry channels appear in the WAVE channel mask. The backend
removes the dry channel before configuring the ambience-bed input. Dynamic
channel `n` feeds Spatial Mixer input `n + 1` in PointSource mode. Each submitted
quantum carries a bounded sideband snapshot containing that slot's position and
valid-frame prefix. The sideband record is published before the matching audio
write cursor, and the callback acquires both from the same absolute frame
timeline. This preserves alignment even when a CoreAudio render slice crosses
one or more Windows update boundaries.

An object's first successful `GetBuffer` starts its lifetime. Thereafter an
update without `GetBuffer` is implicit end-of-stream. Explicit end-of-stream
renders only the requested prefix. `IsActive` becomes false immediately after
either form of end-of-stream, but the dynamic capacity is not reusable until
the object is released. A release during an update is deferred until the final
quantum has been submitted. Reset revokes every live object. Position and
volume persist between updates; object volume is applied to the mono samples
before transport, while session and stream volume remain backend channel gains.

## Coordinates and channel mapping

Windows positions are listener-relative metres in a right-handed coordinate
system: positive X is right, positive Y is up, and positive Z is behind the
listener. A dynamic point `(x, y, z)` becomes:

```text
azimuth   = atan2(x, -z) * 180 / pi
elevation = atan2(y, hypot(x, z)) * 180 / pi
distance  = hypot(x, y, z) metres
```

Azimuth, elevation, and distance changes are scheduled with CoreAudio's
real-time parameter-event API at their exact offsets within the current render
slice. Non-finite input is rejected at the COM boundary and checked again at
the Unix boundary. Distance is limited to CoreAudio's documented 10,000-metre
parameter range. Static bottom speakers, which have no WAVE mask bits, retain
the private rectangular-coordinate mapping described in `unixlib.h`.

Stereo and multichannel endpoint layouts are translated to explicit CoreAudio
channel descriptions. Object audio remains mono float at the Windows object
rate; the output Audio Unit performs endpoint sample-rate and device-format
conversion. The dry channel is injected into the physical front pair, or the
front centre when no pair exists, after spatial rendering. It is delayed by
the Spatial Mixer latency so dialogue and the rendered bed remain aligned.

## Real-time and lifetime rules

The spatial output callback may only:

- read or publish fixed-size atomics;
- copy or multiply samples in preallocated buffers;
- fill preallocated parameter events;
- call CoreAudio APIs documented as real-time safe; and
- signal the pre-created, coalescing Mach semaphore used by the event thread.

It performs no allocation, deallocation, blocking lock, Objective-C operation,
file I/O, or logging. Audio and metadata rings, bus contexts, dry-delay storage,
dynamic planes, and parameter-event arrays are bounded and allocated before the
Audio Unit starts. Producer and consumer cursors are placed on distinct cache
lines. The callback walks ring and quantum boundaries incrementally; static-only
streams execute no dynamic metadata division, and dynamic streams perform only
the initial period calculation for a slice. A callback-reentrancy guard refuses
a second concurrent render rather than sharing scratch state. Denormal handling
is left to CoreAudio and the host floating-point mode so the static arithmetic
path is not perturbed.

The stream owns both Audio Units, all bus contexts and buffers, the duplicated
Windows event, and the notification thread. Teardown marks the stream shutting
down, unregisters device listeners, and synchronously drains their private
serial queue. The listener block's shared target is detached on that queue, so
even failed removal on a dead device cannot retain a usable stream pointer. It
then stops and joins the notification thread, stops the output Audio Unit,
waits for the bounded callback-inflight count to reach zero, and only then
uninitializes units and frees storage. Reset first makes callbacks silent,
waits for an in-flight slice, resets cursors and delay state, and then re-enables
rendering. These orders prevent callbacks from observing freed event handles,
bus contexts, or Audio Units.

## Endpoint changes and recovery

Spatial streams are pinned to the endpoint used for activation. Device liveness,
device-change, buffer-size, nominal-rate, output-layout, and abnormal-I/O-stop
properties have public CoreAudio block listeners. Activation fails closed unless
liveness, buffer-size, sample-rate, and at least one topology-change listener
(`DeviceHasChanged` or output `StreamConfiguration`) are registered. A format,
topology, liveness, unplug, or abnormal-stop change sets a sticky invalidation
flag and wakes the Windows event thread; subsequent client operations return
device invalidation and spatial objects are revoked. Replug, Bluetooth/USB
transition, sleep/wake, or a new default endpoint is recovered by obtaining a
new endpoint and reactivating a stream, which rebuilds and revalidates the
graph. Existing streams are never silently moved onto a different layout.
Processor-overload listeners are deliberately not registered through the block
API because CoreAudio may deliver that selector on its I/O context; callback
starvation is counted directly as an underrun instead.

## Security and resource boundaries

The activation blob is treated as untrusted input. The COM layer verifies the
`PROPVARIANT` type, blob size, blob pointer, event handle, object format, static
mask, and dynamic min/max range before retaining any object references. Dynamic
capacity is capped by the validated endpoint capability and by the fixed
64-object backend design; no executable name, title, or caller identity is used
to change capabilities.

The Unix boundary revalidates every render submission: sideband object count
must equal the reserved dynamic count, object metadata must be present when the
count is nonzero, written frames must equal one spatial quantum, coordinates
must be finite, and each active-frame prefix must fit in the submitted period.
Endpoint generation checks reject stale capability snapshots after device
identity, period, sample-rate, or layout changes.

The real-time callback consumes only preallocated buffers and bounded arrays.
The largest memory commitments are derived from the validated period, transport
channel count, and dynamic object count with integer-overflow checks before
allocation. There is no callback-side file I/O, logging, blocking lock,
Objective-C allocation, heap allocation, or unbounded growth. Shutdown and reset
mark the stream inactive, drain queued device/listener callbacks, wait for
in-flight render callbacks, and only then release Audio Units, event handles,
and buffers.

## Verification and profiling

The mmdevapi spatial tests cover malformed activation blobs and ranges, zero
and maximum capacity, combined static masks, exhaustion and slot reuse,
persistent position and volume, invalid coordinates, explicit and implicit
end-of-stream, reset, release during an update, and concurrent object release.
Build both the client and the CoreAudio Unix module before running the test:

```sh
make dlls/mmdevapi/all dlls/winecoreaudio.drv/all
make test TESTDLL=mmdevapi.dll
```

Set `SWITCHYARD_SPATIAL_AUDIO_PROFILE=1` for a diagnostic run. The driver then
preallocates 65,536 timing entries and prints one teardown summary with callback
p50/p95/p99, native/dynamic path labels, underrun, overrun, and reentrant counts,
and the callback allocation count.
The profiling storage and clock reads are absent when the variable is unset.
For release evidence, capture the same static-only and maximum-dynamic workloads
with Instruments `Audio System Trace`, `Time Profiler`, `Allocations`, and
`Leaks`; record end-to-end latency from `IAudioClient::GetStreamLatency`, peak
RSS, CPU, and allocation deltas alongside the driver summary. A soak must cover
at least one hour and include repeated stop/reset/release and endpoint changes.

Source validation should include the normal build with warnings enabled,
Clang static analysis, ASan/UBSan where the Wine host configuration supports
them, and TSan in a dedicated diagnostic build. Sanitizer limitations for PE
code or translated x86_64 processes must be stated with the results rather than
treated as a pass.

### Hardware evidence for this change

The development host's built-in stereo endpoint reported 48,000 Hz, 512-frame
buffers (10.6667 ms), a 15-4096 frame range, and the Spatial Mixer default of
32 input elements. A complete 31-PointSource graph configured and initialized,
so that endpoint advertises 31 dynamic objects. `auval -v aumx 3dem appl`
validated Apple's `AUSpatialMixer` component and parameter scheduling across
the host sample-rate set, including 44.1 kHz, 48 kHz, 96 kHz, and 192 kHz.

The focused x86_64 Wine run completed
`dlls/mmdevapi/tests/mmdevapi_test.exe spatialaudio` with 729 tests executed,
2 existing todo results, 0 failures, and 0 skipped tests. The arm64 PE harness
in this worktree was also built, but `tools/runtest` was terminated by the
environment with `Killed: 9` before producing test results, so it is not used
as pass/fail evidence.

With `SWITCHYARD_SPATIAL_AUDIO_PROFILE=1 WINEDEBUG=+coreaudio`, the same
focused test produced:

```text
static spatial mixer: native 0, dynamic objects 0, samples 23,
  p50 48958 ns, p95 61458 ns, p99 65375 ns,
  underruns 0, overruns 0, reentrant rejects 0, callback allocations 0
maximum dynamic: native 0, dynamic objects 31, samples 2,
  p50 306208 ns, p95 306208 ns, p99 306208 ns,
  underruns 0, overruns 0, reentrant rejects 0, callback allocations 0
```

`/usr/bin/time -l` on the focused x86_64 run reported 1.50 s real time,
0.25 s user CPU, 0.18 s system CPU, 93,564,928-byte maximum resident set size,
and an 87,902,992-byte peak memory footprint. This is a short functional and
callback-metric run, not a long soak or full Instruments capture.

Compilation and Clang static analysis cover the stereo/multichannel and
conversion branches, but this change has not claimed application compatibility
or physical USB, Bluetooth, HDMI multichannel, external sample-rate switching,
sleep/wake, or head-tracked hardware validation. Those devices require the
release matrix above; no entry is added to `docs/compatibility.md` without such
an application run.
