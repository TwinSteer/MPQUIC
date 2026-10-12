#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT/certs"
openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
  -keyout "$ROOT/certs/key.pem" \
  -out "$ROOT/certs/cert.pem" \
  -days 3650 \
  -subj "/CN=TwinSteer-MPQUIC"
chmod 600 "$ROOT/certs/key.pem"
echo "Generated $ROOT/certs/cert.pem and key.pem"
