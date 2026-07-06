# IMA Traveller

A reverse-engineered port of **traveller 2.0c** (Driessens & Verstappen, 1996) —
the "IMA" cellular automaton that endlessly zooms into evolving organic pixel
structures — running on the freeglut-sdl2-ogles2 stack: native Mac/Windows/Linux
(gl4es + ANGLE) and the browser (Emscripten).

Ported from the original Windows 98 + OpenGL executable via Ghidra/Hex-Rays
decompilation.  Source comments reference the original's function addresses
(`sub_XXXXXX`).

## Layout

| File | Contents |
|------|----------|
| `ima_core.c/h` | The CA engine: RNG, mutation table, breeding rules, palette. Pure C, no GL. |
| `traveller.c` | The "zoomer": GLUT app reproducing the original's immediate-mode renderer. |
| `ima_selftest.c` | Headless checksum harness (see Verification). |
| `Makefile` | Build for native + Emscripten, modeled on newave.mk in freeglut-sdl2-ogles2. |
| `web/traveller.html` | Hand-maintained page loading `web/traveller.js`. |

## Build & run

All headers and prebuilt libs (freeglut-sdl2-ogles2, gl4es, ANGLE) are
vendored under `deps/` — see [deps/README.md](deps/README.md) for provenance
and `./deps/refresh.sh` to re-vendor from sibling source trees.  The only
external requirements are SDL2 for native builds (`brew install sdl2`) and
emcc for web builds.  Every dep path is a `?=` make variable, so external
trees can still be swapped in.

```sh
make            # native -> ./bin-<os>-<arch>/traveller
make run
make browser    # web    -> ./web/traveller.js
make selftest   # headless CA core check
```

## How the original renders (and this port reproduces)

- The CA image (default 768x512, 15-bit `(H<<10)|(S<<5)|V` pixels) is converted
  through a 32768-entry palette to **RGBA5551** and tiled into a grid of
  **256x256 `GL_RGB5_A1` textures** (Win98-era max texture size) — two texture
  sets, swapped every generation.
- Each frame draws the tile grid as `GL_TRIANGLE_STRIP` quads (immediate mode,
  identity projection/modelview) over an NDC rectangle that grows by
  `2^(1/frames_per_generation)` per frame — an almost seamless 2x zoom per generation,
  panning toward the mouse-driven focus point.
- Evolution is **amortized**: `speed` (1..4) selects 1/2/4/8 CA rows evolved per
  displayed frame; when the generation completes, the new image is uploaded one
  tile per frame (with the original's dummy `GL_POINTS` draw to force the
  transfer), then the texture sets swap and the zoom rectangle resets.

Each generation reads a half-size region of the previous image at the focus
point; every parent pixel breeds a 2x2 block of children that inherit each HSV
channel from whichever of 3 neighbors is closest to the parent, plus a small
mutation from a 16x16 table indexed by pixel bits and a lagged-Fibonacci RNG.

## Config

Reads `traveller.config` from the working directory — same keys and defaults
as the original:

```
speed 2          # 1..4 -> 1/2/4/8 CA rows per frame (5 = 16: port extension)
rate 60          # target fps for sync CLOCK
steer LINEAR     # LINEAR | CURVED (Hermite-interpolated pan)
sync CLOCK       # FREE | CLOCK | CHRONO | VBLANK
filter NEAREST   # NEAREST | LINEAR texture filtering
detail LOW       # LOW 768x512 | MEDIUM 1024x768 | HIGH 1280x1024
                 # | ULTRA 2048x1536 | ULTRA2 2560x2048
                 # (ULTRA/ULTRA2: port extensions for retina drawables)
xres 640         # window width: 640 | 800 | 1024
fullscreen no
intro yes        # port extension: compress the first ~10 near-black
                 # generations while the seed grows to full screen
```

If the config file does not set `detail`, the port auto-matches it to the
drawable height at startup so a texel is ~1 pixel at generation start --
the original's window/CA pairing, without which large modern windows turn
blocky well before each generation's refinement arrives.  Retina-class
drawables select ULTRA (over ~1140 px tall) or ULTRA2 (over ~1700 px),
port extensions beyond the original's three sizes (same 256x256 tile
architecture).  Auto-selected detail also raises `speed` (unless set) to
keep the original's ~2x zoom per 2.2 s cadence: MEDIUM/HIGH -> 3,
ULTRA -> 4, ULTRA2 -> 5.  Set `detail LOW` + `speed 2` + `intro no` for
the literal 1998 behavior.

High-DPI note: rendering is at full device resolution on every target
(SDL_WINDOW_ALLOW_HIGHDPI native, devicePixelRatio-scaled canvas backing
store on Emscripten) -- remaining blockiness is texel density, not scaling.

**Controls**: mouse steers the zoom focus; right button opens the menu
(speed/steer/sync/rate/filter/detail); `F1`/`p` freeze; `F4`/`s` single-step
while frozen; `ESC` quits (prints the original's cycle/fps stats).

Port extensions: `-seed N` (fixed RNG seed; default `time(0)` like the
original), `-trial` (quit after 101 generations, as the original demo build
did), `-dump N` (write a BMP per generation then quit; used for headless
render verification).

## Verification

The CA core is **bit-exact** with the Python reference implementation
(`ima-traveller` repo, `ima_traveller_final.py`):

```sh
make selftest
# vs.
python3 ima_traveller_final.py --seed 12345 --frames 12 --checksums
```

Both print identical per-generation CRC32s of the CA buffer (verified for
768x512 and 1024x768, 40+ generations).

## Faithfulness notes

- Timing uses `SDL_GetPerformanceCounter` (the original used
  `QueryPerformanceCounter`); on Emscripten the busy-wait pacing is skipped and
  requestAnimationFrame provides ~60 Hz.
- The original ran the generation-end tile uploads in a single message-loop
  iteration with one real buffer swap per tile; this port runs each upload as
  its own display callback (exactly one `glutSwapBuffers` per callback) --
  the same paced frames natively, and the correct contract under the
  browser's requestAnimationFrame, where only one composite happens per
  callback no matter how many swaps are issued.
- The original re-centered the Windows cursor during the first 3 generations;
  this port pins the *stored* mouse position to center instead (no pointer
  warping, which the web cannot do).
- The original was single-buffer capable (`doublebuffer no` + `glFinish`);
  this port always double-buffers.
