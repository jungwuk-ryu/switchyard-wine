# Switchyard FEX runtime

This directory owns the stable C boundary between native ARM64 Wine and the
Switchyard macOS port of FEXCore. It is a private downstream integration, not
an upstream FEX contribution.

The former Unicorn/TCG provider is retired as a product architecture. It is
not a performance fallback for this runtime. Historical contract-20 inputs are
retained only as compact forensic evidence.

## Current boundary

`include/switchyard_fex.h` exposes opaque process and thread objects, complete
x86-64 architectural state transfer, explicit code invalidation, bounded stop
records, and deterministic teardown. The provider dylib exports only that C
ABI. FEX C++ symbols and implementation types are hidden.

The v6 boundary returns cleanly for ARM64EC code transitions, `SYSCALL`,
`INT 2E`, cooperative suspension, invalid instructions, single-step traps, and
an optional designated `HLT`. The core translates guest addresses below 4 GiB
to Wine's fixed high shadow without changing identity addresses above that limit.
The adapter can reconstruct complete x86 state and canonical fault addresses
from an original Darwin ARM64 context after a generated-code fault. Guest
instruction-fetch permissions are checked by the embedding's executable-range
callback before decoding, and failures produce ordinary guest execution stops.
It also owns a `PROT_NONE` host-page guard immediately after the
shadow, so an access crossing the 4-GiB boundary fails closed without adding a
branch to every generated memory operation.

Low-shadow configuration accepts identity mode (both fields zero) or exactly
the Windows4-GiB low domain with a valid power-of-two host offset. The generated
CLZ/bit-5 predicate is specialized for that domain; other sizes are rejected
before process allocation, not silently translated with different geometry.
Both the public C gate and the private core setter enforce it. Positive identity
and4-GiB execution and every other power-of-two limit are covered by the tests.

The 25-symbol boundary also repairs two provider implementation faults before
Wine abandons the active native call: dynamically unaligned Arm TSO accesses
and overflow/underflow of FEX's non-architectural call/return prediction cache.
Both require an authenticated current-thread JIT context. Predictor recovery
additionally checks the exact guard address, access direction, instruction,
and saved predictor register; it never substitutes for guest exception handling.

The non-consuming signal-time stack query reads architectural RSP through
FEX's configured static-register map. Wine places its exception frame below
that stack, not the provider's control stack; the PE dispatcher then consumes
the original JIT context. Non-JIT contexts are never reconstructed by guessing
from a nearby guest RIP: abandoning decoder frames leaks their C++ resources.

Register the executable-range callback once before creating threads. Its
context is borrowed for process lifetime and may be called concurrently by
compiling threads. Missing, failed, overflowing, or malformed answers deny
decode. Wine uses its existing committed mapping/permission registry and
mutation/invalidation protocol. This adds a cached compile-time range lookup,
not a check on each execution of translated code. Permission changes must
invalidate both linked code and the decoder's range cache before resuming.

The C test covers low and identity code/data/stack access, atomics, SIMD,
strings, non-temporal and cache operations, invalidation, eight-thread first
use, complete fault-state reconstruction, MAP_JIT permission recovery, both
directions of a live EC-bitmap mutation, exact syscall and exception state, and
cross-thread suspension of a running backward edge, dynamic TSO repair at all
scalar widths/alignments, concurrent backpatching, and 280,000 external-call /
unmatched-return iterations across both prediction-cache guards.

The experimental Wine Unix provider is integrated with the existing ARM64EC
transition wrapper, memory/code observers, and the ntdll-owned v3 signal
observer. Fhourstones, SciMark2-C, MiBench bitcount, and Dijkstra have produced
their exact reference outputs. These are correctness bring-up results, not a
performance score or release approval. An ordinary AMD64 PE VEH regression
checks low/high read/write faults, architectural state and continuation,
no-access/read-write execute denial, and execute permission grant/revocation.
Broader SEH/unwind/debugger and real-thread failure paths, full-core sanitizer
validation, and complete source-matched Wine packaging remain open. A clean FEX
SDK alone does not establish the full runtime's compatibility or performance.

## Experimental generated-code register ABI

`SWITCHYARD_FEX_ARM64EC_REGISTER_ABI=ON` selects the existing ARM64EC guest
register placement in the downstream Darwin core. It is off by default and
requires the matching private core changes. It does not enable Windows host
code, custom-x18 JIT execution or direct native calls; those need a separate
entry/reentry, suspension and host-callback implementation. All existing Wine
state-transfer and executable-code checks still apply. Do not promote this
build or infer a speedup merely because its register layout matches ARM64EC.

Signal recovery obtains the predictor-stack register from the emitter's
configuration, and authenticates that same register in the faulting pair
instruction. The C-ABI tests independently decode the instruction's base
register and check complete architectural register preservation across JIT
execution, so the recovery test is not tied to either x25 or x17.

## Darwin x18 ownership and signal tests

The Darwin C API, including callbacks into it, requires system x18 mode.
Where the SDK and running OS provide the mode APIs, their addresses are bound
at process initialization. A JIT return still in custom mode is normalized to
system mode before error reporting and teardown, and returns `ERROR_INTERNAL`.
The adapter never restores or compares a saved system x18 value: macOS owns
that value and may change it across mode switches or signal delivery.

On macOS 26.4+ build targets, `switchyard-fex-darwin-abi-test` also supports
`--wine-signal`, `--wine-signal-public`, and `--omit-final-system-transition`.
The signal tests compile the actual Wine mode/signal wrapper, inject bounded
breakpoints into owned JIT/dispatcher instructions, restore each instruction,
and resume at the same PC. This covers standalone signal resumption, not Wine
server suspension, SIGUSR2, SEH, nonlocal exits or native PE entry/reentry.
The opt-in custom-dispatch configuration requires the supported Darwin mode API;
unsupported systems reject it rather than silently selecting another backend.

The test requires a matching Wine source checkout; set
`SWITCHYARD_FEX_WINE_SOURCE_DIR` when the adapter is copied elsewhere. Source
snapshots must retain `dlls/ntdll/unix/signal_arm64.c`, `include/wine/asm.h`,
and their LGPL notices. CMake regenerates the policy include when its source
changes and rejects changed extraction anchors. Build the explicit adapter/test
targets and run CTest in `<build>/SwitchyardFEXAdapter`; the core-only upstream
default `all` target still includes an unused Linux-linked shared-core target.

## Opt-in detached-dispatch boundary

The downstream core now provides a typed generated entry for a caller already
in custom-x18 mode. It returns in that same mode, while real Mach-O compiler and
helper calls still use the established system-mode gates. Its immutable entry
is a dispatcher, never a cached translated-block continuation. A per-activation
16-byte return-mode header avoids shared-thread return-mode state; it is not
emitted for the normal system-only core. ABI4 adds a40-byte descriptor and two
system-ABI calls, prepare and complete; the architectural state layout is unchanged.

The adapter's ordinary execution and detached entry share acquisition
and completion. An attached thread pins lifetime, and a generation-owned POD
activation is consumed once. Completion rejects old/wrong-thread/already-consumed
tokens, including after nonlocal fault reconstruction. Mutation stays excluded
until the generated invocation has fully returned and accounting is released.
Completion normalizes a leaked custom mode before any C++ TLS access.

`--detached-dispatch` exercises a naked trampoline with a guarded private stack,
exact state/SP/x18, warm zero-internal-toggle entry, cold compilation, actual
host calls, single-step, doorbell stop, invalidation and same-stack nested reuse.
Its normal entry/return traps use the actual Wine signal-mode wrapper. Its
nonlocal memory-fault case is adapter-only: the standalone shim has no Wine
TEB/syscall frame and deliberately rejects Wine lost-x18 fault recovery.
`--detached-ordinary-entry` is a negative control that must exit96 at the
double-mode-transition assertion.

Wine's initial ABI16 connection and the adapter share one generation-owned admission
cell (see below). `WINE_FEX_CUSTOM_DISPATCH=1` selects
the assembly PE caller; the normal system-mode FEX path remains the control.
Completion precedes native guest execution; syscall and internal-suspend exits
prepare a fresh descriptor under the current mapping generation. Fault
reconstruction consumes the same lease. The512-byte Unix call record retains
the original472-byte begin structure and leaves naked BeginSimulation unchanged.
The PE entry's expected x18 comes only from Wine's Unix-authenticated TEB.

The exception-stack query accepts proven JIT contexts on either the Unix stack
or the detached control stack; it still rejects checked-copy recovery, unrelated
host PCs and invalid guest stacks. `--public-detached-dispatch` covers the public
configuration, stale/foreign tokens, mutation exclusion and guarded256-KiB stack
reuse. These tests do not prove general SEH/unwind or debugger support.

The Wine connection still uses prepare/complete Unix calls. Ordinary state
transfer uses the bounded register window below; syscall and fault continuation
retain complete state. This does not remove the native-call overhead. Whole-program
performance, broader suspension/SEH and reproducible packaging remain required;
unit tests and individual PE checks do not authorize a runtime promotion.

## Shared execution admission

ABI5 adds a versioned32-byte external-domain descriptor and returns a borrowed
pointer to the adapter-owned8-byte admission cell at thread creation. Internal
and external ownership cannot be mixed within a process. Standalone users retain
internal close-all/reopen; Wine owns closure under its existing mutation mutex.
The adapter checks that every external cell is closed and idle before mutation,
and never reopens the embedding's gate. No new PE pointer or Unix opcode is exposed.

Wine reserves ordinary execution under that mutex using prepare-execution, then
unlocks before execute-prepared. Detached preparation uses the same ordering.
The ACTIVE/EXECUTING/generation bits have one atomic modification order; a
completion preserves CLOSED and invokes the embedding's wake only if it consumed
a closed activation. Short state transfers never wake or recursively lock Wine.
The system-ABI wake callback must not throw or reenter the adapter, and its
context must outlive thread destruction and the final bridge access.

## Borrowed native register window

ABI6 adds a40-byte descriptor for a normalized408-byte x64 register window.
Wine borrows its existing context rather than materializing an872-byte complete
state record on every native entry and return. Byte-buffer alignment may be one;
fixed-width memcpy accesses avoid type aliasing. Version, flags, exact payload
size, address overflow and known private aliases are checked before state access.
The caller pins the buffer, owns it exclusively until return, and must supply
accessible memory; geometry validation is not an arbitrary-pointer probe. Neither
pointer is retained, and the same admission cell serializes all state APIs.

Import preserves the former native-entry contract: GPR/RIP/flags/MXCSR/XMM are
installed, x87 is reset to default/empty, segment selectors and bases are reset
except CS and GS, and absent YMM high halves are preserved. Export initializes
all408 bytes and leaves them unchanged on failure. Generic vector reconstruction
uses the core's complete representation, not a null high-half shortcut.

This is not a full architectural snapshot. Internal syscall reentry and fault
reconstruction must continue to use the complete-state API or guest x87, segment
and YMM state would be lost. The provider checks every real Wine field offset;
the actual-core differential test checks264 whole-state/output cases against an
independent pre-window conversion oracle, and the public C gate covers malformed
descriptors, unaligned buffers, error publication and eight-thread contention.

An early wake may let mutation finish before the executor resumes Wine's stop
classification. Pause ownership is therefore recorded against the execution
generation, not the transient doorbell-ownership boolean. A late completion
cannot leak an internal suspension to the guest or consume a newer activation.
Wine closes an idle cell and destroys its thread under the membership mutex
before removing the binding; process teardown cannot overtake the last detach.
Failed destruction stays pinned and poisons the provider rather than freeing
live callback storage. Arbitrary concurrent destruction of opaque API objects
is still outside the C contract.

`--shared-admission` checks malformed domains, ownership mode, external closure,
reserved execution, exact wake counts, stale/foreign generations and teardown.
The actual Wine-provider test forces mutation to finish before completion and
checks replay followed by a genuine external suspension. This shared-cell step
does not itself remove the ordinary Unix call or detached prepare/complete calls.

## Wine control-stack ownership

The current development PE/Unix handshake is ABI19 (104 bytes); the FEX C ABI
remains5. ABI17 added the opt-in `WINE_FEX_DIRECT_DISPATCH=1` native boundary;
the ordinary mode returns its complete native activation before a guest native
callback. It is not a general Unix-call replacement. Detached/custom dispatch
remains a separate opt-in, and neither mode is a release default.

ABI18 additionally returns Unix-authenticated native page size. Windows
SystemBasicInformation.PageSize remains the logical mapping/EC-bitmap unit and
must not define a native guard. The provider's existing256-KiB allocation now
contains one native state page, one inaccessible native page and the descending
control stack; an enabled recorder stays at the exact aligned high end.
All three mapping permission runs are published before attachment. Partial
publication or failed detach retains owned storage and fails closed.

ABI19 adds native guard initialization for this unpublished allocation. The PE
caller keeps its tracked allocation and release paths. Unix authenticates the
complete private, uniformly RW identity view, uses its validated native page
size and calls Wine's normal protection path without holding the provider mutex.
All three final permission runs must be published before ThreadInit succeeds.
This leaves exactly one owned PE allocation for ntdll's existing acknowledgement;
its sequence, generation and active-mutation checks are unchanged. Intervening,
failed or in-flight mutations still require ordinary resync. Equal final mappings
do not prove unchanged code content and must not bypass cache invalidation.

If a committed view disappears between the snapshot collector's two queries,
discard the partial snapshot and restart within the existing finite budget;
do not treat that invalidated observation as a permanent provider failure or
commit earlier runs. Allocation/malformed-range errors still fail directly.
See the [standalone boundary fixtures](../../dlls/xtajit64/provider_tests/native_boundary_pe.md).
Guard faults and bounded cold-JIT watermarks do not prove arbitrary compiler
stack depth, stack-clash handling, or every asynchronous failure path.

## Reproducible source

`source-deps.tsv` pins the exact upstream FEX revision and the four submodules
needed by the core-only build. `../patches/fex-73dc3b3-darwin-core.patch` is a
zero-context patch so the repository itself has no whitespace-only patch-file
lines; apply it only with `git apply --unidiff-zero` and only after verifying
the pinned clean revision and patch SHA-256.

`../build_fex_runtime.sh` contract5 selects the EC register layout explicitly,
pins compiler/SDK/CMake identity, and snapshots the entire adapter/test input
list plus the real Wine signal source, assembly header and LGPL notices. It
requires the exact13-test CTest plan with no skipped test, followed by25 direct
C-ABI repetitions by default. Installed corresponding source is the same snapshot
that was compiled. Signed ABI5 output and the complete SDK are hash-pinned, and
publication is exclusive; existing outputs cannot be replaced or nested into.
The source option's generic/OFF default is unchanged for separate ABI controls.

This development contract includes the held state-export initialization change;
its known Fhourstones regression still prevents performance/release promotion.
The full Wine builder's native profile must not be used until its remaining
Unicorn dependencies are replaced by the FEX closure. No TCG fallback is permitted.

Redistribution must preserve the FEX/FEXCore MIT notices, the licenses for fmt,
range-v3, unordered_dense, and xxHash, the bundled Cephes notice, and the
Berkeley SoftFloat license text carried by its source files.
