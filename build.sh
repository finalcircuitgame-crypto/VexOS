#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! command -v make >/dev/null 2>&1; then
    echo "[BUILD] GNU Make is required." >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "[BUILD] Python 3 is required to validate NetLog IPv4 settings." >&2
    exit 1
fi

# QEMU defaults. For a physical LAN, set all four NETLOG_* values:
# NETLOG_LOCAL_IP=192.168.1.50
# NETLOG_NETMASK=255.255.255.0
# NETLOG_GATEWAY=192.168.1.1
# NETLOG_DESTINATION=192.168.1.10
NETLOG_LOCAL_IP="${NETLOG_LOCAL_IP:-10.0.2.15}"
NETLOG_NETMASK="${NETLOG_NETMASK:-255.255.255.0}"
NETLOG_GATEWAY="${NETLOG_GATEWAY:-10.0.2.2}"
NETLOG_DESTINATION="${NETLOG_DESTINATION:-10.0.2.2}"

ipv4_define() {
    python3 - "$1" <<'PY'
import ipaddress
import sys

try:
    address = ipaddress.IPv4Address(sys.argv[1])
except ipaddress.AddressValueError:
    print(f"[BUILD] Invalid IPv4 address: {sys.argv[1]}", file=sys.stderr)
    raise SystemExit(1)

print(f"0x{int(address):08X}u")
PY
}

if ! NETLOG_LOCAL_IPV4="$(ipv4_define "$NETLOG_LOCAL_IP")"; then
    exit 1
fi
if ! NETLOG_NETMASK_IPV4="$(ipv4_define "$NETLOG_NETMASK")"; then
    exit 1
fi
if ! NETLOG_GATEWAY_IPV4="$(ipv4_define "$NETLOG_GATEWAY")"; then
    exit 1
fi
if ! NETLOG_DESTINATION_IPV4="$(ipv4_define "$NETLOG_DESTINATION")"; then
    exit 1
fi

echo "[BUILD] Building VexOS"
echo "[BUILD] NetLog local:       $NETLOG_LOCAL_IP"
echo "[BUILD] NetLog netmask:     $NETLOG_NETMASK"
echo "[BUILD] NetLog gateway:     $NETLOG_GATEWAY"
echo "[BUILD] NetLog destination: $NETLOG_DESTINATION"

make all \
    NETLOG_LOCAL_IPV4="$NETLOG_LOCAL_IPV4" \
    NETLOG_NETMASK_IPV4="$NETLOG_NETMASK_IPV4" \
    NETLOG_GATEWAY_IPV4="$NETLOG_GATEWAY_IPV4" \
    NETLOG_DESTINATION_IPV4="$NETLOG_DESTINATION_IPV4"

# If a repository-local IWAD is present, also build and pack the Doom payload.
if [[ -f "assets/doom.wad" ]]; then
    echo "[BUILD] Building optional Doom payload"
    make doom-storage WAD="assets/doom.wad"
fi

echo
echo "[BUILD] Complete."
echo "[BUILD] Boot image: dist/tiny64.img"
echo "[BUILD] Storage:    dist/storage.img"
