#!/usr/bin/env bash

switchyard_d3dm_u32_is_valid() {
  local value="$1"
  local digits

  case "$value" in
    0x*|0X*)
      digits="${value:2}"
      [ -n "$digits" ] || return 1
      case "$digits" in *[!0123456789abcdefABCDEF]*) return 1 ;; esac
      [ "${#digits}" -le 8 ]
      ;;
    *)
      [ -n "$value" ] || return 1
      case "$value" in *[!0123456789]*) return 1 ;; esac
      [ "${#value}" -le 10 ] || return 1
      [ "${#value}" -lt 10 ] || [ "$value" -le 4294967295 ]
      ;;
  esac
}

# Configure a coherent process-wide graphics capability description before
# Wine or any selected graphics-provider module is loaded.
switchyard_configure_d3dmetal_gpu() {
  local runtime_dir="$1"
  local helper="${SWITCHYARD_GPU_INFO_HELPER:-$runtime_dir/libexec/switchyard-host-gpu-info}"
  local identity vendor_id device_id subsystem_id revision_id description extra
  local identity_override_count=0

  [ -n "${SWITCHYARD_GPTK_PATH:-}" ] || return 0

  export SWITCHYARD_GPU_BACKEND="d3dmetal"
  export SWITCHYARD_GPU_AMD_ADL="0"
  export SWITCHYARD_GPU_AMD_AGS_EXTENSIONS="0"
  export SWITCHYARD_GPU_AMD_UMD="0"

  if [ ! -x "$helper" ]; then
    echo "Switchyard runtime is missing its D3DMetal GPU identity helper: $helper" >&2
    return 127
  fi
  if ! identity="$("$helper")"; then
    echo "Switchyard could not determine the host GPU identity for D3DMetal." >&2
    return 1
  fi
  case "$identity" in
    *$'\r'*|*$'\n'*)
      echo "Switchyard received a multi-line host GPU identity." >&2
      return 1
      ;;
  esac

  IFS=$'\t' read -r vendor_id device_id subsystem_id revision_id description extra <<<"$identity"
  if [ -n "${extra:-}" ] ||
     [ -z "$vendor_id" ] || [ -z "$device_id" ] ||
     [ -z "$subsystem_id" ] || [ -z "$revision_id" ] ||
     [ -z "$description" ]; then
    echo "Switchyard received an incomplete host GPU identity." >&2
    return 1
  fi
  case "$vendor_id$device_id$subsystem_id$revision_id" in
    *[!0123456789abcdefABCDEF]*)
      echo "Switchyard received an invalid host GPU identity." >&2
      return 1
      ;;
  esac
  if [ "${#vendor_id}" -gt 8 ] || [ "${#device_id}" -gt 8 ] ||
     [ "${#subsystem_id}" -gt 8 ] || [ "${#revision_id}" -gt 8 ]; then
    echo "Switchyard received an out-of-range host GPU identity." >&2
    return 1
  fi
  if [ -z "${vendor_id//0/}" ] || [ -z "${device_id//0/}" ]; then
    echo "Switchyard received a zero host GPU vendor or device identifier." >&2
    return 1
  fi

  export SWITCHYARD_GPU_HOST_VENDOR_ID="0x$vendor_id"
  export SWITCHYARD_GPU_HOST_DEVICE_ID="0x$device_id"
  export SWITCHYARD_GPU_HOST_SUBSYSTEM_ID="0x$subsystem_id"
  export SWITCHYARD_GPU_HOST_REVISION_ID="0x$revision_id"
  export SWITCHYARD_GPU_HOST_DESCRIPTION="$description"

  [ -n "${D3DM_VENDOR_ID:-}" ] && identity_override_count=$((identity_override_count + 1))
  [ -n "${D3DM_DEVICE_ID:-}" ] && identity_override_count=$((identity_override_count + 1))
  [ -n "${D3DM_DEVICE_SUBSYS:-}" ] && identity_override_count=$((identity_override_count + 1))
  [ -n "${D3DM_DEVICE_REVISION:-}" ] && identity_override_count=$((identity_override_count + 1))
  [ -n "${D3DM_DEVICE_DESCRIPTION:-}" ] && identity_override_count=$((identity_override_count + 1))

  if [ "$identity_override_count" -ne 0 ] && [ "$identity_override_count" -ne 5 ]; then
    echo "D3DMetal GPU identity overrides must set all five D3DM_* identity variables." >&2
    return 2
  fi

  if [ "$identity_override_count" -eq 5 ]; then
    if ! switchyard_d3dm_u32_is_valid "$D3DM_VENDOR_ID" ||
       ! switchyard_d3dm_u32_is_valid "$D3DM_DEVICE_ID" ||
       ! switchyard_d3dm_u32_is_valid "$D3DM_DEVICE_SUBSYS" ||
       ! switchyard_d3dm_u32_is_valid "$D3DM_DEVICE_REVISION"; then
      echo "D3DMetal GPU identity overrides contain an invalid 32-bit identifier." >&2
      return 2
    fi
    case "$D3DM_DEVICE_DESCRIPTION" in
      *$'\t'*|*$'\r'*|*$'\n'*)
        echo "D3DMetal GPU descriptions cannot contain tabs or newlines." >&2
        return 2
        ;;
    esac
    export SWITCHYARD_GPU_IDENTITY_MODE="explicit"
    export SWITCHYARD_GPU_REPORTED_VENDOR_ID="$D3DM_VENDOR_ID"
    export SWITCHYARD_GPU_REPORTED_DEVICE_ID="$D3DM_DEVICE_ID"
    export SWITCHYARD_GPU_REPORTED_SUBSYSTEM_ID="$D3DM_DEVICE_SUBSYS"
    export SWITCHYARD_GPU_REPORTED_REVISION_ID="$D3DM_DEVICE_REVISION"
    export SWITCHYARD_GPU_REPORTED_DESCRIPTION="$D3DM_DEVICE_DESCRIPTION"
  else
    # The provider owns the application-facing identity unless the user
    # supplies a complete diagnostic override.  In particular, D3DMetal's
    # compatibility identity is part of its application contract; replacing
    # it with the physical Apple identity can select unsupported vendor-private
    # paths in applications even though the backend capabilities are unchanged.
    unset D3DM_VENDOR_ID D3DM_DEVICE_ID D3DM_DEVICE_SUBSYS
    unset D3DM_DEVICE_REVISION D3DM_DEVICE_DESCRIPTION
    unset SWITCHYARD_GPU_REPORTED_VENDOR_ID
    unset SWITCHYARD_GPU_REPORTED_DEVICE_ID
    unset SWITCHYARD_GPU_REPORTED_SUBSYSTEM_ID
    unset SWITCHYARD_GPU_REPORTED_REVISION_ID
    unset SWITCHYARD_GPU_REPORTED_DESCRIPTION
    export SWITCHYARD_GPU_IDENTITY_MODE="provider-default"
  fi
}
