#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT/configs/local.env}"

if [[ ! -f "$CONFIG" ]]; then
  echo "Missing config: $CONFIG" >&2
  echo "Copy configs/example.env to configs/local.env and edit it." >&2
  exit 1
fi
# shellcheck disable=SC1090
source "$CONFIG"

cd "$ROOT/sender"
exec "$ROOT/build/mpquic_sender" \
  --uu-peer "$UU_RECEIVER_IP" \
  --uu-port "$UU_RECEIVER_PORT" \
  --pc5-local "$PC5_SENDER_IP" \
  --pc5-peer "$PC5_RECEIVER_IP" \
  --gps-host "$GPS_HOST" \
  --gps-port "$GPS_PORT" \
  --camera "$CAMERA_INDEX" \
  --exp "${EXP_NAME:-rr_reference}"
