#!/usr/bin/env python3
"""Every control must actually change the picture.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything.

This renders each parameter at both ends of its range against the same moving
scene and reports any that made no difference at all.

    python3 tools/sweep.py [--binary build/rotest] [--size WxH] [--jobs N]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**Many controls are conditional.** Frequency does nothing until Amount is up;
the flash controls do nothing until a flash is in flight; the audio ones do
nothing until a spectrum arrives, because the host is the only thing that ever
supplies one. Each such parameter carries the context it needs in `CONTEXT`,
and a parameter missing from that table is swept on the defaults. That is the
table doing its job: a new control has to say what else must be true.

**A flash is an event, not a level.** Sweeping Length on a frame with no flash
in it proves nothing, so the flash contexts put the trigger on Interval at its
shortest, which fires on frames 0, 6, 12 ... 36 at the harness's 60 fps, and
render exactly 37 frames so the last one holds a fresh pulse.

**Every name must be unique.** `--set` finds a parameter by name and takes the
first match.

**Never sweep the About block or the spectrum.** The About entries are buttons
that open a web browser; the Audio buffer has no scalar to sweep and is fed by
`--audio` instead.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

WIDTH, HEIGHT = 320, 180
FRAMES = 40

# What else has to be true for a parameter to have any effect at all. Keys
# beginning with "_" are HARNESS settings, not plugin parameters:
#   _frames   how many frames to render (default 40)
#   _audio    feed the synthetic spectrum at this level
#   _low/_high  the two positions to compare, when not the range's ends
FLASH = {"Trigger": 4, "Interval": 0, "_frames": 37}
CONTEXT = {
    "Frequency": {"Amount": 0.6},
    "Audio Drive": {"_audio": 0.8},
    "Audio Band": {"_audio": 0.8, "Audio Drive": 1.0},
    # A press before the first frame lands the pulse on frame 0.
    "Fire": {"_frames": 1},
    # Off against Beat: at 120 bpm the second beat lands on frame 30.
    "Trigger": {"_high": 1, "_frames": 31},
    # 0.1 s against 4 s: only the short one has fired again by frame 36.
    "Interval": {"Trigger": 4, "_frames": 37},
    "Length": FLASH,
    "Phase": FLASH,
    "Level": FLASH,
    "Colour": FLASH,
    "Depth": {"Mains": 1},
}


def parameters(binary):
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append((int(m.group(1)), m.group(2).strip(), m.group(3),
                          float(m.group(5)), float(m.group(6))))
    return found


def render(binary, path, overrides):
    frames = overrides.get("_frames", FRAMES)
    args = [binary, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    if "_audio" in overrides:
        args += ["--audio", str(overrides["_audio"])]
    for name, value in overrides.items():
        if not name.startswith("_"):
            args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    binary, scratch, pid, name, low, high, context = job

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(binary, f"{scratch}/{pid}_lo.png", lo)
    b = render(binary, f"{scratch}/{pid}_hi.png", hi)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run cut off by a CI timeout
    # still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=str(ROOT / "build" / "rotest"))
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    binary = str(pathlib.Path(args.binary).resolve())
    if not pathlib.Path(binary).exists():
        print(f"{binary} is not built")
        return 1

    scratch = tempfile.mkdtemp(prefix="rosweep")

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters(binary):
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("buffer", "text"):
            skipped.append((name, "no scalar to sweep; the spectrum is fed by --audio"))
            continue
        work.append((binary, scratch, pid, name, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1
    print(f"all {len(results)} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
