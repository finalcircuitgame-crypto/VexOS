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
extern void VEXOS_DoomTraceTiccmd(int maketic, int angleturn,
                                  int forwardmove, int sidemove);

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

    carry_line = "        carry = desired_angleturn - cmd->angleturn;"
    if source.count(carry_line) != 1:
        print(
            "patch_doom_game_input: expected exactly one low-res turn carry line; "
            "refusing to inject command tracing at an unknown location.",
            file=sys.stderr,
        )
        return 1

    patched = source.replace(OLD, NEW, 1)
    carry_pos = patched.index(carry_line)
    function_end = patched.find("\n}", carry_pos)
    if function_end < 0:
        print(
            "patch_doom_game_input: could not locate G_BuildTiccmd closing brace.",
            file=sys.stderr,
        )
        return 1

    trace_call = (
        "\n    VEXOS_DoomTraceTiccmd(maketic, cmd->angleturn, "
        "cmd->forwardmove, cmd->sidemove);"
    )
    patched = patched[:function_end] + trace_call + patched[function_end:]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        args.output.write_text(patched, encoding="utf-8")
    except OSError as exc:
        print(f"patch_doom_game_input: cannot write {args.output}: {exc}", file=sys.stderr)
        return 1

    print("[DOOM] Patched ticcmd builder: reconcile keys and trace final movement commands")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
