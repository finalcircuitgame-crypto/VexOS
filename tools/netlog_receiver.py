#!/usr/bin/env python3
"""VexOS NetLog UDP receiver with a clear idle state and optional file logging."""

from __future__ import annotations

import argparse
import datetime as dt
import socket
import sys
import time
from pathlib import Path


def timestamp() -> str:
    return dt.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def positive_port(value: str) -> int:
    try:
        port = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("port must be a number") from exc
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Receive VexOS development logs over UDP.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="local address to listen on; 0.0.0.0 listens on all network adapters",
    )
    parser.add_argument("--port", type=positive_port, default=5140, help="UDP listen port")
    parser.add_argument(
        "--output",
        type=Path,
        help="also append received log packets to this file",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="send one local test datagram after binding (does not test VexOS or the LAN)",
    )
    parser.add_argument(
        "--quiet-status",
        action="store_true",
        help="do not print periodic waiting/packet-count status messages",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output = None

    if args.output:
        try:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            output = args.output.open("a", encoding="utf-8", buffering=1)
        except OSError as exc:
            print(f"[NetLog] Cannot open output file {args.output}: {exc}", file=sys.stderr)
            return 2

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((args.host, args.port))
        sock.settimeout(1.0)
    except OSError as exc:
        sock.close()
        if output:
            output.close()
        print(
            f"[NetLog] Could not listen on {args.host}:{args.port}/UDP: {exc}",
            file=sys.stderr,
        )
        print(
            "[NetLog] Check whether another receiver is already using this port, "
            "and verify the bind address.",
            file=sys.stderr,
        )
        return 2

    start_time = time.monotonic()
    last_status = start_time
    packet_count = 0
    byte_count = 0
    first_packet = False

    print()
    print("╭──────────────────────────────────────────────────────────╮")
    print("│                    VEXOS NETLOG                           │")
    print("╰──────────────────────────────────────────────────────────╯")
    print(f"[LISTENING] {args.host}:{args.port} / UDP")
    print("[WAITING]  Waiting for VexOS packets. Silence means no UDP log")
    print("           datagrams have reached this PC yet.")
    print("[CHECK]    Start this receiver BEFORE booting VexOS.")
    print("           Physical builds must target this PC's LAN IP.")
    if args.output:
        print(f"[FILE]     Appending received packets to: {args.output}")
    if args.self_test:
        destination = args.host if args.host not in ("0.0.0.0", "") else "127.0.0.1"
        try:
            sock.sendto(
                b"[NETLOG RECEIVER SELF-TEST] Local UDP receive path is working.\n",
                (destination, args.port),
            )
            print("[TEST]     Sent a local test datagram. This checks the receiver only.")
        except OSError as exc:
            print(f"[TEST]     Local test could not be sent: {exc}")
    print("[EXIT]     Press Ctrl+C to stop.")
    print()

    try:
        while True:
            try:
                data, peer = sock.recvfrom(65535)
            except socket.timeout:
                now = time.monotonic()
                if not args.quiet_status and now - last_status >= 10.0:
                    elapsed = int(now - start_time)
                    if not first_packet:
                        print(
                            f"[STILL WAITING] {elapsed}s elapsed; 0 packets received. "
                            "Check VexOS NETLOG_DESTINATION and the Windows firewall."
                        )
                    else:
                        print(
                            f"[STATUS] {packet_count:,} packets, {byte_count:,} bytes received."
                        )
                    last_status = now
                continue

            packet_count += 1
            byte_count += len(data)
            first_packet = True
            stamp = timestamp()
            decoded = data.decode("utf-8", errors="replace")
            heading = (
                f"[{stamp}] [PACKET {packet_count:06d}] "
                f"{peer[0]}:{peer[1]}  {len(data):,} bytes"
            )

            print(heading, flush=True)
            if decoded:
                sys.stdout.write(decoded)
                if not decoded.endswith("\n"):
                    sys.stdout.write("\n")
            else:
                print("[empty datagram]")
            sys.stdout.flush()

            if output:
                output.write(heading + "\n")
                output.write(decoded)
                if not decoded.endswith("\n"):
                    output.write("\n")
                output.flush()

    except KeyboardInterrupt:
        elapsed = max(0.001, time.monotonic() - start_time)
        print()
        print("[STOPPED] NetLog receiver stopped.")
        print(
            f"[SUMMARY] {packet_count:,} packets • {byte_count:,} bytes • "
            f"{elapsed:.1f}s • {packet_count / elapsed:.1f} packets/s"
        )
    finally:
        sock.close()
        if output:
            output.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
