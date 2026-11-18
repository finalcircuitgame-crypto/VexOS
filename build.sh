#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! command -v make >/dev/null 2>&1; then
    echo "[BUILD] GNU Make is required." >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "[BUILD] Python 3 is required to validate the NetLog destination." >&2
    exit 1
fi

# QEMU's default host endpoint. Override for a receiver on another machine:
# NETLOG_DESTINATION=192.168.1.50 ./build.sh
NETLOG_DESTINATION="${NETLOG_DESTINATION:-10.0.2.2}"
if ! NETLOG_DESTINATION_IPV4=$(python3 -c 'import ipaddress, sys; print("0x%08Xu" % int(ipaddress.IPv4Address(sys.argv[1])))' "$NETLOG_DESTINATION"); then
    echo "[BUILD] Invalid NETLOG_DESTINATION IPv4 address: $NETLOG_DESTINATION" >&2
    exit 1
fi

echo "[BUILD] Building VexOS (NetLog destination: $NETLOG_DESTINATION)"
make all NETLOG_DESTINATION_IPV4="$NETLOG_DESTINATION_IPV4"

# If a repository-local IWAD is present, also build and pack the Doom payload.
if [[ -f "assets/doom.wad" ]]; then
    echo "[BUILD] Building optional Doom payload"
    make doom-storage WAD="assets/doom.wad"
fi

echo
echo "[BUILD] Complete."
echo "[BUILD] Boot image: dist/tiny64.img"
echo "[BUILD] Storage:    dist/storage.img"
