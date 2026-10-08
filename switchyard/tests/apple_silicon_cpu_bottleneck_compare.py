#!/usr/bin/env python3
"""Compare two Switchyard Apple Silicon CPU benchmark logs."""

from __future__ import annotations

import argparse
import json
import math
import shlex
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


class BenchmarkLogError(ValueError):
    """Raised when a benchmark log violates the machine-readable contract."""


@dataclass(frozen=True)
class Metric:
    case: str
    category: str
    unit: str
    p50_ns: float
    p95_ns: float


@dataclass
class BenchmarkLog:
    path: Path
    host: dict[str, str]
    runtime: dict[str, str]
    benchmark_headers: dict[int, dict[str, str]]
    metrics: dict[int, dict[str, Metric]]
    completed_runs: set[int]

    @property
    def run_ids(self) -> list[int]:
        return sorted(self.metrics)

    @property
    def cases(self) -> set[str]:
        result: set[str] = set()
        for run_metrics in self.metrics.values():
            result.update(run_metrics)
        return result


def parse_key_values(parts: Iterable[str], *, path: Path, line_number: int) -> dict[str, str]:
    values: dict[str, str] = {}
    for part in parts:
        if "=" not in part:
            raise BenchmarkLogError(f"{path}:{line_number}: malformed field {part!r}")
        key, value = part.split("=", 1)
        if not key or not value:
            raise BenchmarkLogError(f"{path}:{line_number}: malformed field {part!r}")
        if key in values:
            raise BenchmarkLogError(f"{path}:{line_number}: duplicate field {key!r}")
        values[key] = value
    return values


def require_fields(record: dict[str, str], fields: Iterable[str], *, path: Path,
                   line_number: int, record_type: str) -> None:
    missing = [field for field in fields if field not in record]
    if missing:
        raise BenchmarkLogError(
            f"{path}:{line_number}: {record_type} is missing {', '.join(missing)}"
        )


def parse_run(record: dict[str, str], *, path: Path, line_number: int) -> int:
    try:
        run = int(record["run"], 10)
    except (KeyError, ValueError) as error:
        raise BenchmarkLogError(f"{path}:{line_number}: invalid run number") from error
    if run <= 0:
        raise BenchmarkLogError(f"{path}:{line_number}: run must be positive")
    return run


def parse_positive_float(value: str, *, path: Path, line_number: int,
                         field: str) -> float:
    try:
        result = float(value)
    except ValueError as error:
        raise BenchmarkLogError(
            f"{path}:{line_number}: {field} is not a floating-point number"
        ) from error
    if not math.isfinite(result) or result <= 0.0:
        raise BenchmarkLogError(f"{path}:{line_number}: {field} must be finite and positive")
    return result


def parse_log(path: Path) -> BenchmarkLog:
    host: dict[str, str] | None = None
    runtime: dict[str, str] | None = None
    headers: dict[int, dict[str, str]] = {}
    metrics: dict[int, dict[str, Metric]] = {}
    completed_runs: set[int] = set()

    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise BenchmarkLogError(f"cannot read {path}: {error}") from error

    for line_number, line in enumerate(lines, 1):
        if not line.startswith("apple_cpu_"):
            continue
        try:
            parts = shlex.split(line, posix=True)
        except ValueError as error:
            raise BenchmarkLogError(f"{path}:{line_number}: cannot parse record: {error}") from error
        if not parts:
            continue
        record_type = parts[0]
        record = parse_key_values(parts[1:], path=path, line_number=line_number)
        if record.get("schema") != "1":
            raise BenchmarkLogError(
                f"{path}:{line_number}: unsupported schema for {record_type}"
            )

        if record_type == "apple_cpu_host":
            require_fields(record, ("signature", "hw_model", "chip", "macos", "build", "arch"),
                           path=path, line_number=line_number, record_type=record_type)
            if host is not None and host != record:
                raise BenchmarkLogError(f"{path}:{line_number}: conflicting host records")
            host = record
        elif record_type == "apple_cpu_runtime":
            require_fields(record, ("name", "manifest_sha256", "runs", "target_ms", "trials", "case"),
                           path=path, line_number=line_number, record_type=record_type)
            if runtime is not None and runtime != record:
                raise BenchmarkLogError(f"{path}:{line_number}: conflicting runtime records")
            runtime = record
        elif record_type == "apple_cpu_benchmark":
            require_fields(record, ("run", "benchmark_version", "architecture", "target_ms",
                                    "trials", "qpc_frequency", "filter"),
                           path=path, line_number=line_number, record_type=record_type)
            run = parse_run(record, path=path, line_number=line_number)
            if run in headers:
                raise BenchmarkLogError(f"{path}:{line_number}: duplicate benchmark header for run {run}")
            headers[run] = record
        elif record_type == "apple_cpu_metric":
            require_fields(record, ("run", "status", "case", "category", "unit", "p50_ns", "p95_ns"),
                           path=path, line_number=line_number, record_type=record_type)
            if record["status"] != "ok":
                raise BenchmarkLogError(f"{path}:{line_number}: non-success metric status")
            run = parse_run(record, path=path, line_number=line_number)
            metric = Metric(
                case=record["case"],
                category=record["category"],
                unit=record["unit"],
                p50_ns=parse_positive_float(record["p50_ns"], path=path,
                                            line_number=line_number, field="p50_ns"),
                p95_ns=parse_positive_float(record["p95_ns"], path=path,
                                            line_number=line_number, field="p95_ns"),
            )
            run_metrics = metrics.setdefault(run, {})
            if metric.case in run_metrics:
                raise BenchmarkLogError(
                    f"{path}:{line_number}: duplicate metric {metric.case!r} for run {run}"
                )
            run_metrics[metric.case] = metric
        elif record_type == "apple_cpu_complete":
            require_fields(record, ("run", "cases", "sink"), path=path,
                           line_number=line_number, record_type=record_type)
            run = parse_run(record, path=path, line_number=line_number)
            if run in completed_runs:
                raise BenchmarkLogError(f"{path}:{line_number}: duplicate completion for run {run}")
            completed_runs.add(run)

    if host is None:
        raise BenchmarkLogError(f"{path}: no apple_cpu_host record")
    if runtime is None:
        raise BenchmarkLogError(f"{path}: no apple_cpu_runtime record")
    if not metrics:
        raise BenchmarkLogError(f"{path}: no apple_cpu_metric records")

    run_ids = sorted(metrics)
    if set(run_ids) != completed_runs:
        raise BenchmarkLogError(
            f"{path}: metric runs {run_ids} do not match completed runs {sorted(completed_runs)}"
        )
    if set(run_ids) != set(headers):
        raise BenchmarkLogError(
            f"{path}: metric runs {run_ids} do not match benchmark headers {sorted(headers)}"
        )
    try:
        expected_runs = int(runtime["runs"], 10)
    except ValueError as error:
        raise BenchmarkLogError(f"{path}: runtime run count is not an integer") from error
    if expected_runs <= 0 or expected_runs > 100:
        raise BenchmarkLogError(f"{path}: runtime run count is outside 1..100")
    if run_ids != list(range(1, expected_runs + 1)):
        raise BenchmarkLogError(
            f"{path}: expected runs 1..{expected_runs}, found {run_ids}"
        )

    expected_cases = set(metrics[run_ids[0]])
    reference_header = headers[run_ids[0]]
    for run in run_ids:
        if set(metrics[run]) != expected_cases:
            raise BenchmarkLogError(
                f"{path}: run {run} has a different benchmark case set"
            )
        header = headers[run]
        if (header["benchmark_version"] != reference_header["benchmark_version"] or
                header["architecture"] != reference_header["architecture"]):
            raise BenchmarkLogError(
                f"{path}: run {run} changes the benchmark version or architecture"
            )
        if header["target_ms"] != runtime["target_ms"] or header["trials"] != runtime["trials"]:
            raise BenchmarkLogError(
                f"{path}: run {run} header disagrees with the runner configuration"
            )
        if header["filter"] != runtime["case"]:
            raise BenchmarkLogError(f"{path}: run {run} filter disagrees with the runner")

    for case in expected_cases:
        reference = metrics[run_ids[0]][case]
        for run in run_ids[1:]:
            current = metrics[run][case]
            if current.category != reference.category or current.unit != reference.unit:
                raise BenchmarkLogError(
                    f"{path}: metadata for {case!r} changes between runs"
                )

    return BenchmarkLog(path, host, runtime, headers, metrics, completed_runs)


def aggregate(log: BenchmarkLog, case: str, field: str) -> float:
    values = [getattr(log.metrics[run][case], field) for run in log.run_ids]
    return statistics.median(values)


def validate_pair(baseline: BenchmarkLog, candidate: BenchmarkLog,
                  *, allow_host_mismatch: bool, allow_config_mismatch: bool) -> None:
    if baseline.host["signature"] != candidate.host["signature"] and not allow_host_mismatch:
        raise BenchmarkLogError(
            "host signatures differ; benchmark the two runtimes on the same physical Mac "
            "or pass --allow-host-mismatch for exploratory output"
        )
    config_fields = ("runs", "target_ms", "trials", "case")
    mismatches = [
        field for field in config_fields
        if baseline.runtime[field] != candidate.runtime[field]
    ]
    if mismatches and not allow_config_mismatch:
        raise BenchmarkLogError(
            "benchmark configurations differ in " + ", ".join(mismatches)
        )
    if baseline.cases != candidate.cases:
        only_baseline = sorted(baseline.cases - candidate.cases)
        only_candidate = sorted(candidate.cases - baseline.cases)
        raise BenchmarkLogError(
            f"benchmark case sets differ; baseline-only={only_baseline}, "
            f"candidate-only={only_candidate}"
        )
    for case in baseline.cases:
        left = baseline.metrics[baseline.run_ids[0]][case]
        right = candidate.metrics[candidate.run_ids[0]][case]
        if (left.category, left.unit) != (right.category, right.unit):
            raise BenchmarkLogError(f"benchmark metadata differs for {case!r}")


def comparison_rows(baseline: BenchmarkLog, candidate: BenchmarkLog) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for case in sorted(baseline.cases):
        metadata = baseline.metrics[baseline.run_ids[0]][case]
        baseline_p50 = aggregate(baseline, case, "p50_ns")
        candidate_p50 = aggregate(candidate, case, "p50_ns")
        baseline_p95 = aggregate(baseline, case, "p95_ns")
        candidate_p95 = aggregate(candidate, case, "p95_ns")
        rows.append(
            {
                "case": case,
                "category": metadata.category,
                "unit": metadata.unit,
                "baseline_p50_ns": baseline_p50,
                "candidate_p50_ns": candidate_p50,
                "p50_delta_percent": (candidate_p50 / baseline_p50 - 1.0) * 100.0,
                "baseline_p95_ns": baseline_p95,
                "candidate_p95_ns": candidate_p95,
                "p95_delta_percent": (candidate_p95 / baseline_p95 - 1.0) * 100.0,
            }
        )
    return rows


def print_text(rows: list[dict[str, object]], baseline: BenchmarkLog,
               candidate: BenchmarkLog) -> None:
    print(
        f"host={baseline.host['chip']} macOS={baseline.host['macos']} "
        f"baseline={baseline.runtime['name']} candidate={candidate.runtime['name']}"
    )
    print(
        f"{'case':32} {'unit':18} {'baseline p50':>14} {'candidate p50':>14} "
        f"{'delta':>10} {'p95 delta':>10}"
    )
    for row in rows:
        print(
            f"{row['case']:32} {row['unit']:18} "
            f"{row['baseline_p50_ns']:14.6f} {row['candidate_p50_ns']:14.6f} "
            f"{row['p50_delta_percent']:+9.2f}% {row['p95_delta_percent']:+9.2f}%"
        )


def print_markdown(rows: list[dict[str, object]], baseline: BenchmarkLog,
                   candidate: BenchmarkLog) -> None:
    print(f"Host: `{baseline.host['chip']}` / macOS `{baseline.host['macos']}`")
    print()
    print(f"Baseline: `{baseline.runtime['name']}`")
    print(f"Candidate: `{candidate.runtime['name']}`")
    print()
    print("| Case | Category | Unit | Baseline p50 (ns) | Candidate p50 (ns) | p50 delta | p95 delta |")
    print("|---|---|---:|---:|---:|---:|---:|")
    for row in rows:
        print(
            f"| `{row['case']}` | {row['category']} | {row['unit']} | "
            f"{row['baseline_p50_ns']:.6f} | {row['candidate_p50_ns']:.6f} | "
            f"{row['p50_delta_percent']:+.2f}% | {row['p95_delta_percent']:+.2f}% |"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare repeated Apple Silicon CPU benchmark logs. Lower is better."
    )
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument(
        "--format", choices=("text", "markdown", "json"), default="text",
        help="output format (default: text)",
    )
    parser.add_argument(
        "--fail-above-pct", type=float,
        help="exit 1 when any median p50 regression exceeds this percentage",
    )
    parser.add_argument("--allow-host-mismatch", action="store_true")
    parser.add_argument("--allow-config-mismatch", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.fail_above_pct is not None:
        if not math.isfinite(args.fail_above_pct) or args.fail_above_pct < 0.0:
            print("--fail-above-pct must be finite and non-negative", file=sys.stderr)
            return 2
    try:
        baseline = parse_log(args.baseline)
        candidate = parse_log(args.candidate)
        validate_pair(
            baseline,
            candidate,
            allow_host_mismatch=args.allow_host_mismatch,
            allow_config_mismatch=args.allow_config_mismatch,
        )
    except BenchmarkLogError as error:
        print(f"benchmark comparison failed: {error}", file=sys.stderr)
        return 2

    rows = comparison_rows(baseline, candidate)
    if args.format == "json":
        print(
            json.dumps(
                {
                    "schema": 1,
                    "host": baseline.host,
                    "baseline_runtime": baseline.runtime,
                    "candidate_runtime": candidate.runtime,
                    "results": rows,
                },
                indent=2,
                sort_keys=True,
            )
        )
    elif args.format == "markdown":
        print_markdown(rows, baseline, candidate)
    else:
        print_text(rows, baseline, candidate)

    if args.fail_above_pct is not None:
        regressions = [
            row for row in rows
            if float(row["p50_delta_percent"]) > args.fail_above_pct
        ]
        if regressions:
            print(
                "regression threshold exceeded: "
                + ", ".join(str(row["case"]) for row in regressions),
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
