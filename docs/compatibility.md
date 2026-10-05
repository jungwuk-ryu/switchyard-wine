# Application compatibility

This document records point-in-time application compatibility results for Switchyard Wine.
Each row is a self-contained verification record with its own confirmation date, runtime, host
environment, and launch or graphics path. The current entries repeat the selected runtime and
host recorded when the results were supplied, but these fields remain per-application so later
checks can update independently.

Some historical rows were supplied by the user rather than independently retested. New checks
record the application build or Steam App ID and the exercised launch path when that information
is available.

## Compatibility results

An earlier 2026-10-04 baseline used complete optimized native/FEX and Rosetta
builds from the same frozen Wine source
`6bb4505e1f8da52348b5cac9ee278f1958304667c51629a179dc24881cd24123`.
Native runtime `3bb193f63d137b492a53a0b74915a089362845265fe86f694d21b52bb22101ad`
and Rosetta runtime `b14a6a303f36f237a71ea828b1916b5bea2cb125b592615086575f2d72ad3b55`
produced 84 exact outputs across six order-balanced pairs per workload.
Paired native/Rosetta whole-main medians ranged from 1.478 to 5.451, not parity.

A 2026-10-04 CPU baseline used complete-source SDK6/PE19 native and
identical-source Rosetta builds, not the earlier component-rebuilt trials.
The bounded register window removes intermediate native-entry state
materialization while preserving the shared admission cell and complete
x87/segment/YMM state for internal syscalls and faults. A clean production SDK
rebuild reproduced its signed library and payload pins after all 15 tests,
25 C-ABI repetitions, 25 contention repetitions and 25 whole-state repetitions.
Eight optimized/provider-C-UBSan positives, four required syscall-loss mutants
and 20 exact ordinary/direct Windows boundary runs closed on the full build.

Six interleaved order-balanced pairs per workload then produced 84 exact outputs.
Paired native/Rosetta whole-main medians are Python 5.234081 (MAD 0.073201),
Bullet 2.299255 (MAD 0.005125), TSVC 1.482109 (MAD 0.005451), SciMark2 2.096792
(MAD 0.005708) and Fhourstones 1.721181 (MAD 0.010472). These remain material
performance gaps, not parity. Bitcount and Dijkstra are short discriminators,
not an aggregate score. Direct/custom opt-ins, diagnostics and profiling were
disabled; source, runtime, PE, input, tool, seed, Python and cleanup hashes
closed. No outliers were removed; guest CPU-time fields remain unvalidated and
unscored. Whole-process counters include loading and teardown, not only guest
main. Independent retained/rejected component trials are not multiplied into
these new matched ratios.

Application output correctness is not whole-runtime acceptance. The independent
winebth driver-load failure remains observable. This is CPU-only development
qualification, not graphics or release admission. Historical game/Blender rows
retain their actually tested older runtime.

A separate non-scoring UCRT architecture experiment replaced only an isolated
COW-prefix UCRT leaf with the same-source Rosetta control's x64 Wine UCRT
(`801c97d77b839b793b60c684b853eddc719e75a4bfbc4db79e9e240a59b28c10`).
Native-only policy rejects Wine builtins; builtin policy loaded a UCRT but native
helpers rejected the architecture and CPython faulted before main. Both probes
failed their output oracle and were cleaned up; no timing or default-policy
change is admitted. These deliberately altered configurations are not the
successful default configurations recorded in the table.

A standalone CRT-boundary x64 PE then passed on these same complete CPU-only
builds: two structural positive runs, four required canary/ABI negative controls
and twelve no-instrumentation positive runs in six balanced interleaved pairs.
Each positive checks 900 memory cases, 24 nonvolatile-ABI calls, 24 protected
boundaries and six access violations before timing. Imported UCRT targets were
matched to the exact fixture loader thread and installed export/code metadata;
no DLL was replaced. Small imported native calls have a roughly 163–165 ns
floor on this host, substantially more than the internal x64 kernels. The
internal bytewise large comparison is slower than the native implementation,
so this is not permission for a global CRT rewrite or a whole-application gain.
All 288 phase observations and unchanged source/runtime/SDK/seed/tool identities
were retained; the independent winebth failure remains visible.

A bounded profile-mode driver also passed sixteen positive, required-negative,
input-limit and gate-state checks on the same native/Rosetta pair. Two separate
debug-only native captures bracketed a warmed imported memcpy16 loop and an
internal x64 loop. The imported interval's 1,998 exact raw-PC samples included
14 in UCRT code/thunks, while all 1,997 internal samples mapped to validated
post-body fixture blocks. This prioritizes shared native-call/dispatch overhead;
it is not a removable-time, instruction-stall, JIT-generation or performance
score. No production runtime, floating-point contract or DLL policy changed.

A separate 2026-10-04 continuation regression on the earlier native runtime confirmed
loss of the x64 parity flag after a handled write fault. The ARM64EC context
conversion also discards auxiliary-carry and direction flags in a source-exact
test. The shared CONTROL-owned metadata correction preserves all three without
placing them in native CPSR/PSTATE. Twenty exact Windows GPR/VEH/SEH/native-call/
suspension programs pass on each corrected variant, plus four runs of the public
flag-edit regression. These bounded results do not establish complete-source,
remote-context, graphics or release acceptance; no new game result is claimed.

An earlier CPU screen identified an unpublished initial-native-entry candidate
on that unchanged complete Wine parent. Only the Unix provider, private SDK and
explicitly non-admitted manifest differ; PE19 and other Wine modules are unchanged.
The candidate combines register import and execution reservation under one
existing generation. Internal syscall and suspension continuations keep their
full state; no application-specific path or default-policy change is introduced.
Eight provider positives, four required full-state-loss negatives, ten Windows
boundaries and three CRT positive/negative preflight cases closed before timing.

Two independent order-balanced strata produced 126 exact primary outputs across
seven workloads and 18 CRT positives retaining all 432 phases. Paired
candidate/original main medians are Python 0.986043 and Bullet 0.991236 (both
lower in all six pairs); TSVC 1.000263 and SciMark2 1.000749 show no meaningful
gain. Fhourstones is 1.006185 with one adverse 1.095573 pair retained, not
excluded or attributed to a root cause. Short Bitcount/Dijkstra remain noisy
discriminators. Python compression also has a small mixed adverse signal.
Candidate/Rosetta main medians remain Python 5.206097 and Bullet 2.283547,
not parity. Whole-process counters are separate from main-phase timing.
All inputs, components, seeds, tools, Python and process-cleanup identities
closed. Preserve the small candidate, but hold default adoption: general
non-regression, sustained application, cumulative ABI/security and packaging
gates remain open. The native-cost standard control also retains a small
adverse signal. No new game, graphics or release result is claimed.

A further private owned-completion differential exports the stopped native
register window under that same execution generation, then releases and wakes
before the provider mutex or any native callback. Syscall full state, custom
dispatch, floating-point handling and PE19 remain unchanged. Sixteen exact core
tests, repeated ownership/byte differentials, eight provider positives, four
required full-state-loss negatives, ten Windows boundaries and CRT positive/
negative preflight checks closed before its 72 primary and 18 CRT runs.

That earlier screen compared this candidate against the retained private entry-only
baseline and the same-parent Rosetta control, not a new full Wine build. Small
imported memcpy16 cost falls from 167.7388 to 161.8916 ns (paired 0.961944,
lower in all six pairs); Rosetta is 3.7824 ns. Whole-main results are small and
mixed, Python compression is adverse in five pairs, and the Fhourstones
1.041709 adverse pair is retained. Preserve the small candidate but hold default
adoption; this does not establish broad non-regression or approach Rosetta parity.

A 2026-10-05 private Unix-owned native gate then removed repeated warm provider
mutex entries and a second SDK call under the same execution generation. Mapping
epochs are checked after acquire; stopped state is exported and released before
native callbacks. Full syscall/suspension state, FP/x18, PE19 and FEX core remain
unchanged. This is a component differential on the same compiled Wine parent,
not a new current-source build or a production ABI extension.

After core/concurrency/actual-JIT/provider positive and required-negative gates,
ten exact Windows boundaries and CRT preflight, six complete three-runtime
permutations produced 72 exact primary outputs and 18 CRT runs/432 phases.
Python/Bullet whole-main reductions are about6.5%/2.7%, recovering only8.160%/
4.893% of Rosetta excess. TSVC/Fhourstones remain mixed and compression has four
adverse pairs. Preserve the candidate but HOLD default. Per-run static/launch
identity does not substitute for comprehensive mapped-image authentication.

Separate actual-image-qualified same-byte SDK/FEX-Wine/Rosetta-Wine kernels,
serial Python worker counters and a current-SDK stable-interval raw-PC profile
support investigating frequent multi-ABI state/FP/dispatch alongside guest code.
These diagnostics are not exclusive removable-time fractions or performance
scores. A cleanup-only same-byte v6 fixes signal-handler lifetime; its exact
three-mode preflight does not relabel historical v5 timings. Application outputs
do not establish cumulative security/ABI/concurrency, sustained non-regression,
graphics or packaging admission; no new Heartopia or second-game result.

The current six-kernel same-byte campaign adds a checked push/pop control and
retains all raw results from six complete SDK/FEX-Wine/Rosetta-Wine orders.
Direct-call slowdown2.268517 already appears in the SDK harness; the stack-only
control does not reproduce it. A separately isolated unsafe two-poll sensitivity
trial establishes no gain and is not an application or deployment result. Actual
return-dependency/prediction/stall attribution remains unmeasured; no production
runtime change or new whole-app improvement is inferred from these diagnostics.

A subsequent 2026-10-05 isolated canonical-state view removed the intermediate
408-byte native-return transport while retaining complete FP/exception state and
same-generation ownership. Its archived campaign identified a private
ABI20/SDK6 candidate and unchanged same-parent controls; this was not a new
full-current-source Wine build. Eighteen no-op runs and 72 whole-main runs have
six complete interleaved orders, exact outputs, actual mapped Mach image
paths/architectures/hashes before timing, and clean owned-process teardown.
Main includes main-time JIT; startup/import/inventory and whole-process wall are
not scored. Python paired reduction is about1.9%, recovering only2.438% of the
same-pair Rosetta excess. LLVM results are mostly neutral/mixed and compression
has four adverse pairs. HOLD the isolated prototype; no default promotion.

Manual NEW-diff review found a pre-helper recorder-observation order regression.
A separate repaired PE has five normal and five recorder-enabled exact boundary
outputs, but no performance score; frozen original measurements are not
relabelled. A separate 24-run current-SDK target diagnostic narrows frequent
native calls in Python/Bullet. Its median counting overhead is about1.7%/1.9%;
all diagnostic timings are unscored and call shares are not CPU-time fractions.
Neither result establishes full dirty-tree security, packaging, graphics,
sustained game performance or a new Heartopia compatibility result.

A later 2026-10-05 SDK-only immutable geometry certificate retained every dynamic
state/admission/FP/exception contract. The four current whole-program rows below
record 72 balanced main runs and 18 additional Fhourstones precision runs, with
exact outputs and actual-image-qualified controls. Python/Bullet median time
reductions of2.39%/1.70% recover only about3% of Rosetta excess; TSVC is adverse
in five pairs and Fhourstones gain is not reproduced. Preserve the isolated
candidate, HOLD default promotion; no current-source/game/graphics/release claim.

| Application | Status | Last confirmed | Runtime | Host environment | Launch / graphics path |
| --- | --- | --- | --- | --- | --- |
| Switchyard no-op ARM64EC boundary fixture (x64 PE) | Partially working — historical ABI19 64 KiB-section ARM64EC DLL fails relocation writable protection (`0xc0000022`, LoadLibrary error 5); 4 KiB-section DLL works on ABI19 and private ABI20 | 2026-10-05 | Development Wine `b861d8e260f933a76c17d5482bbbe376bb4ea51e` plus dirty integration measured at `12791f112aac2d2e7778f87d013da2b6db483eaa94ebc5567ae271847ef6b84d`; complete compiled Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · native-gate baseline `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5` · same-parent Rosetta `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46` · private canonical-view candidate `783c7b925d9da9a74e6afb4a5cabf3df3dc95c403a1ecb1cadbab5b448d6d20a` / SDK `346d9b39af2aef031b645023321ecdac3fe776fc4fa83e2edcf81d524e7359b1` / FEX library `8d16136bdd3124a6d7fc532aa8f026d711cbd2e403bd6f833b7a932f393a1d79` / Unix provider `13fedd25622dda4e50132eae9e63a03f00c62ce05194e641ea2663e59b630317` / ARM64EC provider PE `a1ce0ee56dbc2b497600636666871cbd0fba619a308d2cbe892937f407d5d3a4`; unpublished ABI20 canonical-view/SDK6 experiment, NOT current full-source/production/package admission · historical 64 KiB-section failure established on ABI19 baseline `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5`, primary `945efdcccd5a3c038cff12553fdec431531bb0e3f6a4034d0a6764e433841d03`, not retested or attributed to ABI20 | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB host pages · baseline/candidate native ARM64 FEX and x86_64 host/Rosetta 2 | Identical x64 caller `0a43b3e5e76f3141d8c01fbdc32d82034bdeaaf13038bbd0c1aadf38b9a730ab`; ARM64EC DLL `0f546e8795087553b39b9db59ecdc7ab83fda93a8f5caea97b3d408fc9dbfb8a`, x64 DLL `a3568bf4f5ae0d101896629e5528c574958fe1f3cc8e5431e4bf99a32ebeeebb`; typed three-integer/64-bit-return call; current campaign18 counterless positives/126 exact phases in six complete three-runtime orders; paired canonical/gate EC no-op1.008409/MAD0.025913,4/6 adverse; absolute baseline140.256453/canonical142.097139ns; same-caller Rosetta x64-DLL1.132059ns has a different callee ISA, NOT translator/app slowdown; historical Rosetta2.747679ns not pooled; three preflight positives/two required ABI/value negatives; live FFS/body/EC-map and installed PE hashes plus actual Mach paths/architectures/hashes, not full relocated PE memory hashes; fresh COW prefixes/clean bounded teardown; historical 64 KiB failure before timing, no score or protection bypass; CPU-only, HOLD, no default/graphics admission |
| Switchyard same-byte kernel fixture (x64 PE) | Working — 2.27× direct-call kernel slowdown versus Rosetta | 2026-10-05 | Development Wine `b861d8e260f933a76c17d5482bbbe376bb4ea51e` plus dirty integration measured at `217371d9ed160572b07dfff1ebe396136f8340f0ef3ee600e88724f7828ed39e`; complete compiled Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · native-gate baseline `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5` / Unix provider `20cbe32f4322973e661e37aa10bfeed04596d006a4fcca3e16f7cdce113f8211` / ARM64EC provider PE `1b8a38fbcc22aa3aed59fca79a62e01dff0fe8948eb8ea137adab1b9be94cc17` / SDK `90d7203549cd9b15472440cc22d6233e6694e97dc3e53e9a752f28bdba035f03` / FEX library `dcbf5a6349939dfa43abd692ba1615e0396dd4364d2d6fde10df5fd518b1fd99`; FEX upstream `73dc3b3eddaf72745c743fe8ec76ed66f87636dc` plus 44 unchanged retained Darwin core paths · same-parent Rosetta runtime `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46`; private ABI19/native-gate SDK6, NOT current full-source/production/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB host pages · native ARM64 FEX and x86_64 host/Rosetta 2 · CPU-only, no D3D workload | One identical 1536-byte relocation-free x86 payload `f636037ed5e7ca9652441b41c7ebf6a4ff7010beb95c9dd653a2d2c27f6d1cbc`; identical x64 PE `d6b88f4a4047fad2ae62cf28a7a783e3e854f3b98a83ea4518fade950254c24d` and native ARM64 harness `8e51b763902d3b50fcba76086d05eaa04455b025b9e6edc6053c16a70d0c3bc2`; register/scalar/SSE2/direct/indirect/push-pop kernels, independent bytes/data/stack/zero-one oracles; 18 runs/324 exact phases in six complete SDK/FEX-Wine/Rosetta-Wine orders; actual mapped Mach paths/architectures/hashes and logical PE/installed backing machine+hash before clocks, not complete relocated PE memory hashes; rewritten8664 headers do not establish physical EC classification; explicit optional MSVCRT absence with required UCRT; paired warm-long FEX/Rosetta median(MAD): register0.982894(0.006569), scalar1.534668(0.002918), SSE2memory1.361219(0.017304), direct2.268517(0.022490), indirect0.911615(0.002083), stack0.600545(0.002199); direct SDK/Rosetta2.278625 and FEX/SDK0.989585 are kernel contrasts, not exclusive app-time buckets; first entry is not cold OS/Rosetta cache, persistent FEX cache off; owned COW prefixes/clean teardown and borrowed signal-handler retirement; historical v5/v6 scores not relabelled; separate unsafe SDK-only polling sensitivity is not this compatibility/performance score; no new game/graphics/default/release admission |
| Switchyard CRT-boundary fixture (x64 PE) | Working — elevated small imported-call cost on native ARM64 FEX | 2026-10-05 | Development Wine `b861d8e260f933a76c17d5482bbbe376bb4ea51e` plus dirty integration measured at `e0d92604446eabaae20561db0454cf96716e4a9fd769445cead92880ab442e4e`; complete compiled Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · retained entry+completion baseline `36385455cd5272b3c0d508d0be86d4786a6088250895e258a6c75dddc19fc55e` · same-parent Rosetta runtime `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46` · private Unix-owned native-gate candidate `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5` / Unix `20cbe32f4322973e661e37aa10bfeed04596d006a4fcca3e16f7cdce113f8211` / SDK `90d7203549cd9b15472440cc22d6233e6694e97dc3e53e9a752f28bdba035f03` / library `dcbf5a6349939dfa43abd692ba1615e0396dd4364d2d6fde10df5fd518b1fd99`; ABI19/6 with unpublished gate extensions, NOT current full-source/production SDK/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB pages · baseline/candidate native ARM64 FEX and x86_64 host/Rosetta 2 | Optimized CRT-free x64 PE, typed UCRT targets and internal kernels; 18 timed positives in six three-runtime permutations; structural positive/canary21/ABI23 negatives; 900 memory/24 nonvolatile-ABI/24 guard/6 AV checks per positive and all432 phases retained; memcpy16 baseline160.3654/candidate144.8746/Rosetta3.9314 ns, paired0.895368/MAD0.003423, 6/6 lower; identical optimized Windows x64 PE/input, exact outputs and fresh COW prefixes; six complete baseline/candidate/Rosetta permutations; static/launch/runtime/input/seed/process closure, not per-run vmmap; ordinary dispatch, instrumentation/direct/custom unset; independent winebth driver_load retained; CPU-only, HOLD default, no graphics/release admission |
| LLVM test-suite Bullet (x64 PE) | Working — 2.12× whole-main slowdown versus Rosetta | 2026-10-05 | Wine HEAD `b861d8e260f933a76c17d5482bbbe376bb4ea51e`, measured dirty `bdc894daa950af7481eda6734131711bad658831f82ed156216f4ffe7434dfcd`; compiled Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · SDK-only immutable-geometry candidate `6131b529987978c4dba6de0560a6cb9ca8f67faf58ca2409690e506b861a6596` / SDK `103ad74d2912797d393a680723a234eaf670c674b57a2fb956e795d872df07af` / loaded FEX library `3f66f57d8435a1aae8516e01550907e5bab84f55c6ce8d6dd3116cafcede871d` / unchanged Unix provider `20cbe32f4322973e661e37aa10bfeed04596d006a4fcca3e16f7cdce113f8211` / ARM64EC PE19 `1b8a38fbcc22aa3aed59fca79a62e01dff0fe8948eb8ea137adab1b9be94cc17` · native baseline `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5` · Rosetta control `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46`; not full-current-source/production/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB host pages · native ARM64 FEX and x86_64 host/Rosetta 2 · AC power | LLVM test-suite `3c0a28f12509091808c051c80ca867d9085cd8fb`, retained optimized archive and pre-main inventory wrapper; PE `7bc02482f55361a6f1b71c3265a20faf8f214f75d07ae2fb5f42ac41def12906`; seven demos×100 fixed steps, program-internal BT_PROFILE clocks retained on both paths; candidate/base median0.983005/MAD0.005381,5/6 lower; candidate/Rosetta2.123731/MAD0.019045; paired signed78.519ms saving recovers only3.1561% of Rosetta excess; Identical optimized Windows x64 PE/input; six complete interleaved baseline/candidate/Rosetta orders, exact reference outputs; actual mapped Mach Wine/ntdll/provider/SDK paths, ISA and hashes before main; installed PE provider hashes, not full relocated PE-memory hashes; fresh COW prefixes, bounded clean teardown; main-time JIT included, import/inventory excluded, process-wall and unvalidated guest CPU clocks unscored; no runtime counters/profiler/verbose logging/direct/custom opt-ins; native DLL ISAs/dispatch differ from Rosetta, not pure translator attribution; independent winebth driver_load retained; console-only/no graphics loaded; HOLD isolated candidate, no default/game/release admission |
| LLVM test-suite TSVC ControlFlow-dbl (x64 PE) | Working — 1.48× whole-main slowdown versus Rosetta | 2026-10-05 | Wine HEAD `b861d8e260f933a76c17d5482bbbe376bb4ea51e`, measured dirty `bdc894daa950af7481eda6734131711bad658831f82ed156216f4ffe7434dfcd`; compiled Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · SDK-only immutable-geometry candidate `6131b529987978c4dba6de0560a6cb9ca8f67faf58ca2409690e506b861a6596` / SDK `103ad74d2912797d393a680723a234eaf670c674b57a2fb956e795d872df07af` / loaded FEX library `3f66f57d8435a1aae8516e01550907e5bab84f55c6ce8d6dd3116cafcede871d` / unchanged Unix provider `20cbe32f4322973e661e37aa10bfeed04596d006a4fcca3e16f7cdce113f8211` / ARM64EC PE19 `1b8a38fbcc22aa3aed59fca79a62e01dff0fe8948eb8ea137adab1b9be94cc17` · native baseline `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5` · Rosetta control `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46`; not full-current-source/production/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB host pages · native ARM64 FEX and x86_64 host/Rosetta 2 · AC power | LLVM test-suite `3c0a28f12509091808c051c80ca867d9085cd8fb`, retained optimized archive and pre-main inventory wrapper; PE `b40066c80b8342ecc0ad02b5c905d59964b7fd72a7bd1600c8d7e344384c1244`; arguments `2325 14`; candidate/base1.001483/MAD0.001415,5/6 adverse, not a non-regression certificate; candidate/Rosetta1.484477/MAD0.001355; Identical optimized Windows x64 PE/input; six complete interleaved baseline/candidate/Rosetta orders, exact reference outputs; actual mapped Mach Wine/ntdll/provider/SDK paths, ISA and hashes before main; installed PE provider hashes, not full relocated PE-memory hashes; fresh COW prefixes, bounded clean teardown; main-time JIT included, import/inventory excluded, process-wall and unvalidated guest CPU clocks unscored; no runtime counters/profiler/verbose logging/direct/custom opt-ins; native DLL ISAs/dispatch differ from Rosetta, not pure translator attribution; independent winebth driver_load retained; console-only/no graphics loaded; HOLD isolated candidate, no default/game/release admission |
| CPython 3.13.13 (Windows x64, bundled with Blender 5.2) | Working — 4.55× whole-main slowdown versus Rosetta | 2026-10-05 | Wine HEAD `b861d8e260f933a76c17d5482bbbe376bb4ea51e`, measured dirty `bdc894daa950af7481eda6734131711bad658831f82ed156216f4ffe7434dfcd`; compiled Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · SDK-only immutable-geometry candidate `6131b529987978c4dba6de0560a6cb9ca8f67faf58ca2409690e506b861a6596` / SDK `103ad74d2912797d393a680723a234eaf670c674b57a2fb956e795d872df07af` / loaded FEX library `3f66f57d8435a1aae8516e01550907e5bab84f55c6ce8d6dd3116cafcede871d` / unchanged Unix provider `20cbe32f4322973e661e37aa10bfeed04596d006a4fcca3e16f7cdce113f8211` / ARM64EC PE19 `1b8a38fbcc22aa3aed59fca79a62e01dff0fe8948eb8ea137adab1b9be94cc17` · native baseline `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5` · Rosetta control `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46`; not full-current-source/production/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB host pages · native ARM64 FEX and x86_64 host/Rosetta 2 · AC power | Installed `python.exe -I -S -B`, unchanged interpreter/tree and integer/hash/compression input; candidate/base main0.976116/MAD0.003740,4/6 lower; candidate/Rosetta4.553344/MAD0.082959; paired signed102.832ms saving recovers only3.0445% of Rosetta excess; blob0.963609/MAD0.010082(all6 lower), integer0.975684/MAD0.005757(4/6), compression0.996405/MAD0.019372(mixed,worst1.041363); prior interval/target diagnostics are separate, not this score; Identical optimized Windows x64 PE/input; six complete interleaved baseline/candidate/Rosetta orders, exact reference outputs; actual mapped Mach Wine/ntdll/provider/SDK paths, ISA and hashes before main; installed PE provider hashes, not full relocated PE-memory hashes; fresh COW prefixes, bounded clean teardown; main-time JIT included, import/inventory excluded, process-wall and unvalidated guest CPU clocks unscored; no runtime counters/profiler/verbose logging/direct/custom opt-ins; native DLL ISAs/dispatch differ from Rosetta, not pure translator attribution; independent winebth driver_load retained; console-only/no graphics loaded; HOLD isolated candidate, no default/game/release admission |
| Wine `wineboot.exe` (native ARM64 helper) | Partially working — built-in winebth Bluetooth driver initialization fails | 2026-10-04 | Development Wine `b861d8e260f933a76c17d5482bbbe376bb4ea51e` plus dirty ABI19/FEX C-ABI6 integration · identical complete optimized native/Rosetta source `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · native runtime `88fabd9b9c8b3becb3468e32c359bcb9794f06548618d65b84105eb80954cae1` / SDK `0450775dee4986112f4877450d5e9bbc10921f99e94f7f7ae421606e353a5b59` / FEX library `1fb4df450ee9f376715ca010bd780015fac6be62220356baf9cc803667492d1c` · same-source Rosetta runtime `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46` | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB pages · native ARM64/custom FEX and x86_64 host/Rosetta 2 control | Fresh native ARM64 and source-matched Rosetta prefix bootstrap with pinned Wine Mono 11.2.0; automatic helpers during 84 LLVM/CPython comparisons and 20 FP/flags/native-SEH/x64-SEH/suspension boundary runs · exact runtime-specific update identity, clean owned-process/prefix teardown · CPU-only private runtimes; winebth `driver_load` remains observable |
| LLVM test-suite Fhourstones 2.0 (x64 PE) | Working — 1.74× whole-main slowdown versus Rosetta | 2026-10-05 | Wine HEAD `b861d8e260f933a76c17d5482bbbe376bb4ea51e`, measured dirty `bdc894daa950af7481eda6734131711bad658831f82ed156216f4ffe7434dfcd`; compiled Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · SDK-only immutable-geometry candidate `6131b529987978c4dba6de0560a6cb9ca8f67faf58ca2409690e506b861a6596` / SDK `103ad74d2912797d393a680723a234eaf670c674b57a2fb956e795d872df07af` / loaded FEX library `3f66f57d8435a1aae8516e01550907e5bab84f55c6ce8d6dd3116cafcede871d` / unchanged Unix provider `20cbe32f4322973e661e37aa10bfeed04596d006a4fcca3e16f7cdce113f8211` / ARM64EC PE19 `1b8a38fbcc22aa3aed59fca79a62e01dff0fe8948eb8ea137adab1b9be94cc17` · native baseline `5bacff15dc7f5bd0c9056681edb1f268de18df9cc8188b033a02cd7a918b29a5` · Rosetta control `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46`; not full-current-source/production/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB host pages · native ARM64 FEX and x86_64 host/Rosetta 2 · AC power | LLVM test-suite `3c0a28f12509091808c051c80ca867d9085cd8fb`, retained optimized archive and pre-main inventory wrapper; PE `1f57b0537dbfa53455252a2035d318c06923af15cbecd4a8ac9bd321168a9880`; pinned stdin; initial candidate/base0.967653/MAD0.060816 was too noisy; six additional complete order permutations give1.001737/MAD0.008816,3/6 lower; all12 candidate/base0.992779/MAD0.018466,7/12 lower,worst1.085570/min0.862147 retained; initial3.23% gain not reproduced; all12 candidate/Rosetta1.738558/MAD0.020266; strata/raw results separate, no outlier exclusion; Identical optimized Windows x64 PE/input; six complete interleaved baseline/candidate/Rosetta orders, exact reference outputs; actual mapped Mach Wine/ntdll/provider/SDK paths, ISA and hashes before main; installed PE provider hashes, not full relocated PE-memory hashes; fresh COW prefixes, bounded clean teardown; main-time JIT included, import/inventory excluded, process-wall and unvalidated guest CPU clocks unscored; no runtime counters/profiler/verbose logging/direct/custom opt-ins; native DLL ISAs/dispatch differ from Rosetta, not pure translator attribution; independent winebth driver_load retained; console-only/no graphics loaded; HOLD isolated candidate, no default/game/release admission |
| LLVM test-suite SciMark2-C (x64 PE) | Working | 2026-10-04 | Development Wine `b861d8e260f933a76c17d5482bbbe376bb4ea51e` plus dirty integration; frozen Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · original native runtime `88fabd9b9c8b3becb3468e32c359bcb9794f06548618d65b84105eb80954cae1` · Rosetta runtime `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46` · private candidate `306a0b6b2c7433b7fe2f22007fdb7dac94c0c65ac072e54038fe074d4de3eec7` / Unix component `b66c2f7f864163d1875c2ef673906b2efd328f53c74c1a388cf6184d19792bd9` / SDK `f2d72e27fbae86dad2d4e94ed05465386c08d19a096a4592b8cad52a8ea47764` / library `cc680f9b4da9c10a98625f203d00e45ad8ec91ad5ffe7bc063f35d98e8637396`; ABI19/6 with unpublished fused-entry API, not full-source/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB pages · original/candidate native ARM64 FEX and x86_64 host/Rosetta 2 | LLVM test-suite `3c0a28f12509091808c051c80ca867d9085cd8fb`, optimized x64 PE; Six complete three-runtime order permutations, exact reference outputs and authenticated fresh COW prefixes; unchanged PE/input/runtime/tool/seed/process hashes; ordinary dispatcher, direct/custom opt-ins and instrumentation unset; independent winebth `driver_load` retained; CPU-only diagnostic, no default/graphics/release promotion; paired candidate/original main median 1.000749 (MAD 0.000690); candidate/Rosetta 2.102149; no meaningful gain claimed |
| LLVM test-suite MiBench automotive-bitcount (x64 PE) | Working | 2026-10-04 | Development Wine `b861d8e260f933a76c17d5482bbbe376bb4ea51e` plus dirty integration; frozen Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · original native runtime `88fabd9b9c8b3becb3468e32c359bcb9794f06548618d65b84105eb80954cae1` · Rosetta runtime `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46` · private candidate `306a0b6b2c7433b7fe2f22007fdb7dac94c0c65ac072e54038fe074d4de3eec7` / Unix component `b66c2f7f864163d1875c2ef673906b2efd328f53c74c1a388cf6184d19792bd9` / SDK `f2d72e27fbae86dad2d4e94ed05465386c08d19a096a4592b8cad52a8ea47764` / library `cc680f9b4da9c10a98625f203d00e45ad8ec91ad5ffe7bc063f35d98e8637396`; ABI19/6 with unpublished fused-entry API, not full-source/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB pages · original/candidate native ARM64 FEX and x86_64 host/Rosetta 2 | LLVM test-suite `3c0a28f12509091808c051c80ca867d9085cd8fb`, optimized x64 PE, argument `1125000`; Six complete three-runtime order permutations, exact reference outputs and authenticated fresh COW prefixes; unchanged PE/input/runtime/tool/seed/process hashes; ordinary dispatcher, direct/custom opt-ins and instrumentation unset; independent winebth `driver_load` retained; CPU-only diagnostic, no default/graphics/release promotion; paired candidate/original main median 1.001059 (MAD 0.057543); candidate/Rosetta 1.691955; short noisy discriminator, not an aggregate performance score |
| LLVM test-suite MiBench network-dijkstra (x64 PE) | Working | 2026-10-04 | Development Wine `b861d8e260f933a76c17d5482bbbe376bb4ea51e` plus dirty integration; frozen Wine parent `eeb82a79d4519304922a89f332816dc94c7adf07ebb59e3489b7cb026a1638d6` · original native runtime `88fabd9b9c8b3becb3468e32c359bcb9794f06548618d65b84105eb80954cae1` · Rosetta runtime `5ba626ce7c104fd7a656f13818fbc0f788a9b644e3b744065a31b7eaa7fa4d46` · private candidate `306a0b6b2c7433b7fe2f22007fdb7dac94c0c65ac072e54038fe074d4de3eec7` / Unix component `b66c2f7f864163d1875c2ef673906b2efd328f53c74c1a388cf6184d19792bd9` / SDK `f2d72e27fbae86dad2d4e94ed05465386c08d19a096a4592b8cad52a8ea47764` / library `cc680f9b4da9c10a98625f203d00e45ad8ec91ad5ffe7bc063f35d98e8637396`; ABI19/6 with unpublished fused-entry API, not full-source/package admission | macOS 26.5.2 (25F84) · Apple M5 Pro (Mac17,8) · 24 GiB / 16 KiB pages · original/candidate native ARM64 FEX and x86_64 host/Rosetta 2 | LLVM test-suite `3c0a28f12509091808c051c80ca867d9085cd8fb`, optimized x64 PE and pinned `input.dat`; Six complete three-runtime order permutations, exact reference outputs and authenticated fresh COW prefixes; unchanged PE/input/runtime/tool/seed/process hashes; ordinary dispatcher, direct/custom opt-ins and instrumentation unset; independent winebth `driver_load` retained; CPU-only diagnostic, no default/graphics/release promotion; paired candidate/original main median 0.997069 (MAD 0.012720); candidate/Rosetta 3.375148; short noisy discriminator, not an aggregate performance score |
| Kiwoom Hero4 (`NKStarter.exe`, i386) | Working | 2026-08-25 | Switchyard Wine source `6deeaee807b8b3b958427ed6bb291231f3d944fb` · native ARM64 v6 candidate `runtime-demand-map-v6` · `xtajit.so` SHA-256 `c9b7a0c3e608724f2fd0b5d1e6b6c881b201113ce6bf2512f4dee8fd44686701` · `libunicorn.2.dylib` SHA-256 `22e50756691436cb060cb8292beae426cb8aee5a95caf84410321b1aab8793e8` · `wow64win.dll` SHA-256 `5f585ebf01eae261c6ccd4f673da9e0668b6a9ac16ebf31787ac9d6eb01553d4` | macOS 26.5.2 (25F84) · Apple M5 Pro · native ARM64 · no Rosetta 2 | Fully synchronized prepared migrated prefix · normal `nkrunlite.exe` → `NKStarter.exe` path · XTAJIT i386 provider · visible 758×521 login dialog and 253×409 connection-settings state transition |
| 7-Zip 26.02 (x64) | Working | 2026-08-13 | Switchyard Wine source `05343646b5b84609d9ad15ea39f8136bbf776906` with dirty-tree digest `701670918da4a68ff08c3c6ae7acb1dea3c15ef09a5f7916ae4db20ef5d93eaa` · native ARM64 runtime closure `810b01abb70078d2622838be866b152a6ab0a319a9776cc39a66f6f342b99150` | macOS 26.5.2 (25F84) · Apple M5 Pro · native ARM64 · no Rosetta 2 | Direct x64 installer, `7z.exe`, and 7-Zip File Manager · ARM64EC CPU provider · deterministic archive create, test, extract, and visible-window launch |
| Blender 5.2.0 LTS | Working | 2026-08-25 | Switchyard Wine source `6deeaee807b8b3b958427ed6bb291231f3d944fb` · native ARM64 v6 candidate `runtime-demand-map-v6` · `xtajit64.so` SHA-256 `fdbbeebc876ba5e952c603db56a3e5eaa8b40f2230a36c1d78fb73cd3022f10e` · `libunicorn.2.dylib` SHA-256 `22e50756691436cb060cb8292beae426cb8aee5a95caf84410321b1aab8793e8` | macOS 26.5.2 (25F84) · Apple M5 Pro · native ARM64 · no Rosetta 2 | Direct x86_64 `blender.exe --background --factory-startup` · deterministic 480-iteration scene workload · six checkpoints and final scene digest verified |
| Heartopia (Steam build 24086143) | Working | 2026-08-31 | Switchyard Wine source closure `3c3392ffe8c6bbc02f5a6073d498c67c83e972c3` plus tracked hashed-index commit `59aeb9c1754d45e4cf7eddb2f0a8bc40c5545938` · contract-20 commit `22bb836242acbb496806c4946d9f2b3f54b69af1` · base runtime manifest SHA-256 `0d431eb98738c484a50084da1c9ba4f864e2352a59e7e6ac8840f3f44f191ff6` · `xtajit64.so` SHA-256 `22b7d481c9328cca1cc0b454cc83610de4bad7925d4fc545584b755d4355ff49` · hashed ARM64EC `xtajit64.dll` SHA-256 `779a460f066d8deea6beccfb56f636e12769aea31490a28dce2ffc751488e2f5` · Unicorn patch SHA-256 `96a647d57f6f749c3c3864ead959c2e9306488151f5fed468e6ad334483e6cc5` · contract v20 signed dylib SHA-256 `1ea70e727ae6db21ea15ac449cd253740263efa5ffeece27f9c3c09b4a1fee1c` · benchmark-only DXMT closure `4935aa3ee1369efd0a2c0816320d14aa4db44fbc` | macOS 26.5.2 (25F84) · Apple M5 Pro · native ARM64 · no Rosetta 2 | Direct x86_64 `xdt.exe` from a fresh contract-20 candidate copy-on-write Steam-prefix clone (App ID 4025700) · diagnostics, profiler, and performance counters disabled · XTAJIT64 · D3D11/DXGI → DXMT/Winemetal/Metal · feature level 11.1 and swapchain creation · hotfix/resource validation, Mono startup, scene loading, and `TableData.PostInit` verified before clean bounded teardown |
| TEKKEN 8 Demo (Steam build 13144577) | Not working — native ARM64 direct launch faults before an Unreal log; the source-matched control also faults | 2026-08-31 | Switchyard Wine source closure `3c3392ffe8c6bbc02f5a6073d498c67c83e972c3` plus tracked hashed-index commit `59aeb9c1754d45e4cf7eddb2f0a8bc40c5545938` · contract-20 commit `22bb836242acbb496806c4946d9f2b3f54b69af1` · shared `xtajit64.so` SHA-256 `22b7d481c9328cca1cc0b454cc83610de4bad7925d4fc545584b755d4355ff49` · Unicorn patch SHA-256 `96a647d57f6f749c3c3864ead959c2e9306488151f5fed468e6ad334483e6cc5` · contract v20 signed dylib SHA-256 `1ea70e727ae6db21ea15ac449cd253740263efa5ffeece27f9c3c09b4a1fee1c` · prior source-matched control dylib SHA-256 `e47a410bcdfb99d2542eacc850d09495d30f9869999a0d2833cad58755a660c2` · benchmark-only DXMT closure `4935aa3ee1369efd0a2c0816320d14aa4db44fbc` | macOS 26.5.2 (25F84) · Apple M5 Pro · native ARM64 · no Rosetta 2 | Direct x86_64 `TEKKEN 8 Demo.exe` from a fresh contract-20 candidate copy-on-write Steam-prefix clone (App ID 2524440) · generic `-NoSteam -dx11` launch · diagnostics, profiler, and performance counters disabled · XTAJIT64 · unhandled execute-access fault before graphics initialization · clean bounded teardown |
| Terraria (Steam build 22266454) | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 105600) · GPTK 4.0 beta 1 · 32-bit Steam overlay hotpatch path |
| Steam (client build 1785799196) | Working | 2026-08-05 | Switchyard Wine source `15214c91b6bbcfbcc2be5ed7a69525111c4abb9c` · GPTK 4 runtime `switchyard-local-wow64-x86_64-15214c91b6bb-05ce84e4cb6f-b4525679e7da-a73cda115f3e-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct Windows Steam client · GPTK 4.0 beta 1 · automatic login · CEF Store · `SC_MOVE` client-frame path exercised under CGWindow sampling |
| Battle.net 2.52.5.17620 | Working | 2026-07-31 | Switchyard Wine source `f04d651c48a756eb75ac16407bc5fd5b15e54462` · GPTK 4 runtime `switchyard-local-wow64-x86_64-f04d651c48a7-05ce84e4cb6f-b4525679e7da-a73cda115f3e-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `Battle.net.exe` · GPTK 4.0 beta 1 · automatic login · CEF/ANGLE GPU/renderer and DComp content visually confirmed |
| KakaoTalk | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `KakaoTalk.exe` · shared Steam prefix · GPTK 4.0 beta 1 |
| Goose Goose Duck (Steam build 23328520) | Blocked — Easy Anti-Cheat module mapping failed; excluded from runtime diagnosis at user request | 2026-07-25 | Switchyard Wine source `b9c90d58679b8022915ff040e139143064477111` · GPTK 4 runtime `switchyard-local-wow64-x86_64-b9c90d58679b-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 1568590) · Easy Anti-Cheat launcher |
| Poppy Playtime (Steam build 21905565) | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 1721470) · launcher plus Steam-tracked game executable · GPTK 4.0 beta 1 |
| Unturned 3.26.3.4 (Steam build 24080152) | Partially working — BattlEye is unsupported; cancelling it reaches the non-BattlEye main menu | 2026-07-25 | Switchyard Wine source `d08996c05014888ca1c0f81b95b6d5555f32efa5` · GPTK 4 runtime `switchyard-local-wow64-x86_64-d08996c05014-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 304930) · BattlEye cancelled · D3D11/DXGI → D3DMetal · asset load and `Menu UI ready` verified |
| Bro Falls: Ultimate Showdown | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Pratfall | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · Vulkan renderer |
| Overwatch 2.23.1.1 (build 151722.1; executable 2.23.1.20650) | Working | 2026-07-30 | Switchyard Wine source `499b1b79c110181572510d2269abaf3810b71ae6` with dirty-tree digest `874f48c4c938` · Wine-only runtime `switchyard-local-wow64-x86_64-499b1b79c110-dirty-874f48c4c938-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Battle.net `--exec=launch Pro` · externally selected GPTK 4.0 beta 1 · D3D11/DXGI → D3DMetal · logged-in main menu visually confirmed |
| Supermarket Together | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Sonic the Hedgehog (P-06) Patch 1.45 | Working | 2026-08-25 | Switchyard Wine source `6deeaee807b8b3b958427ed6bb291231f3d944fb` · native ARM64 runtime content SHA-256 `e8c655bce7857dbc9771e65a3be2f1452d300bb80ef57f93353bb79248facd3b` · closure SHA-256 `e62998411ebc6cbe70a1ba58bcb56358c443e6db3fc8757e88a65d44d3931229` · `xtajit64.so` SHA-256 `246fe3dbd3360ed79dc058ed8ee0df8a6e79518f3918e146ad4530e202081981` · `libunicorn.2.dylib` SHA-256 `f98f65ff58b3455b3fbab7585d82fc004dc62676250117958250cd266c57a54a` · DXMT artifact `af8ab67d197a4bc6751483b8c16fa17df3b0a6b0` | macOS 26.5.2 (25F84) · Apple M5 Pro · native ARM64 · no Rosetta 2 | Direct x86_64 `S:\Sonic the Hedgehog.exe` through an exact payload drive mapping · strict fresh prefix · D3D11/DXGI → DXMT/Winemetal/Metal · documented DXMT Wine-stderr diagnostics |
| Rockstar Games Launcher 1.0.108.2970 | Working | 2026-07-31 | Switchyard Wine source `f04d651c48a756eb75ac16407bc5fd5b15e54462` · GPTK 4 runtime `switchyard-local-wow64-x86_64-f04d651c48a7-05ce84e4cb6f-b4525679e7da-a73cda115f3e-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Spawned by `PlayGTAV.exe -nobattleye` from the GTA V Enhanced installation directory · GPTK 4.0 beta 1 · signed-in launch and current-title verification |
| Google Chrome 150.0.7871.187 | Working | 2026-08-25 | Switchyard Wine source `6deeaee807b8b3b958427ed6bb291231f3d944fb` · native ARM64 v6 candidate `runtime-demand-map-v6` · `xtajit64.so` SHA-256 `be12bca3d3a80289baa2e19f42b18bbf777b790b1897f5c1a1f7fd48e1ade125` · `xtajit64.dll` SHA-256 `d28a6b5a327721882c7648ca992aad8a0234af3274ad5363220e4c84e7eba1ea` · `libunicorn.2.dylib` SHA-256 `22e50756691436cb060cb8292beae426cb8aee5a95caf84410321b1aab8793e8` | macOS 26.5.2 (25F84) · Apple M5 Pro · native ARM64 · no Rosetta 2 | Direct x86_64 `chrome.exe` · isolated prefix clone · deterministic local page · default GPU and sandbox paths with no bypass flags · rendered `ready` state and checksum `3685088189` retained beyond six minutes |
| Grand Theft Auto V Enhanced 1.0.1158.13 | Working | 2026-08-27 | Switchyard Wine source `34a09170d3754522dc55416617c6754dd2d9a086` · GitHub release runtime `switchyard-local-wow64-x86_64-34a09170d375-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Rockstar container · official `PlayGTAV.exe` → signed-in Rockstar Games Launcher → `GTA5_Enhanced.exe` · GPTK 4.0 beta 1 · Direct3D 12 / D3DMetal · Story Mode entered with no crash observed |
| RV There Yet? 1.2.0.17491 (Steam build 22864294) | Working — first-launch PSO precompilation took 96 seconds | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam default launch (App ID 3949040; no game launch options) · GPTK 4.0 beta 1 · Direct3D 12 / SM6 |
| Epic Games Launcher 20.1.6-56134335 | Working | 2026-07-29 | Switchyard Wine source `88ca753ede989a12a747aef7e7785d390c38b6d2` · GPTK 4 runtime `switchyard-local-wow64-x86_64-88ca753ede98-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `EpicGamesLauncher.exe` · dedicated Switchyard container · SwiftShader · CEF native-child-to-OSR fallback · signed-in Store rendering and input verified |
| Sons Of The Forest (Steam build 20228174) | Working | 2026-07-30 | Switchyard Wine source `499b1b79c110181572510d2269abaf3810b71ae6` · Wine-only runtime `switchyard-local-wow64-x86_64-499b1b79c110-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 1326470) · externally selected GPTK 4.0 beta 1 · D3D11/DXGI → D3DMetal · in-game rendering and input verified |
| NexonPlug Installer 1.0.0.15 | Working | 2026-07-31 | Switchyard Wine source `9ef2193eceacbd9d13a13958979fe2891a241996` with dirty-tree digest `71754481d573` · GPTK 4 runtime `switchyard-local-wow64-x86_64-9ef2193eceac-dirty-71754481d573-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `Install_NexonPlug.exe /install` · General Switchyard container · WebBrowser/MSHTML with Wine Gecko 2.47.4 · GPTK 4.0 beta 1 selected · full download and installation to `ProgramData\Nexon` verified |
| Life is Strange (Steam build 2304417) | Working | 2026-08-10 | Switchyard Wine source `66ece46d693dce6511a3a2de72e7d2c3c2e9f8a5` · Wine-only runtime `switchyard-local-wow64-x86_64-66ece46d693d-no-gptk-b4525679e7da-a73cda115f3e-9245db166022-37a4f0cfb0fb-4fbf9011be92-f35d3a3b07ea-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 319630) · Direct3D 9 → WineD3D/OpenGL · 1280×720 · VSync enabled · Save 1 classroom scene |

## Status interpretation

`Working` means that no blocking issue was observed within the recorded workflow. A visible window
or live process is not sufficient: current checks require an application-specific load-completion
milestone, temporal progress, a safe input response, and no new crash or unexpected Steam tracking
loss. Historical rows without those details should not be interpreted as a guarantee that every
feature works. Status notes are reserved for confirmed bugs and material limitations, not successful
behavior or untested workflows.

## Excluded capability classes

The native ARM64 runtime does not claim compatibility for Windows kernel-mode
drivers, kernel-dependent anti-cheat or anti-tamper systems, hardware
virtualization or hypervisor products, or software whose required hardware DRM
or trusted-execution path is unavailable through Wine. These are capability
boundaries, not evidence that the ordinary user-mode portion of every affected
application was tested or failed.

Compatibility may change with application updates, runtime revisions, macOS versions, hardware,
or graphics layers. Future updates should record the application version and distribution path,
verification date, runtime revision, host environment, and any known limitations.

The shared 2026-07-21 runtime revision expands to
`783c55de9a5b631b6710ed690ec696654a7d17b9`. It uses the `switchyard-wow64-pe` profile
with `i386` and `x86_64` PE support. GPTK refers to the user-provided Apple Game Porting
Toolkit 3.0 overlay; it is not distributed by this repository.
