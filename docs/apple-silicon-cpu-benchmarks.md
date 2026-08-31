# Apple Silicon CPU bottleneck benchmarks

Switchyard's native ARM64 CPU provider executes x86-64 Windows code on Apple
Silicon. A compatibility test can prove that a workload completes, but it does
not show whether a change introduced a branch, instruction-delivery, execution,
or memory-system bottleneck. This benchmark supplies small, deterministic guest
workloads for that purpose.

The design follows Apple's Apple Silicon CPU optimization guidance without
copying chip-specific latency tables, private event encodings, or absolute
thresholds into the repository. The durable process is:

1. define the latency or throughput question;
2. isolate one processor behaviour at a time;
3. collect repeated measurements on the same physical Mac;
4. compare a baseline runtime with a candidate runtime; and
5. use Instruments CPU Counters or Processor Trace to explain a measured
   regression before changing the implementation.

The benchmark is an x86-64 Windows executable and only uses public Win32 APIs.
It does not call a private Wine, winemac, or `xtajit64` entry point. The same
executable can therefore run against two runtime trees and exercise their real
process, WoW64, CPU-provider, memory-protection, and translated-execution paths.

## Cases

Lower `ns` values are better. A pair of related cases is more informative than
an isolated absolute number.

| Case | Intended bottleneck signal |
|---|---|
| `dependent_muladd` | A single long integer operation dependency chain; a latency-bound execution proxy. |
| `independent_muladd_8` | Eight independent chains with the same total guest-operation count; an instruction-level parallelism and execution-bandwidth proxy. |
| `branch_predictable` | A fixed conditional outcome with one taken branch per decision. |
| `branch_unpredictable` | The same branch body and taken-branch density with deterministic pseudo-random outcomes; a conditional-prediction proxy. |
| `indirect_targets_1` | Repeated calls through one indirect target. |
| `indirect_targets_16` | The same call sequence distributed over sixteen targets; an indirect-target prediction and dispatch proxy. |
| `pointer_chase_8k` | Dependent loads from a small working set. |
| `pointer_chase_256k` | Dependent loads from a medium working set. |
| `pointer_chase_16m` | Dependent loads spanning a large working set and many translations. |
| `line_load_aligned` | Repeated 16-byte guest loads beginning at a 128-byte boundary. |
| `line_load_cross_128b` | The same load width beginning eight bytes before a 128-byte boundary. |
| `store_load_exact` | A load from exactly the address and width of the immediately preceding store. |
| `store_load_partial_overlap` | A load shifted by one byte from the preceding store; a store-to-load forwarding stressor. |
| `code_footprint_4k` | Generated guest arithmetic code with a small instruction footprint. |
| `code_footprint_64k` | The same generated instruction pattern with a medium footprint. |
| `code_footprint_256k` | The same generated instruction pattern with a large footprint. |
| `jit_republish_page` | Repeated `RX -> RW -> RX` publication, `FlushInstructionCache`, and execution of modified guest code. |

The code-footprint cases execute real `add` instructions instead of a NOP sled.
This prevents a translator from legitimately discarding the entire generated
body and makes the reported unit, `guest_instruction`, useful for relative
comparison.

These are diagnostic proxies, not microarchitectural specifications. For
example, the 16 MiB pointer chase may involve several cache and TLB levels, and
the JIT case intentionally includes the public Win32 protection and publication
contract rather than pretending to measure only one cache-maintenance
instruction.

## Run a runtime

Requirements are the same as the Apple Silicon build, including
`x86_64-w64-mingw32-gcc`.

```sh
./switchyard/tests/apple_silicon_cpu_bottleneck_benchmark.sh \
  ~/.switchyard/runtimes/<runtime-id> 5 \
  > baseline.log 2> baseline.stderr
```

Run the candidate on the **same physical Mac**:

```sh
./switchyard/tests/apple_silicon_cpu_bottleneck_benchmark.sh \
  ~/.switchyard/runtimes/<candidate-runtime-id> 5 \
  > candidate.log 2> candidate.stderr
```

The runner refuses non-Apple-Silicon hosts, builds one x86-64 executable, uses a
fresh temporary prefix, records the chip, Mac model, macOS build, logical and
physical CPU counts, power and thermal state, and runtime-manifest digest, then
performs the requested number of runs. The prefix is shared only among runs in
one invocation; benchmark timings do not include prefix startup.

Useful controls are:

```sh
APPLE_CPU_BENCH_TARGET_MS=150 \
APPLE_CPU_BENCH_TRIALS=11 \
APPLE_CPU_BENCH_CASE=indirect_targets_16 \
  ./switchyard/tests/apple_silicon_cpu_bottleneck_benchmark.sh \
  ~/.switchyard/runtimes/<runtime-id> 7
```

- `APPLE_CPU_BENCH_TARGET_MS` controls the calibrated measurement duration per
  trial, from 10 through 10,000 milliseconds.
- `APPLE_CPU_BENCH_TRIALS` controls the in-process sample count, from 3 through
  101. The benchmark reports min, p50, p95, and max.
- `APPLE_CPU_BENCH_CASE` selects one exact case name; the default is `all`.

A displayed `ns` value is an aggregate elapsed interval divided by a large
number of logical operations. It is not a claim that one timer observation has
single-nanosecond accuracy.

## Compare two logs

```sh
./switchyard/tests/apple_silicon_cpu_bottleneck_compare.py \
  baseline.log candidate.log --format markdown
```

The comparator:

- rejects malformed, duplicate, incomplete, zero, NaN, or infinite metrics;
- requires every run to contain the same case set;
- requires matching benchmark duration, trial count, and case filter;
- rejects a different host signature by default; and
- compares the median of each run's p50 and p95 values.

It does not impose a universal regression threshold. M-series generations,
power states, and macOS releases have different performance characteristics.
For a controlled CI or local qualification gate, opt in explicitly:

```sh
./switchyard/tests/apple_silicon_cpu_bottleneck_compare.py \
  baseline.log candidate.log --fail-above-pct 5
```

A threshold should be justified by repeated measurements on the particular Mac,
not copied between M-series families. Alternate baseline and candidate runs when
possible, keep the machine on a stable power source, close unrelated workloads,
and investigate thermal or context-switch outliers recorded by the runner.

## Diagnose a regression with Instruments

First reduce the run to the affected case and increase its target duration. Run
that executable path under Instruments with CPU Counters or Processor Trace.
The case category narrows the first question:

- `execution`: dependency latency, execution bandwidth, or insufficient
  independent work;
- `branch`: discarded work from conditional or indirect prediction;
- `instruction_delivery`: translated-code layout, instruction-cache or
  instruction-TLB pressure, and taken-branch delivery;
- `memory`: dependent-load latency, data-cache/TLB behaviour, alignment,
  dependence prediction, or forwarding; and
- `jit`: memory-protection transitions, code invalidation, publication, and
  re-entry into modified code.

Do not infer a cause from elapsed time alone. A slower pointer-chase result does
not by itself prove an L1 data-cache regression, and a slower large-code result
does not by itself prove an instruction-cache miss. Use counters, a trace, and a
source-level change hypothesis together.

## Qualification boundary

This suite intentionally does not claim that a synthetic improvement guarantees
better game performance. It is a stable reproducer for narrowing a runtime
regression and checking whether a proposed fix moves the expected processor
behaviour. Keep application compatibility and representative game or launcher
measurements beside it when qualifying a runtime release.
