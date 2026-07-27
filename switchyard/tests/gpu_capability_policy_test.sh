#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d /tmp/switchyard-gpu-policy.XXXXXX)"

cleanup() {
  /bin/rm -rf "$work"
}
trap cleanup EXIT

# shellcheck source=/dev/null
source "$ROOT_DIR/switchyard/lib/gpu_capability_policy.sh"

good_helper="$work/good-helper"
cat >"$good_helper" <<'EOF'
#!/usr/bin/env bash
printf '0000106b\t000003f2\t00000000\t00000000\tApple Test GPU\n'
EOF
chmod 0755 "$good_helper"

bad_helper="$work/bad-helper"
cat >"$bad_helper" <<'EOF'
#!/usr/bin/env bash
printf 'not-a-gpu\n'
EOF
chmod 0755 "$bad_helper"

multi_line_helper="$work/multi-line-helper"
cat >"$multi_line_helper" <<'EOF'
#!/usr/bin/env bash
printf '0000106b\t000003f2\t00000000\t00000000\tApple Test GPU\nunexpected\n'
EOF
chmod 0755 "$multi_line_helper"

clear_gpu_environment() {
  unset SWITCHYARD_GPTK_PATH
  unset SWITCHYARD_GPU_BACKEND
  unset SWITCHYARD_GPU_AMD_ADL
  unset SWITCHYARD_GPU_AMD_AGS_EXTENSIONS
  unset SWITCHYARD_GPU_AMD_UMD
  unset SWITCHYARD_GPU_IDENTITY_MODE
  unset SWITCHYARD_GPU_HOST_VENDOR_ID
  unset SWITCHYARD_GPU_HOST_DEVICE_ID
  unset SWITCHYARD_GPU_HOST_SUBSYSTEM_ID
  unset SWITCHYARD_GPU_HOST_REVISION_ID
  unset SWITCHYARD_GPU_HOST_DESCRIPTION
  unset SWITCHYARD_GPU_REPORTED_VENDOR_ID
  unset SWITCHYARD_GPU_REPORTED_DEVICE_ID
  unset SWITCHYARD_GPU_REPORTED_SUBSYSTEM_ID
  unset SWITCHYARD_GPU_REPORTED_REVISION_ID
  unset SWITCHYARD_GPU_REPORTED_DESCRIPTION
  unset D3DM_VENDOR_ID
  unset D3DM_DEVICE_ID
  unset D3DM_DEVICE_SUBSYS
  unset D3DM_DEVICE_REVISION
  unset D3DM_DEVICE_DESCRIPTION
}

(
  clear_gpu_environment
  SWITCHYARD_GPU_INFO_HELPER="$good_helper"
  switchyard_configure_d3dmetal_gpu "$work"
  [ -z "${D3DM_VENDOR_ID:-}" ]
  [ -z "${SWITCHYARD_GPU_BACKEND:-}" ]
)

(
  clear_gpu_environment
  export SWITCHYARD_GPTK_PATH=/test/gptk
  export SWITCHYARD_GPU_INFO_HELPER="$good_helper"
  switchyard_configure_d3dmetal_gpu "$work"

  [ "$SWITCHYARD_GPU_BACKEND" = d3dmetal ]
  [ "$SWITCHYARD_GPU_AMD_ADL" = 0 ]
  [ "$SWITCHYARD_GPU_AMD_AGS_EXTENSIONS" = 0 ]
  [ "$SWITCHYARD_GPU_AMD_UMD" = 0 ]
  [ "$SWITCHYARD_GPU_HOST_VENDOR_ID" = 0x0000106b ]
  [ "$SWITCHYARD_GPU_HOST_DEVICE_ID" = 0x000003f2 ]
  [ "$SWITCHYARD_GPU_IDENTITY_MODE" = provider-default ]
  [ -z "${D3DM_VENDOR_ID:-}" ]
  [ -z "${D3DM_DEVICE_ID:-}" ]
  [ -z "${D3DM_DEVICE_DESCRIPTION:-}" ]
  [ -z "${SWITCHYARD_GPU_REPORTED_VENDOR_ID:-}" ]
  [ -z "${SWITCHYARD_GPU_REPORTED_DESCRIPTION:-}" ]
)

(
  clear_gpu_environment
  export SWITCHYARD_GPTK_PATH=/test/gptk
  export SWITCHYARD_GPU_INFO_HELPER="$good_helper"
  export D3DM_VENDOR_ID=0x1234
  export D3DM_DEVICE_ID=0x5678
  export D3DM_DEVICE_SUBSYS=0x9abc
  export D3DM_DEVICE_REVISION=0x01
  export D3DM_DEVICE_DESCRIPTION="Explicit Test GPU"
  switchyard_configure_d3dmetal_gpu "$work"

  [ "$SWITCHYARD_GPU_HOST_VENDOR_ID" = 0x0000106b ]
  [ "$SWITCHYARD_GPU_IDENTITY_MODE" = explicit ]
  [ "$SWITCHYARD_GPU_REPORTED_VENDOR_ID" = 0x1234 ]
  [ "$SWITCHYARD_GPU_REPORTED_DEVICE_ID" = 0x5678 ]
  [ "$SWITCHYARD_GPU_REPORTED_DESCRIPTION" = "Explicit Test GPU" ]
)

if (
  clear_gpu_environment
  export SWITCHYARD_GPTK_PATH=/test/gptk
  export SWITCHYARD_GPU_INFO_HELPER="$good_helper"
  export D3DM_VENDOR_ID=0x1234
  switchyard_configure_d3dmetal_gpu "$work"
); then
  echo "A partial D3DMetal identity override unexpectedly succeeded." >&2
  exit 1
fi

if (
  clear_gpu_environment
  export SWITCHYARD_GPTK_PATH=/test/gptk
  export SWITCHYARD_GPU_INFO_HELPER="$bad_helper"
  switchyard_configure_d3dmetal_gpu "$work"
); then
  echo "An invalid host GPU identity unexpectedly succeeded." >&2
  exit 1
fi

if (
  clear_gpu_environment
  export SWITCHYARD_GPTK_PATH=/test/gptk
  export SWITCHYARD_GPU_INFO_HELPER="$multi_line_helper"
  switchyard_configure_d3dmetal_gpu "$work"
); then
  echo "A multi-line host GPU identity unexpectedly succeeded." >&2
  exit 1
fi

if (
  clear_gpu_environment
  export SWITCHYARD_GPTK_PATH=/test/gptk
  export SWITCHYARD_GPU_INFO_HELPER="$good_helper"
  export D3DM_VENDOR_ID=4294967296
  export D3DM_DEVICE_ID=0x5678
  export D3DM_DEVICE_SUBSYS=0
  export D3DM_DEVICE_REVISION=0
  export D3DM_DEVICE_DESCRIPTION="Invalid Test GPU"
  switchyard_configure_d3dmetal_gpu "$work"
); then
  echo "An out-of-range complete D3DMetal identity override unexpectedly succeeded." >&2
  exit 1
fi

echo "[test] graphics capability policy passed"
