# readout

A CMOS rolling shutter as an FFGL **effect** for Resolume Arena/Avenue. C++/GLSL,
CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the sample window, the ring or the flash schedule.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- x64 Windows DLL: cross-compiled in the Parallels guest — `cmake -A x64`, MSVC 2022
  Build Tools, vcpkg triplet `x64-windows-static-md`. 374,272 B, exports `plugMain`.
  On win-lab, Arena must be started through the session-1 wrapper `C:\arena-lab\s1.ps1`
  (`AGENTS.md`); an ssh session there has no desktop.
- Render a frame offline: `./build/rotest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Readout Time=0.9" --set "Mains=1"`
- List parameters, kinds, defaults and ranges: `./build/rotest --list`
- Other sources: `--source bar --speed 24`, `--source flat`
- Fire a flash 0.5 s in: `--fire 0.5`; feed a synthetic spectrum: `--audio 0.8`
- Footage through the real shaders:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/rotest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check + the sweep)
- A bar moving at v leans by v: `./build/rotest --skew`
- A flash bands `(E + L) / T_read` of the frame: `./build/rotest --band`
- Mains bands sit `T_light / T_read` apart: `./build/rotest --flicker`
- Global on returns the input bit-exactly: `./build/rotest --global`
- A shake at f wobbles at `1/f / T_read`: `./build/rotest --jello`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/rotest --bench`
- What a host sees: `../oxbow/build/oxbow probe build-universal/Readout.bundle`

## Notes
- **Every pixel is a sample time.** `Readout.cpp` only converts controls into the
  shader's units and reduces phases; the sample window and everything evaluated at it
  live in `kReadoutShader`. A wrong band is a GLSL fix.
- **Time in the shader is `tau`: seconds BEFORE this frame's timestamp**, never
  absolute. Resolume's clock has been seen at 499,217 s, where a float resolves to
  0.03 s — three whole readouts. Phases are reduced into 0..2π on the CPU, in double.
- **The ring is one `GL_TEXTURE_2D_ARRAY`**, not N textures: GLSL 4.10 indexes a
  sampler array only with a dynamically uniform expression, and a per-pixel frame age
  is not one. `Ring.cpp`.
- **The frame period is measured from the host's own deltas**, not assumed. It is what
  turns milliseconds of readout into frames of ring; assume 60 and every skew is wrong
  at 50.
- **`FFGLShader::Set` has no array overload.** `ShakeHz`, `ShakePhase`, `ShakeAmp`,
  `FlashPulse` and `FlashRGB` go through `glUniform*fv` directly.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every ranged control is 0..1 and `Controls.cpp` holds the units. Every mapping
  has an inverse, so the harness can ask for exactly one frame of readout.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `readout_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `RO01`, display name `SW Readout` (16 characters, the host's limit).

## Not done yet
- **Never run on a GPU in Resolume, and never instantiated in Arena on macOS.** It was
  registered, loaded and instantiated in Arena 7.27.1 on Windows on 2026-09-21, on Mesa
  llvmpipe, with the shaders compiling — no GPU, and nothing timed there. Everything
  numeric is still measured offline on macOS, plus an `oxbow` load. No release tag, not
  registered on the website, no OpenFX port, no browser demo.
- `source/StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies with
  `guide=""`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/readout/readout.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\readout\readout.YYYY-MM-DD.log        (Windows)
