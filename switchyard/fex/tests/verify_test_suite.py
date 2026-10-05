#!/usr/bin/env python3
"""Reject an incomplete or skipped current-source FEX build gate."""
import json
import sys
import xml.etree.ElementTree as ET

EXPECTED = frozenset((
    "switchyard-fex-darwin-abi", "switchyard-fex-wine-signal",
    "switchyard-fex-wine-signal-public", "switchyard-fex-darwin-mode-recovery",
    "switchyard-fex-warm-entry", "switchyard-fex-detached-dispatch",
    "switchyard-fex-public-detached-dispatch", "switchyard-fex-vector-state",
    "switchyard-fex-admission", "switchyard-fex-shared-admission",
    "switchyard-fex-allocator", "switchyard-fex-allocator-fallback",
    "switchyard-fex-c-api",
    "switchyard-fex-register-window", "switchyard-fex-register-window-state",
))
MAX_REPORT = 4 * 1024 * 1024


def check_names(names):
    if len(names) != len(EXPECTED) or frozenset(names) != EXPECTED:
        raise ValueError("FEX gate must contain exactly the 15 current tests")


def check_plan(data):
    plan = json.loads(data)
    if type(plan) is not dict or type(plan.get("tests")) is not list:
        raise ValueError("malformed CTest plan")
    check_names([test["name"] for test in plan["tests"]])


def check_results(data):
    report = ET.fromstring(data)
    if report.tag != "testsuite":
        raise ValueError("unexpected CTest report root")
    cases = report.findall("testcase")
    check_names([case.get("name") for case in cases])
    if any(case.get("status") != "run" or case.find("skipped") is not None or
           case.find("failure") is not None or case.find("error") is not None
           for case in cases):
        raise ValueError("FEX gate contains failed, skipped or incomplete tests")
    if report.get("tests") != str(len(EXPECTED)) or any(
            report.get(field) != "0" for field in ("failures", "disabled", "skipped")):
        raise ValueError("CTest totals do not prove a complete FEX gate")


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("plan", "results"):
        raise ValueError("usage: verify_test_suite.py plan|results REPORT")
    with open(sys.argv[2], "rb") as stream:
        data = stream.read(MAX_REPORT + 1)
    if len(data) > MAX_REPORT:
        raise ValueError("CTest report exceeds the gate bound")
    if sys.argv[1] == "plan":
        check_plan(data)
    else:
        check_results(data)
    print("FEX " + sys.argv[1] + ": 15 exact tests, no skipped gate")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, ET.ParseError) as error:
        raise SystemExit("FEX test gate: " + str(error)) from error
