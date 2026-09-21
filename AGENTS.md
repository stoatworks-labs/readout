# AGENTS.md — Readout

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

A CMOS rolling shutter, as an FFGL 2.1 effect for Resolume Arena and Avenue.

A CMOS sensor does not expose the whole picture at once. It exposes and reads its
rows one after another, so row *r* of *H* is read `T_read * ( 1 - r / H )` before the
frame's timestamp and was integrating for `T_exp` before that. Every pixel therefore
has a **sample window**, and the plugin samples the scene there.

---

## The one idea

**Every pixel gets a sample time, and everything is evaluated at it.**

That is the whole model. There is no skew code, no jello code and no banding code —
there are four things evaluated at the row's window, and the famous artefacts are what
those four *do*:

| evaluated at the window | what comes out |
| --- | --- |
| the ring of recent frames | **skew** — a bar moving at v px/frame leans by v over a one-frame readout — and, with a long exposure, motion blur |
| the camera pose | **jello** — a shake at f Hz puts a full wave every `1/f` of readout down the picture |
| the flash pulse | **banding** — a pulse lights `( T_exp + T_flash ) / T_read` of the frame, with ramps `min( T_exp, T_flash )` long where the overlap is partial |
| the mains light | **flicker bands** — a lamp at twice the mains frequency beats against the readout, `T_light / T_read` of the frame apart |

The propeller and the partial-frame looks need no code at all; they are the first row
of the table applied to content that happens to be rotating or cutting.

This is why the harness can measure rather than eyeball. Each of those is a closed
form with one right answer, so `rotest` renders a synthetic scene through the real
plugin class, reads the pixels back, and compares against the arithmetic.

### What falls out, and what does not

The honest limit is **discrete input**. A real sensor integrates a continuous scene;
this one has whatever frames the host handed it, at the host's frame rate.

- Between two frames the scene is either **interpolated linearly** (`Blend`) or
  **held** (`Hold`). Blend is the better model of a moving object and makes a
  continuous lean; Hold is what a real sensor fed by a low-frame-rate source does, and
  it stairsteps.
- So the skew of a bar moving at 24 px/frame is right to a fraction of a pixel, but
  the *sub-frame detail* of anything moving non-linearly between two input frames is
  invented by the interpolation, not observed.
- **The camera pose is evaluated once per row, not integrated across the exposure.**
  A real sensor blurs the shake as well as sampling it. Rows are the axis that makes
  jello, and that part is exact; the blur is not modelled.

`Global` is the control that makes all of this checkable: it sets `T_read` to zero, and
with nothing else on the plugin then returns the input **bit-exactly**.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Controls.{h,cpp}` | What a 0..1 slider position means, in seconds and hertz. Every mapping has an inverse, so a check can ask for exactly one frame of readout. |
| `source/Ring.{h,cpp}` | The last N input frames, as the layers of one array texture. |
| `source/Shaders.{h,cpp}` | Two complete shaders. Nothing is assembled at run time. |
| `source/Readout.{h,cpp}` | The plugin: parameters, the clock, the shake, the flash schedule, the audio, the two passes. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/rotest/` | The offline harness: renders, measures, benchmarks, pipes. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, plus the release-time checks done locally. |

Two passes:

1. **capture** — the host's input into the ring layer the newest frame belongs in.
   Resolves `MaxUV` and the half-texel inset once, so the readout pass works on a
   texture where the picture fills the layer. At an interior pixel centre it is an
   exact copy, which is what lets `--global` assert 0/255.
2. **readout** — straight to the host's framebuffer. Row index, sample window, camera,
   ring integration, flash, light, mix.

---

## Traps

Roughly in the order they will bite.

### ☠️ The host's clock is enormous, and a float cannot hold it

Ferric measured Resolume handing over **499,217,238 ms** — 5.8 days of absolute clock.
A `float` of that number resolves to about 0.03 s, which is one and a half whole
readouts: every phase in the shader would quantise to garbage, and it would look like
a *timing* bug rather than a precision one.

So **nothing absolute crosses into GLSL**. Time in the shader is `tau`, seconds before
*this frame's* timestamp, which is at most a tenth of a second. The mains phase and
each shake component's phase are reduced into 0..2π on the CPU, in `double`
(`reducedAngle`), and handed over as angles. Keep it that way.

**The Windows run of 2026-09-21 is the first confirmation of this from a real host.**
Under `oxbow` the detector settles on **seconds** — this plugin's own diag log shows
`scale=1.000000` by frame 60. Under **Arena 7.27.1** the fleet's plugins saw
**milliseconds**, a raw host time of about **574,073**, and the detection decided
milliseconds. The unit really does differ between hosts, the voting code really does
pick it up, and the number really is large enough to matter in a `float`. What is still
missing is `readout`'s own in-Arena log line: the milliseconds decision was read from a
sibling's log during that run, not from this one's.

### ☠️ The ring must be an array texture, not N samplers

The readout pass has to fetch a different frame per *pixel* — a row near the top of the
picture is reading further back in time than one near the bottom, and a long exposure
straddles several frames. GLSL 4.10 allows indexing a `sampler2D[]` only with a
**dynamically uniform** expression, and a per-pixel frame age is not one. Some drivers
accept it anyway, which is worse: it would work here and fail on somebody's show.

One `GL_TEXTURE_2D_ARRAY` is a single sampler that any integer can index. The cost is
that a layer is drawn into through `glFramebufferTextureLayer`, so `Ring` owns its own
FBO rather than using the SDK's `FFGLFBO`.

### ☠️ The frame period must be measured, not assumed

`Readout Time` is in milliseconds because that is what a sensor is specified in, but
the ring is indexed in *frames*. The conversion is the host's frame period, and it is
measured from the host's own clock deltas, smoothed. Assume 60 and every skew is 20%
wrong at 50 fps — and, worse, the offline harness would agree with itself perfectly,
because it drives a synthetic 60.

The smoothing is why `--skew` renders eight frames before it measures: the period
converges within two or three.

### ☠️ The harness must declare its clock unit

Left to the wall clock, the harness renders a hundred frames in a few milliseconds.
The plugin would then measure a frame period of near zero, a 20 ms readout would come
out as *thousands* of frames of ring, and the ring would clamp to 16 — so every check
would measure the clamp. `Session::render` calls `SetClockScaleForTest( 1.0 )` and
drives `SetTime` at a synthetic 60 fps.

### ☠️ A control the host alone can feed reads as dead

Three of them here: the spectrum (`Audio`, `Audio Drive`, `Audio Band`), the transport
(`Trigger` on Beat or Bar), and the Fire button. Nothing in an offline harness supplies
any of those unless it is asked to, so the sweep would report five working controls as
dead and bury a real failure among them.

`rotest` therefore drives `SetBeatInfo( 120, phase )` every frame, injects a **shaped**
synthetic spectrum on `--audio` (pink-ish, with a kick confined to the low bins and
hiss in the top ones — a *flat* spectrum would make `Audio Band` genuinely dead, and
correctly so, which is the worst kind of test failure to debug), and presses Fire on a
chosen frame.

### ☠️ A flash is an event, and the sweep has to catch one in flight

Sweeping `Length` on a frame with no pulse in it compares two identical black frames.
The flash contexts in `sweep.py` put `Trigger` on Interval at its shortest — 0.1 s,
which at the harness's 60 fps fires on frames 0, 6, 12 … — and render exactly 37
frames, so the last frame always holds a pulse that was fired one frame earlier and is
still inside the readout window. Change the frame count and several flash controls go
quietly dead.

### The pulse is stored by its CENTRE, and the shader is handed its BEGINNING

`Flash::centre` is a host timestamp; the shader's `FlashPulse.x` is
`( now - centre ) + length / 2`, i.e. how long before *now* the pulse began. Two
representations because the CPU side has to keep a pulse across frames (absolute) and
the GPU side must never see an absolute time (relative). Getting the half-length on the
wrong side moves the band by half its own height, which reads as a phase error.

### `FFGLShader::Set` has no array overload

The overloads are `float`, `vec2`, `vec3`, `vec4` and `int`. `ShakeHz`, `ShakePhase`,
`ShakeAmp`, `FlashPulse` and `FlashRGB` are arrays and go through `glUniform1fv` /
`3fv` / `4fv` against a hand-fetched location. A `Set( name, a, b )` on an array would
resolve to the `(float,float)` overload and raise `GL_INVALID_OPERATION` where nothing
can see it.

### A GLSL uniform name that does not match the C++ is silently ignored

`glGetUniformLocation` returns −1 and `glUniform(-1)` is a documented no-op, so a
control can be stone dead while everything compiles, links, loads and renders.
`tools/sweep.py` is the only thing that catches it.

### A ranged STANDARD parameter cannot have a ranged default

`SetParamInfo` clamps a standard default into 0..1 *before* returning, and
`SetParamRange` can only be called afterwards. So every ranged control here is a plain
0..1 float and the conversions live in `Controls.cpp` — with **inverses**, because the
alternative is a check that asks for "about one frame" of readout and reports a number
near the right one.

### A TEXT parameter without `SetTextParameter` kills the whole plugin

`instantiateGL` pushes every declared default back through the setters and deletes the
instance the moment one returns `FF_FAIL` — which is exactly what
`CFFGLPlugin::SetTextParameter` does. The About block is display-only text, so there is
nothing to store, but it has to say so *successfully*. Invisible in every in-repo
harness, because they call the plugin class directly; `oxbow selftest` is what actually
exercises it here.

### The plugin registers itself from a file-scope constructor

`CFFGLPluginInfo` is never referenced by name, so in a **STATIC** archive the linker may
drop the whole translation unit, giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `readout_core` is an **OBJECT** library for that
reason.

### Nothing in the SDK restores a viewport

`ScopedFBOBinding` restores the framebuffer binding and only that. The capture pass
sizes the viewport to the ring, so `ProcessOpenGL` reads `GL_VIEWPORT` before anything
of ours touches it and puts it back before the readout pass draws to the host's
framebuffer. `Ring::EndCapture` deliberately restores the *framebuffer* and not the
viewport, because nothing inside `Ring` knows what the host's was.

Related: every `ffglex::Scoped*` binding **clears to 0** on scope exit rather than
restoring, and allocating a texture leaves the active unit bound to nothing. Every
`Ensure()` happens before anything binds a texture, and it has to stay that way — the
symptom otherwise is correct on every frame except the one that allocates.

### Changing the ring's depth empties it

The layer indexing is modulo the depth, so a different depth does not shuffle the
contents, it files every one of them under the wrong age — which shows as the picture
jumping to a different arrangement of recent frames. `Ring::Ensure` reports a reshape
and the plugin resets `head` and `filled`.

That is also why the depth is rounded up to a multiple of four: dragging `Readout Time`
would otherwise reallocate and empty the ring at *every millisecond* of travel.

### macOS must build universal, and the log will lie about it

CMake latches `CMAKE_OSX_ARCHITECTURES` when the first target is created, so setting it
late is silently ignored and the build still logs a success. Only `lipo` is honest,
which is why `verify.sh` configures a **fresh** `build-universal` rather than measuring
whatever was lying around.

### `nm … | grep -q` fails when grep SUCCEEDS

Under `set -o pipefail`, `grep -q` exits at its first match, `nm` takes SIGPIPE, and
the *pipeline* reports failure on a perfectly good binary. It is output-size dependent,
so it looks intermittent. Capture first, match against a here-string or with `case`.

### `vcpkg.json` is invisible from the CMakeLists

GLEW arrives through the vcpkg manifest and nothing in `CMakeLists.txt` mentions it —
so every local build and every macOS CI job passes while the Windows job fails at
*configure*.

### ☠️ An ssh session on Windows has no desktop, so Arena starts but does nothing

An ssh login lands on the **service window station**, which has no desktop. Arena
launched from there sits at about 31 MB, never opens a window, loads nothing, and
cannot be screenshotted — and it does not say why. It has to be started **in the
console session (session 1)**, through the scheduled-task wrapper `C:\arena-lab\s1.ps1`
on win-lab. Anything that needs Arena to actually render goes through that wrapper.

### ☠️ Arena's REST API lists effects by `idstring`, and its add-effect route lies

`/api/v1/effects` and `/api/v1/sources` key each entry by the **FFGL id** — `RO01`, not
`SW Readout` — as `idstring`, so searching the list by display name finds nothing even
when the plugin is registered. Worse, the add-effect endpoint returns **200 without
adding anything**: after posting to it, `/api/v1/…/clips/1` still showed only
`Transform`. Instantiation has to be driven from **Arena's own effects browser** (a
double-click applies the effect to the current selection), and the proof is the
plugin's diag log, not the clip's effect list.

---

## Decisions taken without asking

The brief said to decide and write it down.

- **Sixteen ring slots.** Sixteen full pictures is ~250 MB at 4K, and it covers the
  longest window the controls can ask for (60 ms readout + 40 ms exposure) at any frame
  rate up to about 140 fps. Past that the oldest rows read the oldest frame there is,
  which degrades to a shorter effective readout rather than to anything wrong.
- **No factory presets.** Most of the fleet has them, and they bring the whole
  host-owns-parameter-state mechanism (`hostValues[]`, two tolerances, the three-host
  test) with them. Twenty-one controls where the defaults already do the one thing did
  not justify it for 0.1.0; the mechanism is well understood in `afterglow` and
  `tinsel` and can be lifted wholesale later.
- **The flash colour is a list, not RGB sliders.** Three more parameters to express
  "red" was the wrong trade; eight entries cover white, two colour temperatures and
  five gels.
- **Audio drives the shake, not a new effect.** A speaker near a camera moves the
  camera; that is the honest mechanism, and everything downstream of the pose follows
  on its own. Audio onsets can also fire the flash, which is a cue, not physics.
- **`Interpolation` is two options rather than a mix slider.** They are different
  models of what happened between two frames, not two ends of one.
- **Direction is one dropdown of four**, and left-right/right-left genuinely swap which
  axis the rows run along — that is what a sensor mounted sideways does, and it is one
  `if` in the shader.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-21)

Every number below is `tools/verify.sh` on this machine, against a fresh universal
Release build. The scene is synthetic and the expectation is closed-form in each case.

- **Skew.** A bar moving at 24 px/frame under a one-frame readout leans **24.000 px**
  in all three cases. Blend and Hold cannot agree here and should not: Blend resolves
  the one row's worth the last row is read before the timestamp, so its closed form is
  **23.967** (the 1/H), while Hold snaps to whole source frames and its closed form is
  **24.000**. The harness allows **0.1 px**, which is what makes both pass; the
  remainder is v/H, so it is 0.02 px at 1080 rows and 0.13 px at the 180 CI runs on —
  which is why a single expectation calibrated on one raster failed on CI. Three cases:
  Blend and Hold, top-down and bottom-up.
- **Flash banding.** With `T_read` 40 ms, `T_exp` 4 ms, `T_flash` 8 ms at phase 0.30 on
  a 720-row frame: band **215.0 rows** against an expected **216.0**, centred at
  **252.5** against an expected **252.0**, peaking **+191/255**.
- **Flicker banding.** 50 Hz: period **120.00 rows** over 6 crossings, expected
  **120.00**. 60 Hz: **100.00** over 7 crossings, expected **100.00**.
- **Jello.** A 40 Hz shake of 0.02 picture heights under a 60 ms readout: vertical
  period **300.00 rows** against an expected **300.00**, peak-to-peak swing **28.80 px**
  against an expected **28.80**.
- **Global.** Global on returns the input at **0/255** deviation, at both 20 ms and
  60 ms of nominal readout. The negative control — the same scene with Global *off* —
  differs by **164/255**, so the check can fail.
- **No dead controls.** All **20** swept parameters measurably change the picture; 5
  skipped with a reason (the About buttons and the spectrum buffer).
- **Every shader compiles** through `glslc`, not merely through Apple's driver.
- **The bundle** is universal (`x86_64 arm64`), exports `_plugMain`, names its own
  binary in its plist, carries `com.stoatworks.ffgl.readout`, and ad-hoc signs.
- **A real FFGL host loads it.** `oxbow probe` reports `SW Readout` / `RO01` / `effect`
  — which is the only check that the 16-character name field was not silently truncated
  — and `oxbow selftest` instantiates it through `plugMain` and renders 120 frames with
  no GL error. That path exercises `instantiateGL`, and so the `SetTextParameter` trap.
- **Render cost** (`--bench`, 60 frames after a 20-frame warm-up, `glFinish` both
  sides, defaults): **0.278 ms** at 720p, **0.517 ms** at 1080p, **0.902 ms** at 1440p,
  **2.274 ms** at 4K — 13.6% of a 60 fps frame at 4K.

### Verified in Resolume Arena, on Windows (2026-09-21)

The DLL taken to Arena was **cross-compiled in the Parallels guest on this Mac** — ARM64
Windows 11, MSVC 2022 Build Tools, `cmake -A x64`, vcpkg triplet
`x64-windows-static-md` — because there is no x64 Windows machine in the *local* build
loop. CI builds x64 Windows itself, but that build has never been in front of Arena.
The hand-built one was taken to **win-lab**, an x64 Windows 11 Pro VM with **no GPU**:
the adapter is the Microsoft Basic Display Adapter, so OpenGL is **Mesa llvmpipe**
dropped in beside Arena. The host was **Resolume Arena 7.27.1** (build 15990),
started in the console session.

- **The DLL builds and exports the entry point.** **374,272 bytes**, and
  `dumpbin /EXPORTS` shows **`plugMain`**.
- **Arena registers it.** `/api/v1/effects` lists **`SW Readout`** among 112 video
  effects, under its FFGL id **`RO01`** as `idstring`, with the description the plugin
  declares. That is the in-host counterpart to the `oxbow probe` name check.
- **Arena loads the DLL.** `%LOCALAPPDATA%\readout\` holds
  `plugin loaded build=<stamp>`, the stamp of the DLL built minutes earlier.
- **Arena instantiates it, and the shaders compile.** Applied from Arena's own effects
  browser, the diag log shows
  `GL vendor=Mesa renderer=llvmpipe (LLVM 22.1.8, 256 bits) version=4.5 (Core Profile) Mesa 26.2.0`
  followed by `initialised`, and Arena drew its inspector for it, groups and all. So
  `instantiateGL` — and with it the `SetTextParameter` trap — survives a real host.
- **It renders headlessly on x64 Windows too.** `oxbow selftest`, built x64 in the same
  guest: **120 frames, gl error 0x0, PASS**, with **921,600 of 921,600** pixels lit
  (100%).
- **The clock-unit detection met a real host**, and decided **milliseconds** under Arena
  against **seconds** under `oxbow`. See the first trap above.
- **No warnings or errors.** The diag log is clean of WARN, ERROR and FAIL.

That run did not establish two things: **no GPU was involved** — it was all llvmpipe —
and **nothing was timed on Windows**, so the millisecond figures above stay
macOS-only. The effect was applied to the **composition**, not to a clip:
`/api/v1/…/clips/1` still showed only `Transform` afterwards, so the proof of
instantiation is the diag log rather than the clip's effect list.

### Assumed, or not done

- ☠️ **It has never run on a GPU in Resolume, and has never been instantiated in Arena
  on macOS.** The Arena run above was on llvmpipe, a software rasteriser, on Windows —
  which proves registration, loading, instantiation and shader compilation, and nothing
  about speed. Whether 21 controls read sensibly to somebody using them, whether
  Resolume draws `Fire` as a button that sends one rising edge per press, and what its
  blend state looks like on the way in are all still untested.
- **Beat and Bar have only seen a synthetic 120 bpm transport.** Whether Resolume's
  `barPhase` behaves as assumed across a tempo change or a scrub is unknown.
- **The audio path has only ever seen `rotest`'s synthetic spectrum**, never Resolume's
  own FFT — no real audio reached the plugin during the Arena run either. The 64-bin
  count and the sqrt on the magnitudes are the fleet's figures, taken on trust.
- **Nothing has been built for Linux.** The DLL taken to Arena was built by hand in the
  guest; the released one was not. CI (macOS and Windows) and the release workflow have
  both run and passed on GitHub, and the CI Windows build has never been put in front of
  Arena.
- **No long session, no composition save or reload, and no preset recall in the host**
  were exercised on Windows.
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **No factory presets**, and therefore none of the preset/host-echo machinery the rest
  of the fleet carries.
- **`ATTRIBUTIONS.md` is still a provisional hand copy**, in the shape the
  `stoatworks-backend` sync generates. The project is now in the website's
  `projects.json`, in `sync-about.py`'s TARGETS and in `attributions/names.json`, so
  `StoatworksAbout.h` is **generated** — do not hand-edit it; it still carries
  `guide=""` because no user guide exists. What is still missing is
  `sync-attributions.py`'s own master lists, which is why this file is hand-written.
- **Nothing has been through a real show.**

---

## Open questions

- **Should `Exposure` blur the shake as well as the scene?** It currently samples the
  pose once per row. Integrating it would need several pose evaluations per pixel and
  would mostly show at long exposures with a fast shake.
- **Is 16 slots the right cap at 4K?** 250 MB of video memory is real, and the
  alternative — storing the ring at half resolution, as `afterglow` does — trades
  sharpness for headroom in a plugin whose output is mostly *sharp* rows.
- **Should the flash fire on the host's own beat grid rather than on a frame?** A pulse
  is currently placed at the phase of the frame it was fired on, so its position is
  quantised to a frame. On the beat, at 60 fps, that is up to 8 ms of jitter — a
  fraction of a band.

---

## Siblings

- **`afterglow`** — the ring of recent frames, and the reason this one is an array
  texture rather than 16 buffers.
- **`regauss`** — the audio input, the transport, and the clock-unit voting.
- **`tinsel`** — the harness shape, `sweep.py`, `verify.sh`, the CI and release
  workflows. Its `AGENTS.md` is the fleet's trap list.
- **`graticule`** — the About-header pattern (a hand copy at first, generated since) and
  the `verify.sh` shape used here.
- **`oxbow`** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
