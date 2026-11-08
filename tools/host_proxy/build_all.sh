#!/usr/bin/env bash
# Static, dependency-free builds for the Maryati Bridge portable proxy.
set -euo pipefail

cd "$(dirname "$0")"

export CGO_ENABLED=0
LDFLAGS="-s -w -buildid="

GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "$LDFLAGS" -o proxy_win.exe .
GOOS=linux GOARCH=amd64 go build -trimpath -ldflags "$LDFLAGS" -o proxy_linux .

echo "built: $(pwd)/proxy_win.exe"
echo "built: $(pwd)/proxy_linux"
