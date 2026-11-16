#!/usr/bin/env python3
"""Patch DoomGeneric's tic-command builder to reconcile held arrow keys."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys


OLD = """void G_BuildTiccmd (ticcmd_t* cmd, int maketic) 
{ 
"""

NEW = """extern void VEXOS_DoomSyncArrowKeys(unsigned int *keys,
                                     int key_right, int key_left,
                                     int key_down, int key_up);

void G_BuildTiccmd (ticcmd_t* cmd, int maketic) 
{ 
    /*
     * Treat the kernel's live HID state as authoritative for movement keys.
     * This prevents a lost queued key-up from leaving gamekeydown latched.
     */
    VEXOS_DoomSyncArrowKeys(gamekeydown, key_right, key_left,
                            key_down, key_up);
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        source = args.source.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"patch_doom_game_input: cannot read {args.source}: {exc}", file=sys.stderr)
        return 1

    matches = source.count(OLD)
    if matches != 1:
        print(
            "patch_doom_game_input: expected exactly one G_BuildTiccmd entry; "
            f"found {matches}. Refusing to patch an unknown source version.",
            file=sys.stderr,
        )
        return 1

    patched = source.replace(OLD, NEW, 1)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        args.output.write_text(patched, encoding="utf-8")
    except OSError as exc:
        print(f"patch_doom_game_input: cannot write {args.output}: {exc}", file=sys.stderr)
        return 1

    print("[DOOM] Patched ticcmd builder: reconcile live arrow-key state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
