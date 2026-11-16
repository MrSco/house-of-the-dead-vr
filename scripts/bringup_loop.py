#!/usr/bin/env python3
"""Bring-up loop for a new set: run m2run, and while it stops on a jump to code
with no recompiled block ("no recompiled code at X"), add X to the set's seeds
(the i960's seeds/<set>.txt, the sound 68000's seeds/<set>_snd.txt), recompile
and run again.

  bringup_loop.py --set SET [--frames N] [--rounds R] [--scan]

--scan runs seed_scan.py --append first. Each round's m2run output is in
build_<game>/bringup/round<N>.txt. Stops at any other stop (a fault, a TGP
error), printing it, or when the run reaches its frames.
"""

import argparse
import os
import re
import subprocess
import sys

import games

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STOP = re.compile(r"(sound 68000: )?no recompiled code at ([0-9a-fA-F]+)")


def run(cmd, log=None):
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if log:
        with open(log, "w") as f:
            f.write(out.stdout + out.stderr)
    return out.returncode, out.stdout + out.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", required=True)
    ap.add_argument("--frames", type=int, default=1800)
    ap.add_argument("--rounds", type=int, default=40)
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--inputs")
    args = ap.parse_args()
    game = games.game_of(args.set)
    exe = os.path.join(ROOT, games.pc_dir(args.set), "Release", "m2run.exe" if os.name == "nt" else "m2run")
    if not os.path.exists(exe):
        exe = os.path.join(ROOT, games.pc_dir(args.set), "m2run")
    logs = os.path.join(ROOT, games.game_dir(game), "bringup")
    os.makedirs(logs, exist_ok=True)
    if args.scan:
        code, out = run([sys.executable, "scripts/seed_scan.py", "--set", args.set, "--append"])
        print(out[-600:])
        if code:
            sys.exit("bringup: seed_scan failed")
        code, out = run([sys.executable, "scripts/recompile.py", "--set", args.set])
        if code:
            sys.exit("bringup: recompile failed:\n" + out[-3000:])
    for r in range(args.rounds):
        cmd = [exe, games.rom_cache(args.set), str(args.frames)]
        if args.inputs:
            cmd += ["--inputs", args.inputs]
        code, out = run(cmd, os.path.join(logs, f"round{r}.txt"))
        m = STOP.search(out)
        if not m:
            print(f"bringup: round {r}: m2run exit {code}; last lines:\n" + "\n".join(out.strip().splitlines()[-8:]))
            return
        addr = int(m.group(2), 16)
        seeds = os.path.join(ROOT, "seeds", args.set + ("_snd.txt" if m.group(1) else ".txt"))
        with open(seeds, "a") as f:
            f.write(f"{addr:08x}\n")
        print(f"bringup: round {r}: {'sound ' if m.group(1) else ''}seed {addr:08x}")
        code, out = run([sys.executable, "scripts/recompile.py", "--set", args.set])
        if code:
            sys.exit("bringup: recompile failed:\n" + out[-3000:])


if __name__ == "__main__":
    main()
