#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -I \
    "$ROOT_DIR/switchyard/tests/swdbg_experiment_test.py"
