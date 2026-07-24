#!/usr/bin/env bash

# Apple's Metal HUD can fault on a native GCD worker while decoding its font
# atlas under Rosetta.  Wine cannot recover that closed-source worker and the
# fault otherwise turns into a recursive exception inside the process-wide Wine
# signal handler.  Keep GPTK application launches safe unless a developer
# explicitly opts back in for HUD diagnostics.
switchyard_configure_metal_hud() {
  case "${MTL_HUD_ENABLED:-0}" in
    1|true|TRUE|yes|YES|on|ON) ;;
    *) return 0 ;;
  esac

  [ -n "${SWITCHYARD_GPTK_PATH:-}" ] || return 0

  case "${SWITCHYARD_ALLOW_UNSAFE_METAL_HUD:-0}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
  esac

  export MTL_HUD_ENABLED=0
  export SWITCHYARD_MTL_HUD_SUPPRESSED=1
}
