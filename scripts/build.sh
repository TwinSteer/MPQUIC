#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT/configs/local.env}"

if [[ -f "$CONFIG" ]]; then
  # shellcheck disable=SC1090
  source "$CONFIG"
fi

: "${SIRIUS_SDK_DIR:?Set SIRIUS_SDK_DIR or copy configs/example.env to configs/local.env}"

cmake -S "$ROOT" -B "$ROOT/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSIRIUS_SDK_DIR="$SIRIUS_SDK_DIR"
cmake --build "$ROOT/build" -j"$(nproc)"

echo "Built:"
echo "  $ROOT/build/mpquic_sender"
echo "  $ROOT/build/mpquic_receiver"
echo "  $ROOT/build/pc5_beacon"
