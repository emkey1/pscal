#!/usr/bin/env bash
set -euo pipefail

# Compute a libcurl-compatible SPKI pin (sha256//BASE64) from a live host or a PEM file.
#
# Usage:
#   tools/pin-from-host.sh example.com[:443] [--sni name]
#   tools/pin-from-host.sh --pem cert.pem
#
# Prints the pin as: sha256//BASE64
#
# Requirements: openssl, base64

usage() {
  cat <<EOF
Usage:
  $0 host[:port] [--sni name]
  $0 --pem cert.pem

Examples:
  $0 example.com
  $0 example.com:8443 --sni service.example.com
  $0 --pem /path/to/cert.pem
EOF
}

if ! command -v openssl >/dev/null 2>&1; then
  echo "Error: openssl not found in PATH" >&2
  exit 1
fi

if ! command -v base64 >/dev/null 2>&1; then
  echo "Error: base64 not found in PATH" >&2
  exit 1
fi

HOSTPORT=""
SNI_OVERRIDE=""
PEM_FILE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sni)
      shift
      SNI_OVERRIDE=${1:-}
      ;;
    --pem)
      shift
      PEM_FILE=${1:-}
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      if [[ -z "$HOSTPORT" ]]; then
        HOSTPORT="$1"
      else
        echo "Unexpected argument: $1" >&2
        usage
        exit 1
      fi
      ;;
  esac
  shift || true
done

compute_from_stream() {
  # Reads cert/public key data from stdin and emits sha256//BASE64
  openssl x509 -pubkey -noout 2>/dev/null \
    | openssl pkey -pubin -outform der 2>/dev/null \
    | openssl dgst -sha256 -binary 2>/dev/null \
    | base64
}

if [[ -n "$PEM_FILE" ]]; then
  if [[ ! -f "$PEM_FILE" ]]; then
    echo "Error: PEM file not found: $PEM_FILE" >&2
    exit 1
  fi
  PIN_BASE64=$(openssl x509 -in "$PEM_FILE" -pubkey -noout \
    | openssl pkey -pubin -outform der \
    | openssl dgst -sha256 -binary \
    | base64)
  echo "sha256//$PIN_BASE64"
  exit 0
fi

if [[ -z "$HOSTPORT" ]]; then
  echo "Error: host[:port] or --pem is required" >&2
  usage
  exit 1
fi

HOST=${HOSTPORT%%:*}
PORT=${HOSTPORT##*:}
if [[ "$HOST" == "$PORT" ]]; then
  PORT=443
fi
SNI=${SNI_OVERRIDE:-$HOST}

# Fetch leaf certificate from remote and compute pin
PIN_BASE64=$(openssl s_client -connect "$HOST:$PORT" -servername "$SNI" </dev/null 2>/dev/null \
  | compute_from_stream)

if [[ -z "$PIN_BASE64" ]]; then
  echo "Error: failed to compute pin (did the TLS handshake succeed?)" >&2
  exit 1
fi

echo "sha256//$PIN_BASE64"

