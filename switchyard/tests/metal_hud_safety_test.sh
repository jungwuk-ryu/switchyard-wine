#!/usr/bin/env bash
# shellcheck disable=SC1091,SC2030,SC2031
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SAFETY_HELPER="$ROOT_DIR/switchyard/lib/metal_hud_safety.sh"

# shellcheck source=../lib/metal_hud_safety.sh
source "$SAFETY_HELPER"

(
  export MTL_HUD_ENABLED=1
  export SWITCHYARD_GPTK_PATH=/tmp/gptk
  unset SWITCHYARD_ALLOW_UNSAFE_METAL_HUD
  unset SWITCHYARD_MTL_HUD_SUPPRESSED

  switchyard_configure_metal_hud

  [ "$MTL_HUD_ENABLED" = 0 ]
  [ "$SWITCHYARD_MTL_HUD_SUPPRESSED" = 1 ]
)

(
  export MTL_HUD_ENABLED=1
  unset SWITCHYARD_GPTK_PATH
  unset SWITCHYARD_ALLOW_UNSAFE_METAL_HUD
  unset SWITCHYARD_MTL_HUD_SUPPRESSED

  switchyard_configure_metal_hud

  [ "$MTL_HUD_ENABLED" = 1 ]
  [ -z "${SWITCHYARD_MTL_HUD_SUPPRESSED:-}" ]
)

(
  export MTL_HUD_ENABLED=1
  export SWITCHYARD_GPTK_PATH=/tmp/gptk
  export SWITCHYARD_ALLOW_UNSAFE_METAL_HUD=1
  unset SWITCHYARD_MTL_HUD_SUPPRESSED

  switchyard_configure_metal_hud

  [ "$MTL_HUD_ENABLED" = 1 ]
  [ -z "${SWITCHYARD_MTL_HUD_SUPPRESSED:-}" ]
)

(
  export MTL_HUD_ENABLED=0
  export SWITCHYARD_GPTK_PATH=/tmp/gptk
  unset SWITCHYARD_ALLOW_UNSAFE_METAL_HUD
  unset SWITCHYARD_MTL_HUD_SUPPRESSED

  switchyard_configure_metal_hud

  [ "$MTL_HUD_ENABLED" = 0 ]
  [ -z "${SWITCHYARD_MTL_HUD_SUPPRESSED:-}" ]
)

echo "Metal HUD safety tests passed"
