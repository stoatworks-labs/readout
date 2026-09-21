# readout

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The physics is not asserted
> but measured: an offline harness drives the real plugin class in a headless GL
> context and checks each claim against its closed form — a bar moving at v px/frame
> leans by v, a flash bands `(exposure + length) / readout` of the frame where the
> phase says, 50 and 60 Hz mains bands sit `T_light / readout` apart, a sinusoidal
> shake wobbles the rows at `1/f / readout`, and Global returns the input
> bit-exactly — with a negative control on the last of those so the check can fail.
> It has been registered, loaded and instantiated in **Resolume Arena 7.27.1 on
> Windows**, on a software rasteriser, with its shaders compiling. It has **never run
> on a GPU in Resolume**, and has never been instantiated in Arena on macOS. It is
> also loaded by [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real
> FFGL host and is not Resolume. See [Status](#status).

A CMOS rolling shutter, as an FFGL effect for [Resolume](https://resolume.com) Arena
and Avenue.

![A test card read out by a rolling shutter: an amber flash band across the middle, rolling mains-flicker bands, and vertical bars bent by camera shake](docs/hero.png)

<sub>One frame, rendered by `rotest`, the offline harness — not captured from
Resolume. A 52 ms readout: the amber band is a 5 ms flash that only the rows exposed
while it fired ever saw, the broad horizontal shading is a 50 Hz lamp beating against
the readout, and the vertical bars are bent because the camera moved while the sensor
was still reading.</sub>

<!-- downloads:start -->

## Download

**[v0.1.0](https://github.com/stoatworks-labs/readout/releases/tag/v0.1.0)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`readout-0.1.0-macos-universal.dmg`](https://github.com/stoatworks-labs/readout/releases/download/v0.1.0/readout-0.1.0-macos-universal.dmg) | 213 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`readout-macos-universal.zip`](https://github.com/stoatworks-labs/readout/releases/latest/download/readout-macos-universal.zip) | 176 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`readout-0.1.0-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/readout/releases/download/v0.1.0/readout-0.1.0-windows-x86_64-setup.exe) | 221 KB |
| x64 · .zip archive | [`readout-windows-x86_64.zip`](https://github.com/stoatworks-labs/readout/releases/latest/download/readout-windows-x86_64.zip) | 113 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/readout/releases](https://github.com/stoatworks-labs/readout/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## The one idea

A CMOS sensor does not expose a frame. It exposes and reads its **rows**, one after
another. Row *r* of *H* is read `Readout × ( 1 − r / H )` before the frame's
timestamp, and was integrating light for `Exposure` before that.

So every pixel has a sample window, and the picture is sampled there. That is the
whole plugin.

## What falls out

None of these is drawn. Each is that sample window applied to something:

- **Skew.** A vertical bar moving at *v* px/frame leans by `v × Readout`. The top of
  the frame was read earlier, so it shows the bar further back.
- **Jello.** Shake the camera and every row sees it in a different position, so a
  straight edge comes out as a wave — one full cycle every `1/f` of readout.
- **Flash banding.** A strobe shorter than the readout lights only the rows whose
  exposure window overlapped it. The band is `( Exposure + Length ) / Readout` of the
  frame high, its edges ramp over the shorter of the two, and where it sits is the
  flash's phase.
- **Light flicker banding.** A mains-driven lamp is brightest twice per cycle, so at
  50 Hz it pulses at 100 Hz. Beat that against the readout and you get rolling dark
  bands `T_light / Readout` of the frame apart — the reason phone video of a stage
  looks striped.
- **The propeller**, and partial frames, and every other rolling-shutter photograph
  anybody has ever posted — those need no code at all. They are the first item on this
  list, applied to content that happens to be rotating.

**Global** sets the readout to zero: a global shutter, for comparison. With nothing
else switched on it returns the picture untouched, to the byte.

### The honest limit

A real sensor samples a continuous scene. This one has the frames the host gave it, at
the host's frame rate. Between two of them it either **interpolates** (`Blend`) or
**holds** the older one (`Hold`) — so the lean of a moving bar is right, and the
sub-frame detail of anything moving unevenly between two input frames is the
interpolation's invention rather than an observation.

The camera shake is sampled once per row, not integrated across the exposure. Rows are
the axis that makes jello and that part is exact; a real sensor would also blur the
shake, and this does not.

## Controls

| Group | |
| --- | --- |
| **Sensor** | Readout Time (1–60 ms), Exposure (0–40 ms), Direction (top-down, bottom-up, left-right, right-left), Interpolation (Blend or Hold), Global. |
| **Shake** | Amount, Frequency (2–80 Hz), Rotation, plus an audio input: Audio Drive and Audio Band. A speaker near a camera moves the camera, so the audio drives the pose and the jello follows on its own. |
| **Flash** | Fire (a button), Trigger (Off, Beat, Bar, Onset, Interval), Interval, Length (0.1–20 ms), Phase, Level, Colour. |
| **Light** | Mains (Off, 50 Hz, 60 Hz), Depth. |
| **Output** | Mix. |

Beat and Bar follow Resolume's transport; Onset fires on a transient in the routed
audio. Audio Drive defaults to zero, so with nothing routed the camera sits still.

## Status

**v0.1.0, and honestly early — 21 September 2026.**

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (M4 Max, macOS 26.4.1) against a fresh
universal Release build. What it establishes, in numbers:

| check | result |
| --- | --- |
| `--skew` | a bar at 24 px/frame under a one-frame readout leans **24.000 px** in all three cases — Blend and Hold, top-down and bottom-up. Blend's closed form is **23.967** (the last row is read 1/H of a frame early) and Hold's is **24.000** (it snaps to whole source frames); each is inside the harness's **0.1 px** tolerance, so all three pass |
| `--band` | a 8 ms flash at 4 ms exposure over a 40 ms readout bands **215.0 rows** of 720, expected **216.0**, centred **252.5** against **252.0**, peaking +191/255 |
| `--flicker` | 50 Hz: **120.00 rows** period, expected **120.00**. 60 Hz: **100.00**, expected **100.00** |
| `--jello` | a 40 Hz shake over a 60 ms readout: period **300.00 rows** (expected 300.00), swing **28.80 px** (expected 28.80) |
| `--global` | **0/255** deviation from the input, twice; the negative control differs by 164/255, so the check can fail |
| `tools/sweep.py` | all **20** swept controls measurably change the picture |
| shaders | all 3 compile through `glslc`, not merely through Apple's driver |
| the bundle | a local build is universal (`x86_64 arm64`), exports `plugMain` and ad-hoc signs; `oxbow` reports `SW Readout` / `RO01` / `effect` and renders 120 frames through `plugMain` |

Render cost, 60 frames after a warm-up with `glFinish` both sides, at the defaults:
**0.278 ms** at 720p, **0.517 ms** at 1080p, **0.902 ms** at 1440p and **2.274 ms** at
4K — a seventh of a 60 fps frame at 4K. Those are macOS figures and only macOS
figures.

### In Resolume Arena, on Windows — 21 September 2026

The DLL taken to Arena was **cross-compiled in the Parallels guest on this Mac** (ARM64
Windows 11, MSVC 2022 Build Tools, `cmake -A x64`, vcpkg triplet
`x64-windows-static-md`), because there is no x64 Windows machine in the local build
loop. It comes out at **374,272 bytes**, and `dumpbin /EXPORTS` shows **`plugMain`**.
The *released* DLL is a different build: CI builds x64 Windows on every push and the
release workflow builds it again on a GitHub runner. That build has never been put in
front of Arena.

That DLL was taken to win-lab — an x64 Windows 11 Pro VM with **no GPU**: the adapter
is the Microsoft Basic Display Adapter, so OpenGL comes from **Mesa llvmpipe** dropped
in beside Arena. The plugin reported the context itself:
`GL vendor=Mesa renderer=llvmpipe (LLVM 22.1.8, 256 bits) version=4.5 (Core Profile) Mesa 26.2.0`.

| check | result, in **Resolume Arena 7.27.1** (build 15990) unless noted |
| --- | --- |
| Arena registers it | `/api/v1/effects` lists **`SW Readout`** among 112 video effects, under its FFGL id **`RO01`** as `idstring`, with the description the plugin declares |
| Arena loads the DLL | `%LOCALAPPDATA%\readout\` holds `plugin loaded build=<stamp>`, the stamp of the DLL built minutes earlier |
| Arena instantiates it, and the shaders compile | applied from Arena's own effects browser: the diag log shows the Mesa 4.5 Core Profile line and then `initialised`, and Arena drew its inspector for it, groups and all |
| `oxbow selftest`, x64 | **120 frames, gl error 0x0, PASS**, with **921,600 of 921,600** pixels lit (100%) |
| the diag log | clean of WARN, ERROR and FAIL |

**The clock-unit detection has now met a real host.** This repo is where the
float-overflow trap in Resolume's clock was found, and the detector that votes on the
host's unit had until now only ever seen a seconds host. Under `oxbow` this plugin's
own diag log still settles on **seconds** (`scale=1.000000` by frame 60). Under Arena
the fleet's plugins saw **milliseconds** — a raw host time of about **574,073** — and
the detection decided milliseconds there. So the trap is real in Arena and the
detection handles it. What was not read back is this plugin's own in-Arena vote: the
milliseconds line came from a sibling's log, not from `readout`'s.

**What is not established.** It has **never run on a GPU in Resolume** — the Windows
run was llvmpipe, a software rasteriser — and it has **never been instantiated in
Arena on macOS**. Nothing was timed on Windows: there is no frame timing from win-lab
at all, so whether the render cost above survives a real host is an open question. The
effect was applied to the **composition**, not to a clip — `/api/v1/…/clips/1` still
showed only `Transform` afterwards, so the proof of instantiation is the diag log, not
the clip's effect list. No long session, no composition save or reload and no preset
recall in the host were exercised. No real audio reached the plugin in Arena: the audio
path has still only seen the harness's synthetic spectrum, never Resolume's FFT, and
the 64-bin mapping is still assumed rather than measured. Beat and Bar have only seen a
synthetic 120 bpm transport. Nothing has been built for Linux. There is no user guide,
no OpenFX port and no browser demo — none of them in scope for 0.1.0 — and no factory
presets. Nothing has been through a show.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/readout
cd readout
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.
The x64 Windows DLL that was loaded into Arena was cross-compiled in the Parallels
guest on this Mac — `cmake -A x64`, MSVC 2022 Build Tools, vcpkg triplet
`x64-windows-static-md`.

## Building and testing

The offline harness renders the real plugin class headlessly, on a synthetic 60 fps
clock and a synthetic transport:

```bash
./build/rotest --out /tmp/frame.png --size 1920x1080   # the moving test card
./build/rotest --list                                  # every control, kind and default
./build/rotest --skew                                  # each claim, measured
./build/rotest --band
./build/rotest --flicker
./build/rotest --global
./build/rotest --jello
./build/rotest --bench                                 # 720p through 4K
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Footage goes through the real shaders with `--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/rotest --pipe --size 1920x1080 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
```

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model and the traps.

## Licence

MIT — see [LICENSE](LICENSE). Third-party components are listed in
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).
