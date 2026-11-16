#!/usr/bin/env python3
"""Instrument Doom's player thinker to report actual momentum and view angle."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ANCHOR = """void P_PlayerThink (player_t* player)
{
"""
CALL = """    VEXOS_DoomTracePlayerState(leveltime, cmd->angleturn,
                               cmd->forwardmove, cmd->sidemove,
                               player->mo->momx, player->mo->momy,
                               (unsigned int)player->mo->angle);
"""

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        source = args.source.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"patch_doom_player_state: cannot read {args.source}: {exc}", file=sys.stderr)
        return 1

    if source.count(ANCHOR) != 1:
        print("patch_doom_player_state: expected exactly one P_PlayerThink entry; refusing unknown source", file=sys.stderr)
        return 1
    if "VEXOS_DoomTracePlayerState" in source:
        print("patch_doom_player_state: source already instrumented", file=sys.stderr)
        return 1

    # P_PlayerThink is the final function in this upstream source file.
    end = source.rstrip().rfind("\\n}")
    if end < 0:
        print("patch_doom_player_state: could not locate final function brace", file=sys.stderr)
        return 1

    patched = source[:end] + CALL + source[end:]
    patched = patched.replace(ANCHOR, """extern void VEXOS_DoomTracePlayerState(int leveltime, int angleturn,
                                      int forwardmove, int sidemove,
                                      int momx, int momy, unsigned int angle);

""" + ANCHOR, 1)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        args.output.write_text(patched, encoding="utf-8")
    except OSError as exc:
        print(f"patch_doom_player_state: cannot write {args.output}: {exc}", file=sys.stderr)
        return 1
    print("[DOOM] Patched P_PlayerThink: trace final commands, momentum, and view angle")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
