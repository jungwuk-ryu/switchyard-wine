# ARM64EC transition diagnostics

## Status and scope

This guide describes the opt-in diagnostic surface for the native ARM64
runtime's x64-on-ARM64EC provider (`xtajit64`).  It is intended for engineers
debugging a specific reproduction involving a PE/Unix provider boundary,
ARM64EC transition, `CONTEXT` transfer, control stack, or x18/TEB identity.

The feature is deliberately not a general tracing framework and it is not a
hot-attach debugger.  It is compiled into both sides of the provider ABI and
is sampled when the Wine process initializes.  A running process must be
restarted after enabling it.

This change is a diagnostic aid, not a compatibility claim.  It is kept in a
separate reviewable commit because the active provider ABI can evolve while an
application investigation is in progress.  Reconcile it with the current PE
and Unix provider contract before merging or backporting it; do not copy the
state layout or ABI identity mechanically.

## What it records

When enabled, the recorder keeps the most recent 64 provider-boundary events
in an in-process, fixed-size ring.  The common path does not allocate memory,
take a lock, or wait indefinitely.  Each record includes a causal boundary
identifier, binding and engine identities, mapping/context/transition
generations, guest and native execution state, stack ranges, x18/TEB evidence,
and the provider stop result.

The associated watchdogs validate:

- `CONTEXT` flags, MXCSR duplication, RIP/RSP, continuation pairs, and stale
  context publication;
- the private transition control-stack range and transition-frame depth;
- the PE x18 claim against an independently observed Unix `NtCurrentTeb()`;
- process/provider binding and terminal stop normalization.

The first violation atomically freezes the recorder.  A later safe boundary
emits one diagnostic dump containing the freeze summary, the first violation,
and committed ring events.  It begins with one of these strings:

```text
xtajit64 diagnostic recorder frozen at ...
xtajit64 diagnostic first-violation ...
xtajit64 diagnostic ring ...
```

The dump is evidence, not a replacement for the Windows exception contract.
For example, an interrupt-originated provider stop still needs a correct
exception/interrupt bridge.

## Enable it for one reproduction

Set the variable in the environment that launches the target Wine process:

```shell
WINE_XTAJIT64_DIAGNOSTICS=1 <normal Switchyard or Wine launch command>
```

The value must be exactly `1`.  It is sampled once during provider process
initialization; changing it after process launch has no effect.  Start with a
fresh process and a single, minimal reproduction.  Capture the launcher and
Wine stderr alongside the normal application log.

Leave the variable unset for ordinary testing and releases.  Disabled mode
does not bind the recorder or perform diagnostic time/register/OS sampling on
the transition path.

The records contain raw virtual addresses and thread identities because those
are needed to diagnose cross-ABI state.  Treat captured logs as engineering
artifacts: do not attach user prefixes, credentials, proprietary binaries, or
unredacted logs to a public issue or commit.

## Read a frozen dump

Use the first violation as the primary failure point.  Then read the ring in
sequence order, not in log arrival order.

| Field group | Use it to answer |
| --- | --- |
| `causal`, `binding`, `engine`, and `map` | Did both sides describe the same transition and mapping generation? |
| `ctx`, `trans`, `frame`, and `depth` | Was the `CONTEXT` copied from the correct generation and did frame ownership stay balanced? |
| `guest rip/rsp`, `native pc/sp`, and `control` | Did execution and stack ownership cross the expected boundary? |
| `x18`, `saved`, `teb`, `mode`, and `expectation` | Did the live value match the Unix-authenticated TEB claim? |
| `stop` and `detail` | Did the provider normalize the stop correctly?  For an interrupt hook, `detail0` carries the exact interrupt number. |

`lost`, `torn`, `contention-loss`, and `scratch-loss` mean the snapshot is
incomplete.  They do not make the first violation invalid, but they prohibit
claims about an absent event.  `unknown` is an explicit unavailable value, not
a zero value.  In particular, PE custom-x18 mode may be `unknown` where no
supported PE query exists; the numeric x18 check remains independent.

## Recommended investigation loop

1. Reproduce once with diagnostics disabled to establish the normal symptom.
2. Reproduce once with `WINE_XTAJIT64_DIAGNOSTICS=1` and save the complete
   stderr output.
3. Locate the first `recorder frozen` line and its `first-violation` record.
4. Correlate all records with the same causal boundary and verify each
   generation transition before changing provider behavior.
5. Make the smallest semantic fix, add a regression at the provider boundary,
   and repeat both the disabled and enabled reproductions.
6. If the issue is translated-block invalidation rather than a transition
   invariant, use or add translated-block provenance instead of forcing this
   recorder to explain it.

The recorder is especially useful for non-local re-entry, stale `CONTEXT`
publication, x18/TEB disagreements, invalid control-stack ownership, and
unsupported provider stops.  It is not the right first tool for broad
performance profiling, graphics lifetime bugs, or translated-block eviction
storms.

## Validate a change

Run source verification and the provider fixtures from a configured native
ARM64 build.  The concurrency script requires built PE and Unix `xtajit64`
providers plus a verified Unicorn development tree.

```shell
./switchyard/verify_source.sh
./switchyard/tests/native_cpu_provider_test.sh
./switchyard/tests/native_i386_ui_acceptance_test.sh

XTAJIT64_EXPECTED_SOURCE_DIR="$PWD" ITERATIONS=25 \
  ./dlls/xtajit64/provider_tests/run_unixlib_concurrency.sh <configured-build-dir>

XTAJIT64_EXPECTED_SOURCE_DIR="$PWD" SANITIZE=undefined \
  ./dlls/xtajit64/provider_tests/run_unixlib_concurrency.sh <configured-build-dir>
```

After an ABI-affecting port, also build the combined runtime, validate both PE
and Unix provider identities, package it, and run the target reproduction.
Fixture success alone is not a runtime acceptance result.

## Extend it without weakening the path

Use this feature as a reusable diagnostic contract rather than adding ad-hoc
`fprintf()` calls to a live reproduction.  New events must preserve these
properties:

- Keep the recorder opt-in and allocation-free on the transition path.
- Version the event schema for any wire-layout change and update PE and Unix
  users together.
- Record provenance and generation changes before state is discarded.
- Never validate x18 by comparing it only with a fresh PE `NtCurrentTeb()`
  read; that can be tautological on ARM64EC.
- Model loss and unavailable observations explicitly; do not fabricate an
  event or treat a sentinel as valid evidence.
- Add a deterministic provider-boundary test before relying on a new field in
  a real application investigation.

Future diagnostic facilities should remain similarly narrow and evidence-led.
For translated-block coherence, prioritize a separate history/provenance
recorder; for software interrupts and exception dispatch, prioritize an
exception-path recorder.  Keeping these concerns separate prevents a useful
transition recorder from becoming an unsafe all-purpose tracing subsystem.
