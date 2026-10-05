# Public Windows exception and suspension regressions

These standalone fixtures exercise public Windows APIs on the actual runtime.
They are not performance measurements or a substitute for application gates.

`crt_boundary_pe.c` and `crt_boundary_abi.S` are a separate, opt-in boundary
cost discriminator, not an application or translator score. Build the same
optimized x64 PE for both runtimes, with `-fno-builtin`, compiler SEH and the
real Win64 ABI. Do not link a CRT startup or change the runtime's DLL policy:
the fixture resolves UCRT `memcpy`, `memset` and `memcmp` through their exact
prototypes. Its internal kernels have equivalent memory contracts, not identical
implementations. In particular the internal bytewise comparison can be slower
than the native vectorized implementation at large lengths.

The untimed oracle covers 900 disjoint-memory/sign/canary cases, 24 nonvolatile
GPR/XMM/MXCSR-control/x87-control calls, 24 end-of-readable-region cases and six
checked read/write access violations. Guard geometry comes from Windows system
information, not a hard-coded host page size. Deliberate access faults are outside
the assembly ABI wrapper; its unwind metadata is present, but asynchronous faults
inside that wrapper are not claimed. Define `BOUNDARY_NEGATIVE_CANARY` or
`BOUNDARY_NEGATIVE_ABI` in separate binaries: they must exit 21 or 23 before any
phase record, not pass the positive reference. These are fixture-oracle controls,
not runtime mutants.

```sh
(
    cd "$wine_build"
    tools/winegcc/winegcc -o "$out/crt-boundary.exe" --wine-objdir . \
        --cc-cmd="$clang" -b x86_64-windows -nostdlib -nodefaultlibs \
        -Wl,--entry,mainCRTStartup -Wl,--file-alignment,4096 \
        -Wl,--section-alignment,4096 -I"$repo/include" \
        -I"$repo/include/msvcrt" -Iinclude -O2 -g -Wall -Wextra -Werror \
        -D__WINE_PE_BUILD -DUSE_COMPILER_EXCEPTIONS -fno-builtin \
        -fms-extensions -fexceptions -fasync-exceptions -fno-stack-protector \
        "$repo/dlls/xtajit64/provider_tests/crt_boundary_pe.c" \
        "$repo/dlls/xtajit64/provider_tests/crt_boundary_abi.S" \
        -lkernel32 -lntdll
)
```

Set the fixture-only `SWITCHYARD_BOUNDARY_REVERSE` to exactly `0` or `1` to
balance internal/imported order. Normal stdout must match its reference; stderr
contains exactly three target RVAs and 24 QPC phase records. Authenticate the
resolved targets against the exact loader thread, exported PE metadata and native
code ranges in a separate structural preflight. This is installed-image evidence,
not a live-byte/hotpatch-generation certificate. Run at least five interleaved
native/Rosetta pairs without profiling or runtime diagnostics, keep all raw
samples and dispersion, and distinguish the complete imported path from pure
transition latency. Loop overhead and different function bodies are included.
Never use these results alone to replace CRT exports or promote a runtime.

- `suspend_context_pe.c`: sixteen nested suspend/resume cycles; GPR, XMM and
  MXCSR edits; native-call thread identity; actual x64 virtual unwind metadata.
- `seh_unwind_pe.c`: Clang-generated x64 SEH; software, hardware and nested VEH
  exceptions; 108 ordered `__finally` scopes with live stack canaries.
- `seh_unwind_native_pe.c`: native ARM64EC control; software and hardware
  exceptions; 72 ordered scopes. Link with Wine's normal CHPE/load-config producer.
- `control_stack_pe.c`: inspect the real provider allocation and fault both ends
  of its inaccessible native page on nine threads. Enable and disable the flight
  recorder; Windows logical PageSize is not the native protection granularity.
- `control_stack_stress_pe.c`: intrusive ARM64EC-only fixture on its own idle
  control stack. Cold scalar/branch/SIMD blocks and CPUID callbacks run in three
  rounds. A watermark reports touched bytes, not an absolute worst-case SP bound
  or proof against a large frame skipping a guard. Never run this inside an app.
- `thread_lifecycle_pe.c`: same source for native ARM64EC and x64; 32 serial plus
  four eight-thread batches, with checked exits/joins. `THREAD_LIFECYCLE_LONG`
  selects 512 serial plus 64 batches and the separate long reference output.
  Timings are post-main QPC wall intervals on stderr, not guest CPU time.
- `../../ntdll/tests/arm64ec_guest_flags_pe.c`: all eight edited PF/AF/DF
  combinations through authenticated low/high write-fault VEH continuations.
  Exact RIP, registers, access kind, owner thread and single-fault checks prevent
  a skipped or unrelated fault from passing. CLD restores the native C direction
  contract after capturing the returned flags. This is an x64 PE fixture; use
  its companion reference including the runner's final `exit 0` line. It is
  separate from the native converter/metadata test in `arm64ec_guest_flags.c`.

Use optimized builds with warnings as errors. Given an absolute repository path
in `repo`, a configured multiarch Wine build in `wine_build`, a fresh output
directory in `out`, and explicit `clang`, `mingw_cc` and `mingw_sysroot` paths:

```sh
"$mingw_cc" -O2 -g -std=c17 -Wall -Wextra -Werror -Wconversion -Wshadow \
    -static -static-libgcc -Wl,--no-insert-timestamp,--nxcompat,--dynamicbase \
    "$repo/dlls/ntdll/tests/arm64ec_guest_flags_pe.c" \
    -o "$out/arm64ec-guest-flags.exe"
"$mingw_cc" -O2 -g -Wall -Wextra -Werror -static -static-libgcc \
    -Wl,--no-insert-timestamp \
    "$repo/dlls/xtajit64/provider_tests/suspend_context_pe.c" \
    -lntdll -o "$out/suspend-context.exe"
"$clang" --target=x86_64-w64-windows-gnu --sysroot="$mingw_sysroot" \
    -O2 -g -fms-extensions -fexceptions -Wall -Wextra -Werror \
    -c "$repo/dlls/xtajit64/provider_tests/seh_unwind_pe.c" -o "$out/seh-unwind.o"
"$mingw_cc" -static -static-libgcc -Wl,--no-insert-timestamp \
    "$out/seh-unwind.o" -o "$out/seh-unwind.exe"
(
    cd "$wine_build"
    tools/winegcc/winegcc -o "$out/seh-native.exe" --wine-objdir . \
        --cc-cmd="$clang" -b arm64ec-windows -nostdlib -nodefaultlibs \
        -Wl,--entry,mainCRTStartup -Wl,--file-alignment,4096 \
        -Wl,--section-alignment,65536 -I"$repo/include" \
        -I"$repo/include/msvcrt" -Iinclude -O2 -g -Wall -Wextra -Werror \
        -D__WINE_PE_BUILD -fms-extensions -fexceptions -fasync-exceptions \
        "$repo/dlls/xtajit64/provider_tests/seh_unwind_native_pe.c" \
        -lwinecrt0 -lkernel32 -lntdll
)

# These additional native fixtures require the internal SYSTEM_BASIC_INFORMATION
# layout. Keep the normal Wine CHPE/load-config producer.
for fixture in control_stack_pe control_stack_stress_pe thread_lifecycle_pe; do
    (
        cd "$wine_build"
        tools/winegcc/winegcc -o "$out/$fixture.exe" --wine-objdir . \
            --cc-cmd="$clang" -b arm64ec-windows -nostdlib -nodefaultlibs \
            -Wl,--entry,mainCRTStartup -Wl,--file-alignment,4096 \
            -Wl,--section-alignment,65536 -I"$repo/include" \
            -I"$repo/include/msvcrt" -Iinclude -O2 -g -Wall -Wextra -Werror \
            -D__WINE_PE_BUILD -D__WINESRC__ \
            -fms-extensions -fexceptions -fasync-exceptions \
            "$repo/dlls/xtajit64/provider_tests/$fixture.c" \
            -lwinecrt0 -lkernel32 -lntdll
    )
done
"$mingw_cc" -O2 -g -Wall -Wextra -Werror -nostdlib \
    -Wl,--entry,mainCRTStartup -Wl,--no-insert-timestamp \
    "$repo/dlls/xtajit64/provider_tests/thread_lifecycle_pe.c" \
    -lkernel32 -o "$out/thread-lifecycle-x64.exe"
```

The ntdll companion builds a separate ARM64EC PE using the actual dispatcher's
assembly and `virtual_unwind` selector from `signal_arm64ec.c`:

```sh
python3 -B "$repo/dlls/ntdll/tests/build_arm64ec_exception_unwind.py" \
    --wine-build "$wine_build" --clang "$clang" --output "$out/phase"
```

It checks every dispatcher prologue/preparation instruction, an unwound return
from preparation, and the first prepared instruction as a fault or call return.
`--negative-old-context` and `--negative-return-pc`, each with a new output
directory, must exit 12 rather than pass. They distinguish the context-format
contract from the call-site lookup contract. The real unwind implementation is
the selected runtime's ntdll; the two extracted functions are not reimplemented.
This does not inject faults inside the preparation helper or prove all async cases.

Pin compiler, source, PE and complete runtime hashes. Check byte-exact stdout
against each `.reference_output` (which includes the harness's `exit 0` line).
Run in isolated fresh prefixes with no overlapping Wine processes and bounded
timeouts; test ordinary and ordinary-direct modes on the identical package.
Keep error channels observable, distinguish helper/driver failures from fixture
correctness, and remove only the run-owned prefix/processes. Never substitute a
raw ARM64EC link lacking CHPE metadata or suppress a failed cleanup scope.

`check_control_stack.py` compiles the actual PE allocation/layout/publication,
Unix guard initialization and ntdll mutation-acknowledgement functions. It covers
4/16/64-KiB pages, malformed native views, allocation/query/protection rollback,
partial-publication retention and native faults. Successful publication retains
one tracked PE allocation; ordered successful, failed and in-flight interference
must remain dirty. Negative controls remove protection, publish the wrong
permission or add a second PE mutation. `SANITIZE=undefined` enables UBSan.
`check_mapping_snapshot.py` orders unmapping between the real collector's
two queries; verifies bounded full-snapshot restart, partial-result discard,
unchanged hard errors and negative controls. These models complement the PE
fixtures; they cannot establish actual host permission behavior alone.
