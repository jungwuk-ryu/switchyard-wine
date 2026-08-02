# macOS frame scheduler validation

This record accompanies the implementation contract in
`docs/macos-frame-scheduler.md`.  It identifies the executable inputs, keeps
the performance samples needed to reproduce the comparison, and records the
correctness, concurrency, lifetime, and threat-oriented review that was
performed before integration.

## Test environment

- Host: Apple M5 Pro, macOS 26.5.2 (25F84), internal 3456 x 2234 Retina main
  display, AC power, and no recorded thermal or performance warning.
- Baseline runtime revision:
  `8c55fb3a70c5d8822a16c3945321e5a1e9cb476a`.
- Baseline runtime manifest SHA-256:
  `698e9f0ac9c1289d5678368b4c9882de9a80e574bd29ea77b5d5056141adb3f7`.
- Measured candidate runtime revision:
  `5e48564a530ce62dcb2388ff35a7c4e100691a01`.
- Candidate runtime manifest SHA-256:
  `caac6aede75acb7ff36c454ab90ee54ae240d2d56e8b4aa9e98dbb2caed81c23`.
- Graphics overlay: Game Porting Toolkit 4.0 beta 1 from
  `Evaluation_environment_for_Windows_games_4_0_beta_1-c8def0e99dc99794`,
  injected into otherwise `no-gptk` runtimes with identical environment
  variables.
- Configuration: default scheduler registry values, `WINEDEBUG=-all`, a fresh
  prefix shared by the five runs for each D3D11 data set, and a fresh prefix
  for each foreign-WGL run.

The baseline harness printed `sourceDirty=true` because the measurement
programs were then uncommitted in the source worktree.  The executable under
test was the immutable runtime named for `8c55fb3a70c5`; its manifest hash and
revision above identify it independently of that live-tree diagnostic.  The
candidate harness printed `sourceDirty=false`, and its runtime manifest
revision exactly matched the candidate commit.

The internal display did not expose a fixed refresh rate through
`system_profiler`; the approximately 8.33 ms cadence observed in both DXGI
sets is the active 120 Hz ProMotion cadence.  No fixed-refresh assumption was
injected by the harness.

## Method

The D3D11 benchmark creates a flip-discard swapchain with
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, sets maximum frame
latency to one, and performs 1,200 interval-one presents after warm-up.  It
reports:

- frame interval from consecutive frame-latency handle signals;
- completion proxy from `Present` return to the next handle signal;
- display-latency proxy from `Present` entry to the next handle signal;
- synchronous `Present` call duration;
- process CPU time during the active loop and during a three-second idle
  sample taken after the Cocoa display link's two-second idle threshold.

The foreign-WGL test places the Win32 owner and WGL renderer in separate
processes, requests swap interval one, and exercises move, resize, minimize,
eight minimized swaps, restore, 600 timed swaps, DC reacquisition, pixel
format persistence, and teardown.  The baseline failure count is retained
because it is the behavior fixed by the candidate.

`/usr/bin/time -lp` supplies voluntary and involuntary context-switch totals.
Their sum divided by elapsed time is reported as a **wakeup proxy**, not as a
hardware or scheduler wakeup count.  `powermetrics` could not be used without
superuser authority.  Instruments' Power Profiler reports that it is not
supported on macOS.  These are exact tool limitations, so no stronger CPU
wakeup claim is made.

The input-to-display value is likewise a proxy: this host has no photodiode or
instrumented input device.  The time from `Present` entry to the next DXGI
frame-latency signal is the closest repeatable software boundary.  It includes
queue completion and scheduling but is not physical input-to-photon latency.

Reproduction commands, with `RUNTIME` and `GPTK` pointing at the recorded
artifacts, are:

```sh
./switchyard/tests/frame_scheduler_present_benchmark.sh "$RUNTIME" "$GPTK" 5
./switchyard/tests/foreign_wgl_pixel_format_test.sh "$RUNTIME"
./switchyard/tests/d3dmetal_dxgi_resource_smoke_test.sh "$RUNTIME" "$GPTK"
./switchyard/tests/d3dmetal_d3d12_smoke_test.sh "$RUNTIME" "$GPTK"
SWITCHYARD_RUN_TSAN=1 ./switchyard/tests/macos_frame_scheduler_test.sh
```

## Raw performance samples

Run 1 in each D3D11 set is a cold-prefix run and is retained below, but the
summary compares the median of warm runs 2 through 5.  Times are microseconds;
CPU is milliseconds; context switches per second are the wakeup proxy.

### D3D11 baseline

| Run | Interval p50/p95/p99 | Completion p50/p95/p99 | Display proxy p50/p95/p99 | Present p50/p95/p99 | Active/idle CPU | Ctx/s |
| --- | --- | --- | --- | --- | --- | ---: |
| 1 cold | 8329 / 8690 / 8946 | 8304 / 8663 / 8896 | 8324 / 8683 / 8935 | 18 / 40 / 52 | 38 / 0 | 1541.567 |
| 2 | 8332 / 8607 / 8950 | 8303 / 8584 / 8901 | 8325 / 8599 / 8938 | 20 / 45 / 63 | 38 / 0 | 2725.238 |
| 3 | 8330 / 8708 / 8949 | 8307 / 8672 / 8931 | 8324 / 8698 / 8946 | 18 / 39 / 58 | 38 / 0 | 2731.327 |
| 4 | 8338 / 8664 / 8832 | 8313 / 8631 / 8804 | 8333 / 8659 / 8824 | 17 / 39 / 78 | 35 / 0 | 2712.690 |
| 5 | 8328 / 8660 / 8845 | 8307 / 8636 / 8816 | 8323 / 8655 / 8839 | 16 / 34 / 49 | 34 / 0 | 2733.390 |

### D3D11 candidate

| Run | Interval p50/p95/p99 | Completion p50/p95/p99 | Display proxy p50/p95/p99 | Present p50/p95/p99 | Active/idle CPU | Ctx/s |
| --- | --- | --- | --- | --- | --- | ---: |
| 1 cold | 8333 / 8490 / 11644 | 8316 / 8474 / 11627 | 8329 / 8487 / 11640 | 13 / 21 / 46 | 28 / 0 | 1566.345 |
| 2 | 8332 / 8396 / 8465 | 8317 / 8378 / 8443 | 8329 / 8392 / 8461 | 12 / 19 / 26 | 26 / 0 | 2797.598 |
| 3 | 8331 / 8396 / 8467 | 8315 / 8381 / 8452 | 8327 / 8393 / 8463 | 12 / 19 / 25 | 26 / 0 | 2778.139 |
| 4 | 8330 / 8557 / 8875 | 8313 / 8536 / 8821 | 8327 / 8552 / 8868 | 14 / 39 / 54 | 32 / 0 | 2759.282 |
| 5 | 8339 / 8784 / 9154 | 8314 / 8755 / 9092 | 8334 / 8778 / 9135 | 21 / 43 / 54 | 41 / 0 | 2700.507 |

Warm-run medians preserve the 8.33 ms p50 cadence.  Interval p95 changes from
8662 to 8476.5 microseconds (-2.1%), interval p99 from 8897 to 8671 (-2.5%),
and the display-latency proxy p95/p99 from 8657/8888.5 to 8472.5/8665.5
microseconds (-2.1%/-2.5%).  Active CPU changes from 36.5 to 29 ms (-20.5%)
and idle CPU remains zero.  The context-switch proxy changes from 2728.3/s to
2768.7/s (+1.5%); this small adverse movement is retained rather than
presented as an improvement.

### Foreign WGL

| Revision/run | Interval p50/p95/p99 us | Minimized failures | Elapsed s | Ctx/s |
| --- | --- | ---: | ---: | ---: |
| Baseline | 399 / 1078 / 2257 | 8 | 20.00 | 584.600 |
| Candidate 1 cold | 8318 / 10727 / 11766 | 0 | 53.21 | 260.477 |
| Candidate 2 | 8250 / 11331 / 11964 | 0 | 24.19 | 456.842 |
| Candidate 3 | 8327 / 10383 / 12252 | 0 | 24.01 | 444.856 |

The baseline interval is not a successful latency result: the foreign path
ignored the requested swap interval and failed every minimized swap.  The
candidate intentionally supplies the missing approximately 8.33 ms
back-pressure and therefore spends about five additional seconds on 600
frames.  It eliminates the eight API failures and reduces the warm
context-switch proxy from 584.6/s to a 450.8/s median (-22.9%).  Candidate p95
and p99 include WindowServer scheduling tails during the move/resize stress;
all three runs remain bounded without catch-up bursts or busy spinning.

## Correctness and stress evidence

- A clean, full wow64 runtime build from the rebased candidate succeeded, and
  the installed manifest matched the candidate source revision.
- ASan and UBSan timing/state tests passed, followed by 20 repeated soak runs.
- TSan passed one writer with three concurrent snapshot readers, 250,000
  publications, configuration churn, and display-clock slot reuse.
- Timing tests covered nanosecond/tick saturation, `UINT64_MAX` deadlines,
  1 Hz bounding, 100,000-frame no-drift sequences, 100,000 randomized deadline
  transitions, non-integral rate caps, predictive phases, late frames, and
  non-monotonic callback samples.
- Display tests covered largest-intersection selection, current/main tie
  preference, offscreen fallback, same-ID configuration changes, hotplug,
  display migration, active/inactive transitions, suspend/resume reset, and
  clock-slot generation reuse.
- The foreign-WGL test passed three times with cross-process ownership,
  move/resize/minimize/restore, zero minimized failures, stable pixel format,
  DC reacquisition, and clean detach.
- The D3D11 resource smoke passed callback attribution, cross-process shared
  textures, concurrent owner release, and post-release callback joining.
- The D3D12 suite passed callback and descriptor-churn stress, the Chromium
  GPU-process fallback probe, and 240 flip-discard presents using a latency-one
  waitable object, bounded fence waits, minimize/restore, and `ResizeBuffers`.
- `clang --analyze`, MinGW `-Wall -Wextra -Werror` builds for all three new
  Windows programs, ShellCheck, `git diff --check`, and
  `switchyard/verify_source.sh` passed.
- The standalone scheduler reported zero leaks under `leaks --atExit`.
  LeakSanitizer reports that leak detection is unsupported on this macOS
  platform.  Instruments could save a trace but could not attach to the
  restricted Wine/Rosetta process while System Integrity Protection was
  enabled.  Full-Wine ASan/TSan instrumentation would also replace or omit the
  prebuilt D3DMetal/GPTK components.  The focused sanitized scheduler tests,
  native runtime stress, bounded cleanup waits, and independent lifetime
  review are the strongest non-destructive substitutes; external-framework
  heap internals remain the residual limitation.

Physical display unplug/replug, a real sleep/wake cycle, and input-to-photon
photodiode capture were not automated on the single-display test host.  Their
state machines and generation resets are covered by focused tests, but the
hardware transitions remain explicit residual validation work rather than an
unsubstantiated compatibility claim.  No `docs/compatibility.md` entry is made
because no shipping game or CEF application was manually certified.

## Threat-oriented review

No security scanning skill or security-specialist agent was used.  The final
changes were reviewed directly at their trust and resource boundaries:

- A renderer-controlled signed WGL interval is converted without negating
  `INT_MIN`; multiplication saturates, and a single wait is capped at one
  second.  The registry rate accepts only a complete decimal value in the
  existing 0..1000 range.
- Mach conversions use 128-bit intermediate arithmetic and saturating add and
  multiply operations.  Randomized and boundary tests verified that no
  untrusted interval reaches a zero divisor, wrapped deadline, or unbounded
  sleep.
- Display state uses 32 fixed process-lifetime slots.  Window/display arrays
  are fixed-size, the callback dispatch source is pre-created and coalescing,
  and no callback-driven cache or queue can grow without bound.
- The realtime callback performs only bounded arithmetic, atomic publication,
  and a dispatch-source merge.  It performs no allocation, logging, AppKit
  call, Objective-C retain/release, or target-lock acquisition.
- Slot identity, generation, display ID, and period are revalidated by readers.
  Unregister quiesces Core Video before publishing the dead slot, preventing
  stale-pointer reuse and callback UAF.  Dispatch cancellation retains the
  owner/context until queued main-thread work drains.
- Foreign targets retain their backing and layer through present.  The only
  nested target order is `pacing_mutex -> present_mutex`; the present mutex is
  never held across the Mach wait, and registry ownership is not nested with
  either target mutex.  This rules out the new lock-order inversion and keeps
  detach/resize able to make progress.
- Hidden or minimized content retains only its last valid IOSurface, skips the
  copy/publish work, and is bounded to 4 Hz.  No busy spin, producer starvation,
  per-frame allocation, or release logging was found in that path.
- DXGI waitable handles, queue tokens, fences, and back-pressure signals remain
  owned by the existing D3D11/D3D12 backends.  The scheduler neither consumes
  nor fabricates a Windows signal, so it does not widen a cross-process handle
  or permission boundary.
- No new filesystem path, network input, entitlement, private Apple API, or
  elevated operation is introduced.  Errors in Core Video creation/start and
  display reconfiguration leave windows on AppKit autodisplay instead of
  silently disabling redraw.

An independent general reviewer inspected the final scheduler diff read-only
for timing overflow, callback reentrancy, slot reuse, lock/lifetime ordering,
foreign-surface state, and DXGI/D3D12 semantics and reported no confirmed code
defect.  The reviewer did identify the concurrently advanced `main` as a
process blocker; the branch was rebased before the full build and all evidence
recorded above.
