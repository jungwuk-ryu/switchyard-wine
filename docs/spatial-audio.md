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
Windows event, and the backend event thread. A dynamic stream with a notification
object also owns an internal invalidation event and a dedicated MTA worker. The
CoreAudio property-listener queue signals that event directly; invalidation found
by the render callback is relayed through the existing Mach semaphore and backend
event thread, so application code is never entered from an audio callback. The
relay is created before `AudioOutputUnitStart`, closing the activation-to-Start
window in which the Audio Unit can already issue a render callback. The
worker marks every object inactive and invokes
`OnAvailableDynamicObjectCountChange(..., 0, 0)` exactly once without holding the
stream lock. It takes a transient reference only if the stream reference count is
still nonzero, preventing both reference resurrection and a final-release race.
Activation waits for the worker's MTA initialization result, and its start gate
remains closed until the first successful `Start`. Public methods poll and make
sticky any endpoint loss that arrives before that gate opens, without invoking
application code. A `Start` attempt publishes already-recorded endpoint loss
synchronously; a clean stream is not armed while backend `Start` is in progress,
then is repolled and armed only after backend success. Thus a worker cannot
observe unpublished activation state, and a concurrent call cannot expose a
callback before the start boundary.

Teardown marks the stream shutting
down, unregisters device listeners, and synchronously drains their private
serial queue. The listener block's shared target is detached on that queue, so
even failed removal on a dead device cannot retain a usable stream pointer. It
then stops and joins the backend event thread, stops the output Audio Unit,
waits for the bounded callback-inflight count to reach zero, and only then
uninitializes units and frees storage. Reset first makes callbacks silent,
waits for an in-flight slice, resets cursors and delay state, and then re-enables
rendering. The COM stream's final release wakes and joins its invalidation worker
before releasing the notification object, then releases the audio client (which
drains the CoreAudio listener queue) before closing the internal event. If final
release occurs on the worker after a reentrant callback, it skips a self-join.
These orders prevent callbacks from observing freed event handles, notification
objects, bus contexts, or Audio Units.

## Endpoint changes and recovery

Spatial streams are pinned to the endpoint used for activation. Device liveness,
device-change, buffer-size, nominal-rate, output-layout, and abnormal-I/O-stop
properties have public CoreAudio block listeners. Activation fails closed unless
liveness, buffer-size, sample-rate, abnormal-I/O-stop, and at least one
topology-change listener (`DeviceHasChanged` or output `StreamConfiguration`)
are registered. A format,
topology, liveness, unplug, or abnormal-stop change sets a sticky invalidation
flag, wakes the Windows render event, and proactively schedules the dynamic-count
callback on the COM worker after the stream has started, without requiring
another client API call. Before Start, the event remains sticky and the next
public stream or object method revokes capacity without allowing a premature
application callback. Subsequent
spatial-stream operations return `SPTLAUDCLNT_E_RESOURCES_INVALIDATED`, and
spatial objects remain revoked.
Replug, Bluetooth/USB
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
end-of-stream, reset, release during an update, concurrent object release,
pre-Start endpoint loss, backend-relayed endpoint loss, one-shot notification,
and notification callbacks that reenter or release the stream.
Build both the client and the CoreAudio Unix module before running the test:

```sh
make dlls/mmdevapi/all dlls/winecoreaudio.drv/all
make test TESTDLL=mmdevapi.dll
```

Set `SWITCHYARD_SPATIAL_AUDIO_PROFILE=1` for a diagnostic run. The driver then
preallocates 65,536 timing entries and prints one teardown summary with callback
p50/p95/p99, native/dynamic path labels, underrun, overrun, and reentrant counts,
and the structurally audited callback allocation count. The latter is zero
because no callback-reachable path calls an allocator; it is not a replacement
for an allocation profiler. The COM layer also reports the value returned
by `IAudioClient::GetStreamLatency` together with the selected endpoint period.
The profiling storage and clock reads are absent when the variable is unset.
Set `SWITCHYARD_SPATIAL_AUDIO_FAULT_INVALIDATE_ON_START=1` only in a test
process to invalidate an eligible dynamic stream after `Start`. This
bounded, one-shot fault exercises the same atomic flag, Mach-semaphore relay,
MTA notification worker, one-shot callback, reentrant stream reference, and
teardown path as a callback-detected endpoint loss without changing a physical
device. It adds no render-callback branch and is inert unless its value is
exactly `1`.
`SWITCHYARD_SPATIAL_AUDIO_FAULT_INVALIDATE_BEFORE_START=1` is a separate,
COM-side test hook that sets the private event after activation publication. It
isolates the pre-Start polling and callback gate; it is not used as evidence for
the CoreAudio relay, which the `...ON_START` fault covers end to end.
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

The final threat-oriented review follows the activation blob and duplicated
handles across the PE/Unix boundary, checks every dynamic-count and frame-size
allocation for a fixed capability bound or overflow guard, and proves that the
backend is released before the private invalidation handle is closed. It also
checks the listener-detach barrier, relay/worker joins, callback transient
references, reentrant final release, one-shot notification state, and every
error-unwind branch for leak, UAF, double release, or lock-order inversion. The
test-only fault variables accept exactly the value `1`; they do not alter
capability discovery, filesystem paths, permissions, or caller identity. No
actionable security finding remained after the independent review fixes.

### Hardware evidence for this change

The development host's built-in stereo endpoint reported 48,000 Hz, 512-frame
buffers (10.6667 ms), a 15-4096 frame range, and the Spatial Mixer default of
32 input elements. A complete 31-PointSource graph configured and initialized,
so that endpoint advertises 31 dynamic objects. `auval -v aumx 3dem appl`
validated Apple's `AUSpatialMixer` component and parameter scheduling across
the host sample-rate set, including 44.1 kHz, 48 kHz, 96 kHz, and 192 kHz.

The focused x86_64 Wine run completed
`dlls/mmdevapi/tests/mmdevapi_test.exe spatialaudio` with 2,998 tests executed,
2 existing todo results, 0 failures, and 0 skipped tests. The arm64 PE harness
and both arm64 and x86_64 CoreAudio Unix modules built with warnings enabled.
The arm64 harness was not executed, so only x86_64 is used as runtime evidence.

With `SWITCHYARD_SPATIAL_AUDIO_PROFILE=1 WINEDEBUG=+mmdevapi,+coreaudio`, the same
focused test produced:

```text
static spatial mixer: native 0, dynamic objects 0, samples 22,
  p50 55500 ns, p95 93875 ns, p99 98708 ns,
  underruns 0, overruns 0, reentrant rejects 0, callback allocations 0
backend invalidation fault: native 0, dynamic objects 1, samples 10,
  p50 667 ns, p95 1042 ns, p99 1042 ns,
  underruns 0, overruns 0, reentrant rejects 0, callback allocations 0
maximum dynamic: native 0, dynamic objects 31, samples 65,
  p50 209666 ns, p95 374625 ns, p99 458584 ns,
  underruns 0, overruns 0, reentrant rejects 0, callback allocations 0
```

The static path, backend fault, and sustained 64-quantum maximum-object workload
recorded no underrun, overrun, reentrant rejection, or callback-side allocation.

The selected 512-frame period was `106667` 100-ns units (10.6667 ms).
`IAudioClient::GetStreamLatency` normally returned `279584` 100-ns units
(27.9584 ms); one activation measured 27.2917 ms. `/usr/bin/time -l` on this
profiled run reported 2.56 s real time, 0.26 s user CPU, 0.16 s system CPU,
92,893,184-byte maximum resident set size, and an 85,629,400-byte peak memory
footprint.

The final unprofiled soak ran the complete focused test 1,753 times in 3,601
seconds. Every iteration executed 2,998 checks with the same two existing todo
results, zero failures, and zero skipped checks. Per-process maximum RSS was
88,850,432 bytes on the first iteration and 88,752,128 bytes on the last, with
a 90,708,415-byte mean, 76,906,496-byte minimum, and 103,784,448-byte maximum;
there was no monotonic RSS growth. Summed process CPU was 461.99 s user and
247.35 s system. This repeatedly covers activation, maximum-object rendering,
backend fault relay, callback reentrancy/final release, and full teardown in a
shared Wine prefix.

The broader x86_64 mmdevapi run also completed `capture` (9,941 checks),
`mmdevenum` (72), and `propstore` (42) with zero failures. The `render` test's
old fixed-10-ms assertion was replaced with a comparison against
`IAudioClient3::GetSharedModeEnginePeriod`; the 512-frame endpoint period then
passed. That temporary runtime prefix still reports unrelated failures from
`QueryFullProcessImageNameW(PROCESS_NAME_NATIVE)` (`ERROR_NO_SUCH_DEVICE`) and
`GetSessionIdentifier` (`E_FAIL`), while `dependency` lacks two optional COM
class registrations. Those results are recorded as environment limitations,
not as a green full-suite claim.

Final `Time Profiler` and `Audio System Trace` captures both completed with the
same 2,998-test result. `Allocations` and `Leaks` launched and completed the
test, but Instruments could not attach its allocation agent because the Wine
target is restricted while System Integrity Protection is enabled; their trace
files are therefore not treated as allocation or leak evidence. Clang static
analysis reported no findings in the CoreAudio backend, COM implementation, or
focused tests. The final ASan/UBSan module built, but the translated x86_64
Wine process hit AddressSanitizer's host-address-space `unable to unmap` check
before reaching the test, so its ASan run supplies no memory-safety result. A
separate UBSan-only build completed all 2,998 checks with no diagnostic. The
final TSan module ran all 2,998 checks with no TSan warning, then exited 137 during
the Wine/Rosetta sanitizer teardown; this is supporting race evidence, not a
clean sanitizer-process exit. LeakSanitizer is unavailable on this macOS
configuration.

Compilation and Clang static analysis cover the stereo/multichannel and
conversion branches, but this change has not claimed application compatibility
or physical USB, Bluetooth, HDMI multichannel, external sample-rate switching,
sleep/wake, or head-tracked hardware validation. Those devices require the
release matrix above; no entry is added to `docs/compatibility.md` without such
an application run.
