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

cd "$ROOT/receiver"
exec "$ROOT/build/mpquic_receiver" \
  --uu-bind "$UU_RECEIVER_IP" \
  --uu-port "$UU_RECEIVER_PORT" \
  --pc5-local "$PC5_RECEIVER_IP" \
  --pc5-peer "$PC5_SENDER_IP" \
  --exp "${EXP_NAME:-rr_reference}"
