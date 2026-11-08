#!/usr/bin/env bash
# Static, dependency-free builds for the Maryati Bridge portable proxy.
set -euo pipefail

cd "$(dirname "$0")"
OUT_DIR="prebuilt"
mkdir -p "$OUT_DIR"

export CGO_ENABLED=0
LDFLAGS="-s -w -buildid="

GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "$LDFLAGS" -o "$OUT_DIR/proxy_win.exe" .
GOOS=linux GOARCH=amd64 go build -trimpath -ldflags "$LDFLAGS" -o "$OUT_DIR/proxy_linux" .

(
    cd "$OUT_DIR"
    sha256sum proxy_win.exe proxy_linux > SHA256SUMS
)

echo "built: $(pwd)/$OUT_DIR/proxy_win.exe"
echo "built: $(pwd)/$OUT_DIR/proxy_linux"
echo "checksums: $(pwd)/$OUT_DIR/SHA256SUMS"
