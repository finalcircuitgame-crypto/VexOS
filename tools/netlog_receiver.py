#!/usr/bin/env python3
"""VexOS NetLog UDP receiver. Listens on UDP/5140 and prints incoming log text."""
import argparse
import datetime as dt
import socket
import sys

def main():
    parser = argparse.ArgumentParser(description="Receive VexOS NetLog UDP messages")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    parser.add_argument("--port", type=int, default=5140, help="UDP port (default: 5140)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    print(f"[NetLog] Listening on {args.host}:{args.port} (Ctrl+C to stop)", flush=True)
    try:
        while True:
            data, peer = sock.recvfrom(65535)
            stamp = dt.datetime.now().astimezone().strftime("%H:%M:%S.%f")[:-3]
            text = data.decode("utf-8", errors="replace")
            sys.stdout.write(f"[{stamp}] [{peer[0]}:{peer[1]}] {text}")
            if not text.endswith("\n"):
                sys.stdout.write("\n")
            sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n[NetLog] Stopped.")
    finally:
        sock.close()

if __name__ == "__main__":
    main()
