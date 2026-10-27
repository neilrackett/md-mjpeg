# AGENTS.md — MD/MJPEG developer guide

This file is the single source of guidance for AI agents (and humans) working
on this repo. `CLAUDE.md` just points here. It merges the original template
architecture notes with everything learned bringing MD/MJPEG up on real
hardware — read "Hard-won lessons" before changing anything memory- or
boot-related.

## What this repo is

**MD/MJPEG** — a video player microfirmware for the
[SidecarTridge Multi-device](https://sidecartridge.com) (Raspberry Pi Pico /
RP2040 in the Atari ST cartridge slot). It streams a standard AVI (MJPEG
320×200 @ 25 fps + mono u8 PCM @ 5600 Hz for YM, or 25033 Hz for STE DMA
sound) from the SidecarT's SD card, decodes JPEG frames on the RP2040,
quantizes each frame to its own 16-colour ST palette (median cut to 15
colours over an exact RGB333 histogram, plus black at index 0 so the ST
border stays black, + blue-noise ordered dither + a saturation/contrast
boost), and blits it to the ST screen over the cartridge bus at up to 25 fps.
Audio is universal: the firmware auto-detects STE DMA sound (25 kHz 8-bit)
and falls back to YM2149 (~5.6 kHz) on a plain ST. On a mono monitor (ST
high res) it plays in black and white instead: each frame decoded luma-only
and dithered to 640×400 (see "ST high res" below). A/V sync is audio-master:
the fill callback's sample clock paces video; when decoding falls ≥2 frames
behind, frames are skipped so audio never stalls.

Built from **md-framebuffer-template** (the framework does the heavy
lifting): dual page-flipped framebuffer on the ST side, real VBL lock (50 Hz,
or 71 Hz on a mono monitor),
chunky→planar conversion split across both cores, ~6 kHz YM audio, IKBD
keyboard decoding. The template's demo suite was stripped out; the
pristine pre-strip tree is in `rp.bak/` (gitignored) and the demos remain
the reference for RP2040 optimization
tricks (per-file `#pragma GCC optimize("O3")`, `__not_in_flash_func`,
fixed-point LUTs, SIO interpolator, `fb_core1_dispatch` dual-core splits).

Key app sources (all GPL-3.0-or-later, SPDX headers):

| File | Role |
| --- | --- |
| `rp/src/mjpeg.c` / `include/mjpeg.h` | AVI demuxer, playback state machine, PCM ring, audio fill callback (Ghostbusters LUT), A/V clock |
| `rp/src/mjpeg_quant.c` / `include/mjpeg_quant.h` | per-frame colour pipeline: dither LUTs, RGB333 histogram, median cut, 512-entry map, JPEGDEC draw callback. Palette entries are RGB444 published in STE encoding (`PALETTE_STE_RGB`) — a plain ST reads the RGB333 truncation of the same word, so one palette serves both with no detection. This is finer *placement* only; dither + histogram default to RGB333, so distinct-level count (esp. greyscale) is unchanged. `MQ_HIST=g4` upgrades green to 4-bit (RGB343, 1024 bins) for a real greyscale win at +1.5 KB; full RGB444 (4096 bins) is RAM-blocked (lesson 4) — see the histogram-depth flag below. |
| `rp/src/mjpeg_menu.c` / `include/mjpeg_menu.h` | the video menu, laid out like MD/Lynx's MiSTer-style OSD: a 30×16-cell box, navy and white on black, title strip reading upwards (▲/▼ when the list scrolls), ABOUT (version) and EXIT TO GEM above the videos, the About page in the same box. Owns the selection; `mjpeg_menu_key()` hands `emul.c` a video to play, or `MJPEG_MENU_BACK` for ESC. ESC during playback draws the same box over the video (`mjpeg_show_menu()` → `mjpeg_menu_draw_over()` before each publish), and at the end of a video over its last frame (`mjpeg_finish()`: `mjpeg_menu_shown()` stays true, and `mjpeg_menu_render()` draws over the frame instead of clearing it) in the frame's pens 0 and 15: pen 0 is always black, and the quantizer holds 15 at white while `mjpeg_menu_shown()` (`mq_republish()` applies it at once) (published palette and map only — the hysteresis keeps the real colour; mono's grey ramp needs nothing). Holding the pens beats md-lynx's pick-the-extremes here: a video's palette is rebuilt every frame, so picked pens recolour the box with every scene and lose contrast in fades, and the quantizer re-maps pixels around held ones, so md-lynx's speckle problem doesn't arise. The video plays on underneath; closing it over a paused video re-decodes the frozen frame from the seek ring |
| `rp/src/jpegdec_core.c` | instantiates `jpegdec/src/jpeg.inl` (bitbank2/JPEGDEC submodule, Apache-2.0) as plain C; exports `mjpeg_jpeg_init()` |
| `rp/src/fb_mono.c` | ST high res: the chunked 320×200 pens as a 640×400 one-bit picture, doubled and blue-noise dithered (see "ST high res"); shared by the hardware and EmuMD builds |
| `rp/src/emul.c` | boot sequence, FOLDER config self-heal, .AVI discovery, main loop (menu, playing, menu over the video), cart-corruption canary |
| `tools/video_to_avi.sh` | ffmpeg wrapper producing the AVI format contract |

## Build & common commands

```bash
make build     # release UF2 -> dist/<uuid>-<version>.uf2 (uuid from uuid.txt)
make debug     # same but DEBUG_MODE=1 (UART logging on; see boot-race lesson)
make uart-log  # attach serial console, log to ./screenlog.0
make tag       # tag HEAD with version.txt and push (release workflow)
make emu       # build for the host and run in Hatari via EmuMD (see below)
```

- UUID resolution: `APP_UUID_KEY` env → `uuid.txt` (gitignored) → dev default
  `44444444-4444-4444-8444-444444444444`. Must match the UUID registered in
  the device's Booster lookup table or the app jumps back to Booster.
- Host toolchain: ARM GNU Toolchain at `/Applications/ArmGNUToolchain/`
  — **15.2.rel1 is installed and works** (older docs said 14.2; both fine).
  Export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
- `<build_type>` only controls `DEBUG_MODE` and stdio; **CMake always builds
  MinSizeRel** (full `Release` historically broke things). Hot compute files
  opt into O3 with a per-file pragma (`jpegdec_core.c`, `mjpeg_quant.c`).
- m68k side builds via `atarist-toolkit-docker` (`stcmd`, needs a PTY;
  `STCMD_NO_TTY=1` is exported by the build scripts). Pure RP-side C changes
  don't require it, but `make build` runs it anyway and regenerates
  `rp/src/include/target_firmware.h` (version hash in the header changes —
  that diff is expected).
- Bump `version.txt` (`vX.Y.Z`) for every flashed build — it shows in the
  UART banner and on the menu's ABOUT row and About page, and makes hardware
  reports traceable. `build.sh` copies it to `rp/` and `target/`, but commit
  all three (a release bumps them together; `target/atarist/version.txt` is
  separate and has stayed at `v1.0.0beta`).
- **Dither A/B flag**: 32×32 blue noise is the default (marginally the best of the
  three tried on hardware); `MQ_DITHER=bayer ./build.sh ...` (or `make build`) builds
  the 2×2 Bayer instead. Read as an env var in `rp/src/CMakeLists.txt` →
  `MQ_DITHER_BLUENOISE` define on `mjpeg_quant.c` (default 1 = blue noise). The
  blue-noise matrix (`rp/src/include/bluenoise.h`, `const` → flash, regenerate with
  `python3 tools/gen_bluenoise.py`) uses 16 dither levels vs the 2×2 tile's 4; both
  drive the same per-level pre-shifted LUTs (`MQ_DITHER_N` entries) and differ only
  in the threshold pattern. Blue noise's LUTs are the larger ones (16 vs 4 entries,
  ~2.7 KB more `.bss`). `build.sh` wipes `dist/` each run, so to keep both UF2s copy
  each out under a distinct name between builds. (A 4×4 Bayer — 16 levels, dottier in
  motion on hardware — was tried and reverted; it's in git history.)
- **Palette hysteresis flag**: on by default; `MQ_HYSTERESIS=off ./build.sh ...`
  builds the raw per-frame palette. Same env→define wiring in CMakeLists →
  `MQ_HYSTERESIS` in `mjpeg_quant.c`. On, `mq_frame_done` slews the published
  palette toward each frame's median-cut target (≤`MQ_HYST_STEP`/channel, holding
  within `MQ_HYST_DEADBAND`) and snaps when the whole-palette change exceeds
  `MQ_HYST_SNAP` (a scene cut) — kills the frame-to-frame shimmer/"pumping" that
  reads as banding in motion. Tune the three constants at the top of the file.
- **Palette saturation/contrast flag**: on by default; `MQ_SATURATE=off ./build.sh ...`
  publishes the raw median-cut centroids. Same env→define wiring in CMakeLists →
  `MQ_SATURATE` in `mjpeg_quant.c`. On, `mq_frame_done` runs each centroid through
  `boost_entry` before the luminance sort — pushing it away from its own grey level
  (saturation ×`MQ_SAT_NUM`/`MQ_SAT_DEN`, default 1.25) and away from mid-grey
  (contrast ×`MQ_CON_NUM`/`MQ_CON_DEN`, default 1.125). Counters the wash-out from
  median-cut box means (each entry is a population average, which pulls toward grey).
  Grey stays grey (chroma-preserving); tune the four constants at the top of the
  file.
- **Histogram-depth flag**: default RGB333 (512 bins, every channel 8 levels);
  `MQ_HIST=g4 ./build.sh ...` builds RGB343 — green gets a 4th bit (16 levels,
  1024 bins) for finer luminance/greyscale palette selection (green is ~59 % of
  luminance), red/blue stay 3-bit. Env→define `MQ_HIST_G4` on `mjpeg_quant.c`,
  which parameterises the dither LUTs, histogram, median cut and map via
  `MQ_GBITS`/`MQ_GMAX`/`MQ_RSH`/`MQ_BINS` (RGB333 compiles byte-identical). Costs
  +1.5 KB `.bss` (hist 2 KB + map 1 KB; heap headroom 19.3→17.7 KB, still safe).
  **Full RGB444 does NOT fit**: a 4096-bin histogram (8 KB) + 4 KB map is +10.5 KB,
  which drops heap headroom to ~8.8 KB — below the settings library's ~14 KB boot
  malloc → OOM panic (lesson 4). Green-4 is the affordable middle ground; RGB443
  (R+G 4-bit, 2048 bins, map parked in the `.uninitialized_data` hole) would be the
  next step up if the extra red precision is wanted and the hole has ~1.7 KB spare.
- There is no test suite. Verification = build + UF2 on hardware + UART
  console + on-screen diagnostics. Offline validation patterns that worked:
  compile `jpeg.inl` + the exact decode/draw code as a host binary against
  the real AVI (see "host_decode_test" pattern), and mirror the quantizer in
  Python to render preview PNGs. Before any of that, the EmuMD build (below)
  runs the whole app in Hatari on the host.

### Video encoding contract

`tools/video_to_avi.sh in.mp4 [out.avi] [quality]`: 320×200 centre-crop-fill,
25 fps MJPEG (baseline yuvj420p) + mono pcm_u8. Audio rate defaults to
**5600 Hz** (YM path; = the 224 bytes/VBL the m68k Timer-B handler consumes);
`STE=1 tools/video_to_avi.sh ...` selects **25033 Hz** for STE DMA sound — the
universal firmware downsamples that for YM on a plain ST, so 25 kHz plays
everywhere and is the right choice if any target is an STE. `hqdn3d` denoise +
`loudnorm` and tight interleave (`-max_interleave_delta 0`) are always applied.
**Quality is
a decode-speed dial**: measured on hardware, q7 ≈ 52–76 ms/frame (~16 fps
effective, ~1/3 frames skipped), q11+denoise ≈ ~30 % faster (~40 ms ≈ 25 fps).
The 16-colour dither masks most of the quality difference — prefer higher q.

### EmuMD: MD/MJPEG in Hatari, on the host

[EmuMD](https://github.com/neilrackett/emumd) (the `emu/emumd` submodule; its
skill is `emu/emumd/skills/emumd/SKILL.md`) builds the RP-side C, JPEGDEC
included, for the host as `build/md-mjpeg.mdfw` and runs it in a patched
Hatari, with the real m68k cartridge (`target_firmware.h`, so whatever the
last `make build` made) on the cartridge port and `emu/sd` as the microSD
card (videos in `emu/sd/mjpeg`, gitignored; symlinks into `test-videos/`
work). `make emu` runs it in a window (`MACHINE=st` for the YM path,
`MONO=1` for a mono monitor: Hatari's `--monitor mono`). For a check without
one:

```bash
emu/emumd/tools/mdfw run --headless --frames 1200 --no-user-config \
    --screenshot build/out.png --log build/run.log -V --timeout 300 \
    -- --cmd-fifo build/run.fifo
```

`-V` shows `DPRINTF` (the `HB:` heartbeat and per-50-frame decode lines
included). Hatari makes the fifo itself; once the log says `Entering main
loop`, `echo "hatari-event keypress 28" > build/run.fifo` presses Return on
the first menu entry (give ESC as `0x01`: a single character is typed as
that character). Add `--record out.avi` to get every frame and the sound;
`ffmpeg -vf "select='not(mod(n\,100))',tile=4x4"` makes a contact sheet.

How it is put together (`mdfw.ini`, `emu/`):
- `template = sidecartridge`: the app's own `main()` runs, and EmuMD stands
  in for `romemul.c`, `commemul.c`, `select.c`, `hw_config.c`, `sdcard.c`
  and the Booster's first run (this app's lookup entry and settings sector,
  since the emulated flash starts blank). The version comes from
  `version.txt`; the UUID is the CMake default (`uuid.txt` is not committed).
  EmuMD's `commemul.c` provides `commemul_scan` too (the audio refill's).
- The firmware's own `fb_chunked.c` runs, on both cores (EmuMD's core 1 is
  a host thread), so Hatari exercises the real dual-core c2p and mono
  split. Its one host-only branch is the Core 1 hand-off: a job and its
  argument are 64-bit pointers there and cannot go through the 32-bit FIFO,
  so under `#if UINTPTR_MAX > UINT32_MAX` they wait in a slot with the FIFO
  as the doorbell. The Thumb workers in `fb_chunked_asm.S` and
  `fb_blit_asm.S` are C in `emu/`.
- `__MCUXPRESSO` is global here (CMake scopes it to the three JPEGDEC
  files; nothing else reads it). The `MQ_*` flags keep their defaults —
  add e.g. `MQ_HIST_G4=1` to `defines` to try one.
- Firmware change for it, a no-op on the RP2040: `fb.c` does the cart
  framebuffer's address arithmetic in `uintptr_t`. The remaining
  pointer-to-`unsigned int` warnings (`main.c`'s debug dump, the
  config-offset maths in `gconfig.c`/`aconfig.c`) are harmless: only the
  low 32 bits of a difference are used.
- What it cannot tell you: anything about time (the host is far faster, so
  the `D` decode times mean nothing and frames never skip), memory (the RAM ceiling,
  heap headroom and the `.uninitialized_data` hole are not modelled), races
  between the cores, or the boot race (lesson 5). Hatari mixes the YM path
  ~16 dB quieter than DMA sound; that is Hatari's levels, not a bug. EmuMD
  keeps the firmware's variables across an ST reset where the RP2040 starts
  afresh.

Scripting it (what worked):
- Machines: `--machine st` for the YM path, `-- --monitor mono` for ST high
  res (both on `mdfw run`); `--sd DIR` points the card anywhere, e.g. a
  scratch folder of symlinks to test long lists or an empty card.
- Wait for `Entering main loop` in the log before sending keys, then pace
  them ~0.3 s apart. Headless runs are fast-forwarded, so a second of host
  time is several seconds of ST time: give `--frames` plenty of slack after
  the last key, or the run ends before the key lands. `command FIFO read
  error: Resource temporarily unavailable` lines are Hatari polling an empty
  fifo — harmless; filter them out of the log.
- Screenshots and recordings include the ST border (palette entry 0, so
  black during playback): crop 832×528 (colour) or 640×400 (mono) to drop
  Hatari's status bar.
- Sound: compare recordings rather than listening — the loudness envelope
  of a run lines up with a reference run's (cross-correlation ~0.99 for the
  same soundtrack), and a speed error shows as a lag that grows from an
  early window to a late one. **Only compare runs made side by side** (same
  script, started together): fast-forwarded runs made at different times
  drift against each other by a second or more over 30 s with the host's
  load, which once looked exactly like a firmware regression. Two builds run
  together agreed to within ~20 ms.
- To A/B a code change, build a throwaway variant to a copy of the `.mdfw`
  (`mdfw run FILE.mdfw` runs it without rebuilding), restore the source, and
  run both on the same script; matched frames can be found by comparing
  downscaled greyscale frames.

## Hard-won lessons (from bringing this up on hardware)

Things that cost real debugging time. Do not re-learn them.

1. **The m68k executes its code over the cartridge bus, continuously.**
   Anything that corrupts the 64 KB `ROM_IN_RAM` region (RP `0x20030000`,
   ST `$FA0000`) crashes the ST instantly — symptoms range from silent black
   screens to bombs, differing per boot. `emul.c`'s `cart_check()` canary
   compares the served header against `target_firmware[]` at boot stages and
   every main-loop pass; keep it.

2. **Heap grows into the cart region unless capped.** The linker's
   `__StackLimit` (pico-sdk's `_sbrk` ceiling — "poorly named, historically
   maximum heap ptr") originally included ROM_IN_RAM. The settings library
   mallocs ~14 KB at boot (4 KB flash-image buffer + defaults table, ×2
   contexts) and pico-sdk **panics on any malloc failure** (`Out of
   memory`). With statics near the RAM limit, those allocations silently
   overwrote the m68k code. Fixed: `__StackLimit = ORIGIN(RAM)+LENGTH(RAM)`
   in `memmap_rp.ld`. **Heap headroom is load-bearing: keep ≥ 20 KB free**
   between `__bss_end__` and `0x20030000` (check the map after adding
   statics; FatFs LFN ops malloc ~0.5 KB each on top of settings). It has
   sat just under that since the menu and colour work: **19,144 B at
   v1.1.0**, still well clear of the ~14 KB boot malloc — don't let it slide
   further.

3. **`target_firmware[]` must be 4-byte aligned.** `COPY_FIRMWARE_TO_RAM`
   uses the RP2040 XIP-stream DMA, and `XIP_STREAM_ADDR` silently truncates
   to a 32-bit boundary. A `const uint16_t[]` is only 2-aligned, so whether
   the image copied correctly was **per-build linker-layout luck**; a
   2-mod-4 placement shifts the whole cart image by 2 bytes (the canary
   shows the tail of a neighbouring `.rodata` string as the first word).
   `target/atarist/firmware.py` now emits `__attribute__((aligned(4)))`.
   Both this and lesson 2 are template bugs worth upstreaming.

4. **RAM is nearly full — use the alignment hole.** RAM region =
   `0x20000000`, **192 KB**, cart mirror at `0x20030000` (an old version of
   these notes said 128 K / 0x20020000 — the linker script is the truth).
   Big consumers: `fb_chunked_buffer` 64 000 + `fb_planar_scratch` 32 000 +
   commemul ring 32 768 (+ the app's ring/tables). Because `.bss` sorts by
   alignment and the commemul ring needs 32 KB alignment, `.bss` starts at
   `0x20008000` leaving a **~22 KB dead gap** after `.data`. Park large
   buffers there with pico-sdk's `__uninitialized_ram(name)` (only JPEGIMAGE
   `s_jpg` ~18 KB lives there now; the dither LUTs and `hist`/`map9` are in
   ordinary `.bss`). Rules: contents are NOT zeroed at boot (initialize
   explicitly), and the gap contents must stay below `0x20008000` or `.bss`
   jumps 32 KB. **With only `s_jpg` in the hole the margin is comfortable
   (~2.8 KB at v1.1.0: it ends at `0x200074F0`)** — the LUTs were pulled out to `.bss` when the menu's statics
   tipped the then-packed hole past the boundary (jumping `.bss` a full
   32 KB and overflowing RAM); see the `.uninitialized_data` note in
   `mjpeg_quant.c`. Overflowing it is caught by the linker (`.bss will
   not fit in region RAM`), NOT silent — but the build script still prints
   "Build completed successfully" on a failed RP link, so after any static
   growth verify `[100%] Built target rp` / `Linking ... rp.elf` appeared
   and re-check `__bss_end__` in the map.

5. **Debug builds lose the cartridge boot race.** `DEBUG_MODE=1` prints
   ~6 KB of settings/lookup dumps at 115200 baud *before* `emul_start()`
   brings the cart online — TOS (especially on warm reset, which is how
   Booster launches apps) probes the cartridge first and boots to desktop.
   Don't fight it: for diagnostics, ignore the ST entirely and read the
   UART — the main loop logs an `HB:` heartbeat + player counters every 3 s
   regardless. Release builds have no such window.

6. **The ST screen can't be trusted as a diagnostic when the m68k is dead**
   (it freezes showing the last blitted frame, palette register untouched,
   ESC dead because IKBD forwarding runs inside the m68k blit loop). The
   layered diagnostics that finally worked: the UART `HB:` heartbeat (RP
   liveness, with the player counters `F` shown / `S` skipped / `I` index /
   `E` last JPEG error / `R` ring bytes / `D` decode µs / `A` audio
   refills / `L` refill bytes), the menu (the
   version on its ABOUT row, the detected sound path on the About page —
   m68k + fb path liveness) and the cart canary (region integrity). Keep
   them. (An on-screen stall screen that painted the counters went with the
   menu work in `9cdfdbc`; the heartbeat carries them now.)

7. **Per-app config sectors are recycled and can hold stale junk.** This
   app's sector arrived containing `FOLDER=/test` (the upstream template
   default) — and `sdcard_initFilesystem()` *creates* whatever folder it's
   given. `emul_start()` self-heals missing/`/test` FOLDER values to
   `/mjpeg` (before Core 1 launches, so the flash write is safe) but leaves
   genuine custom values alone; the .AVI search also falls back
   `config folder → /mjpeg → /`.

8. **Audio ring sizing is empirical.** ffmpeg's default interleave muxes
   audio in ~0.5 s lumps *after* the video frames they accompany, and a
   worst-case decode blocks demuxing for ~76 ms: with that, a 4 KB ring
   measurably underran (audible dropouts, `R0` in the log) and needed 8 KB.
   `tools/video_to_avi.sh` now interleaves tightly (`-max_interleave_delta
   0`, ~100 ms chunks), which is what lets the ring be back at **4 KB with
   3 KB prefill** (`ARING_SIZE`/`ARING_PREFILL`) — files muxed the old way
   may underrun. `mjpeg_open()` pre-scans movi to prime the ring, then skips
   those bytes inline.

9. **JPEGDEC as plain C**: `jpeg.inl` compiles as C when `JPEGDEC.h` sees a
   non-Arduino platform macro — we define `__MCUXPRESSO` scoped to the three
   files that include it (see `set_source_files_properties` in
   `rp/src/CMakeLists.txt`). `JPEGInit` is hard-static, hence the
   `mjpeg_jpeg_init()` export. No malloc, no big stack frames (~1.6 KB worst
   chain measured; core-0 stack is 4 KB in SCRATCH_Y). The whole decode path
   (windowed FatFs read/seek callbacks on an AVI chunk + draw callback
   geometry) is host-testable — compile `jpeg.inl` + the same callback code
   on macOS against the real file.

10. **fb_publish waits ≤ ~60 ms even with no m68k**, so nothing on the RP
    side deadlocks when the ST is at the desktop — playback free-runs at
    ack-timeout rate. Useful for UART-only testing.

## Architecture (template layer)

The firmware is a two-target build: m68k assembly (`target/atarist/`) is
compiled into a ROM image, embedded as a C array (`target_firmware.h`) in
the RP2040 firmware, and served back to the Atari over the cartridge bus
emulated via PIO + DMA.

### Framebuffer pipeline

1. RP draws into `fb_chunked_buffer` (320×200 bytes, palette index in low
   nibble) — the MJPEG draw callback writes it directly.
2. `fb_publish()` transposes chunky→planar into a RAM scratch (hand-written
   Thumb asm, split across both cores, ~1 ms), waits for the m68k's per-VBL
   ack (cart-bus read captured by commemul; ~60 ms timeout), then does a
   ~120 µs chunk-reversed copy into the cart FB at `$FA8300` and bumps
   `FB_FRAME_COUNTER`. Tear-free by construction.
3. The m68k `userfw.s` VBL loop blits cart FB → ST screen page with a fully
   unrolled MOVEM copy (predec, ~17 ms), flips the video base between
   `$70000`/`$78000`, applies the RP-published palette every VBL, and emits
   the VBL ack. **It blits every VBL unconditionally** and polls the IKBD
   ACIA inside the blit macro — so keyboard forwarding to the RP only works
   while the m68k loop runs.
4. ESC handling: RP-side `ikbd.c` decodes scancodes from the commemul ring;
   ESC press+release within 200 ms writes `CART_CMD_BOOT_GEM` to the cart
   sentinel; the m68k polls it each frame and exits cleanly to GEM.

### ST high res (a mono monitor)

Ported from md-doom's mono support. `main.s` lets high res through, and
`userfw.s` reads the shifter at boot (`UFW_MONO`): colour 0 is set once to
white paper (a set bit is black ink) and the palette copy is skipped. The
cart FB is the same 32,000 bytes in the same chunk-reversed layout — 400
rows of 80 bytes — and the same copy, but mono VBLs are 71.47 Hz (14 ms)
and the copy takes ~17.6 ms, so **a mono frame is two VBLs**: the first
copies `FBDRV_ITERS_A` chunks, sets `UFW_HALF`, skips the flip and reads
`$FB8401` (`VBLSYNC_HALF_ADDR`) instead of the frame ack (half the FB is
still unread; the audio refill keys off either); the next copies the rest,
flips and acks at `$FB8400` as usual. The display is reported every VBL at `$FB9000 + mono` →
`fb_st_mono()` and `audio_set_mono()`. Display rate is up to 35.7 fps, so
25 fps video is unaffected.

RP side: `fb_publish()` calls `fb_transpose_mono(levels)` instead of the
c2p — `fb_mono.c` doubles each pixel and dithers it at 640×400 against the
same 32×32 blue noise, a pen's level 0..16 (0 ink, 16 paper) coming from a
16-entry table: `fb_publish()` uses an even grey ramp (pen 0 black to 15
white), and `fb_publish_levels()` takes another — the menu's
`MenuMonoLevels` (the screen a 50% grey dither, the box black, ink white —
black and white alone lose the box against the screen). Per source row it
builds each pen's ink bits from the blue-noise rows (`ink_masks`, in
flash), so a pixel costs one lookup and an OR: roughly 1.5 ms on both
cores by the compiled code, against ~7 ms for the first cut's four compares
a pixel (which ran out of registers) — not yet measured on hardware. The
per-pixel loop is RAM-resident like the c2p (284 B); keep the once-a-row
setup out of it, or O3 inlines and unrolls it to 1.6 KB of RAM. In mono `decode_frame()` runs JPEGDEC luma-only
(`EIGHT_BIT_GRAYSCALE`, which skips the chroma IDCT and colour conversion)
into `mq_draw_mono`, which writes 16 linear-light grey pens (Y² for the
gamma: a mono monitor mixes light) and skips the quantizer. The time overlay
is unchanged (pen 0 box, pen 15 text = black and white). Audio: a refill
every VBL (14 ms), ~350 DMA bytes (the measured length starts at
`STE_SND_LEN_INIT_MONO` and is steered only on the VBL that ends a frame)
or 79 YM pairs (Timer-B takes ~78.2 a VBL); on the YM path the video clock
advances 350 source samples a refill (`MJPEG_SRC_PER_VBL_MONO`).
**Untested on an SM124**: EmuMD plays it in Hatari (`make emu MONO=1`), and
the timing claims are md-doom's arithmetic and Hatari, not hardware.

### Audio pipeline

RP fills a 1 KB cart buffer (`$FA4100`) with (vA, vB) YM volume pairs each
VBL (224 B consumed per VBL); m68k Timer-B (~5585 Hz) plays them into the
YM2149 volume registers; the VBL handler resets the read cursor (A0) to the
buffer base every frame. MD/MJPEG converts u8 PCM → pairs at runtime through
the 64-entry Ghostbusters LUT (`ghost_lut` in mjpeg.c; same table as
`tools/wav_to_ym4.py --mode dual-ghost`).

On an STE the firmware instead uses **DMA sound** (25 kHz, 8-bit signed): the
RP passes the AVI's PCM through unchanged and `userfw.s` plays it from a
double buffer in the two screen-page tails (`$77D00`/`$7FD00`), copying into
the one the chip isn't playing. The path is auto-detected at boot (`_SND`
cookie, reported to the RP each VBL via `fb_rom3_dispatch` →
`audio_set_mode`); on a plain ST the same 25 kHz stream is box-averaged down
for the YM callback (500 source samples a VBL, 350 at mono's 71 Hz).

**Ported from MD/DOOM in v1.1.0, all three found on hardware** — on a Mega
STE here, sound for a few seconds, then silence for 2:49, noise bursts and
buzzes:
- **The DMA's START/END are re-pointed from a Timer-A interrupt**
  (`userfw_snd_irq`, event-count mode: the chip's frame end is Timer-A's
  input), never from the VBL loop. The chip latches them at each frame end
  and the six byte writes aren't atomic; from the VBL loop, a frame end
  drifting into that window latched one buffer's START with the other's
  END — 32 KB of screen memory as ~1.3 s of noise, or START past END and
  the chip running round the whole 4 MB DMA address space (167 s at
  25,033 Hz: the 2:49). The MFP is in auto-EOI mode on both sound paths,
  as the handler has no in-service ack.
- **The DMA length is measured.** The chip eats ~501.5 samples a VBL (its
  own oscillator, not the video's), so a fixed 500 ran a buffer dry and
  replayed it every few seconds. `.after_copy` steers `UFW_SND_LEN` ±1
  every fourth frame from how the frame counter's offset drifts, and
  reports it each VBL at `$FB8C00 + len - 320` → `audio_sndlen_report()`;
  the fill produces exactly that many samples and the video clock advances
  by them, so A/V follows the sound actually played.
- **The RP refills from a 1 ms timer on Core 1** (`audio_start_vbl_timer`,
  an alarm pool created on Core 1), which peeks at the ROM3 ring
  (`commemul_scan`, not consuming it) for the m68k's `$84xx` ack and fills
  right after it — a whole VBL before the m68k copies again, however busy
  Core 0 is (decoding, or waiting in `fb_publish`). The old main-loop refill
  paced itself off the RP's clock, so it slipped against the VBL. The fill
  callback runs in that interrupt under `audio_lock()`, so the player takes
  the lock to reset its PCM ring or clock (open, close, seek), the ring
  indices are volatile, and it must never touch the SD card (the
  template's `.YMS` streamer went for that reason). MD/DOOM measured the
  same timer on Core 0 stalling compute for 100–200 ms at a time — keep it
  on Core 1.

The heartbeat's `A` (refills since the last line: ~150, ~214 in mono) and
`L` (the refill size, i.e. the DMA length it settled on) show it working.
On the YM path the refill lands while Timer-B is near the end of the
buffer, so the last few samples of a VBL come from the next one (MD/DOOM
has the same, as a known issue).

### Shared 64 KB cartridge region (`$FA0000` ↔ RP `0x20030000`)

Offsets are defined in `rp/src/include/cart_shared.h` (RP) and
`target/atarist/src/main.s` (m68k) — never hard-code addresses.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 16 KB | m68k header + main.s + userfw.s (read-only from m68k) |
| `$FA4000` | `CMD_MAGIC_SENTINEL` | 4 B | RP→m68k command word (ESC exit etc.) |
| `$FA400C` | `FB_FRAME_COUNTER` | 4 B | RP-incremented dirty-frame counter |
| `$FA4010` | `SHARED_VARIABLES` | 240 B | 60 × 4 B slots |
| `$FA4040` | `PALETTE` | 32 B | 16 ST palette words, applied by m68k every VBL |
| `$FA4100` | `AUDIO_BUFFER` | 1 KB | YM sample pairs |
| `$FA4500` | `APP_FREE` | ~15.5 KB | free arena |
| `$FA8300` | `FRAMEBUFFER` | 32 000 B | 320×200×4bpp planar FB (640×400×1bpp in mono), chunk-reversed for the MOVEM copy |

The m68k cannot write the region, so it talks to the RP by **reading** ROM3
(`$FB0000`–`$FBFFFF`); commemul captures the low 16 address bits and
`fb_rom3_dispatch` decodes them:

| Read | Meaning |
| --- | --- |
| `$FB8200 + byte` | IKBD byte (`ikbd.c`) |
| `$FB8400` | frame ack: the copy is done, the cart FB may be rewritten (in mono only on the VBL that ends a frame); refills the audio |
| `$FB8401` | mono, the VBL after a frame's first half: refills the audio, but not a frame ack |
| `$FB8600 + dma` | sound path, every VBL (0 YM, 1 STE DMA) |
| `$FB8C00 + len - 320` | STE DMA sound: the bytes the chip eats per VBL, as measured (every VBL) |
| `$FB9000 + mono` | display, every VBL (1 = ST high res) |

Odd low bytes do reach the RP: the IKBD window and the `+ 1` reports depend
on it.

### RP2040 memory map (`rp/src/memmap_rp.ld`)

| Region | Origin | Length | Notes |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | app code (XIP) |
| `ROM_TEMP` | `0x10100000` | 128 K | scratch |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | never write |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | per-app config sectors |
| `GLOBAL_LOOKUP/CONFIG` | `0x101FE000` | 8 K | UUID lookup + global config |
| `RAM` | `0x20000000` | **192 K** | `.data`/hole/`.bss`/heap (see lesson 4) |
| `ROM_IN_RAM` | `0x20030000` | 64 K | cart region — heap must never touch it |
| `SCRATCH_X/Y` | `0x20040000` | 4 K + 4 K | core1 / core0 stacks |

Core 0 runs the app at 225 MHz / 1.10 V; Core 1 is owned by the c2p worker
(`fb_core1_dispatch` is the only sanctioned way to borrow it). Flash writes
only from Core 0 (`PICO_FLASH_ASSUME_CORE0_SAFE=1`).

### Plumbing modules (leave alone unless needed)

`main.c` (boot + config init — never add features here), `romemul` (ROM4
PIO+DMA read engine), `commemul` (ROM3 capture ring; drained via
`fb_pump_rom3()`), `sdcard`/`hw_config` (FatFs; project config override at
`rp/src/ff/ffconf.h` wins over the submodule via include order),
`gconfig`/`aconfig` (settings; gconfig defaults mirror Booster — don't trim
the WiFi params), `select`, `reset`, `fb_*`, `palette`, `audio`, `ikbd`,
`font8x8`.

**Platform-level reference** (dev-env setup, VS Code, the full generic
RP↔m68k command framework `tprotocol.h`/`chandler`, and the `network.c`/
`httpc`/`download.c` WiFi stack) lives upstream in **`md-microfirmware-template`**
(and, for the download bits, in the deferred-download memory note). MD/MJPEG
uses none of that command framework — it decodes ROM3 reads directly in
`fb_rom3_dispatch` (a magic-word sentinel + fixed windows, no header/checksum
protocol), and links no cyw43/lwIP. This repo's own `programming.md` (a copy
of the template's platform guide) was removed as an upstream duplicate.

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| ST bombs at boot, or black screen + dead ESC, RP log healthy | Cart region corrupted or mis-copied. Check UART for `CART REGION CORRUPTED`; verify `target_firmware` address ends in 0/4/8/C in the map (lesson 3); check heap headroom (lesson 2) |
| `*** PANIC *** Out of memory` on UART | Heap exhausted — pico-sdk panics on failed malloc. Grow headroom (lesson 4) |
| Debug build "loads as normal" straight to desktop | Boot race (lesson 5) — diagnose over UART, or flash release |
| Screen frozen on boot pattern, default colours | RP died/panicked before `mq_init` (palette never changed) or m68k died — check UART heartbeat to tell them apart |
| `E<n>` non-zero in the `HB:` heartbeat | Decoder erroring; `E` = JPEGDEC error code |
| Video races ahead / skips at 2-4× speed, sound too fast | A 5,600 Hz AVI (the encoder's default): see Known issues — encode with `STE=1` |
| Mono: tearing or half-old frames | The VBL after the first half must not ack — `$FB8400` only once the whole frame is copied (`UFW_HALF` in `userfw.s`; that VBL reads `$FB8401`, which `fb.c` must not count) |
| STE DMA: sound stops for ~2:48 then returns, or ~1 s bursts of noise | The DMA's START/END written outside `userfw_snd_irq` (see "Audio pipeline") |
| STE DMA: a crackle every few seconds | The DMA length not steered, or the RP not producing `L` bytes; check `L` in the heartbeat moves off 500 |
| Audio crackle/dropouts every ~0.5 s | PCM ring underrun — check `R` in heartbeat, see lesson 8 |
| Choppy video, many skips | Decode over 40 ms/frame — re-encode at higher `-q` (speed dial), see encoding contract |
| `the input device is not a TTY` from stcmd | Export `STCMD_NO_TTY=1` (build scripts already do) |
| Harmless VASM warnings (`target data type overflow`) | Ignore |
| ST shows garbage but RP works | Stale `target_firmware.h` — confirm `target/atarist/dist/BOOT.BIN` was regenerated by the current build |

## Known issues

- **5,600 Hz AVIs play too fast** (since `9e04d06`, the universal DMA
  sound). `mjpeg_audio_fill()` advances the video clock by a fixed
  `MJPEG_SRC_PER_VBL` (500, or 350 in mono) source samples a refill, which
  is right only for 25,033 Hz audio; the DMA path also passes source bytes
  straight through, assuming that rate. A 5,600 Hz file (what
  `tools/video_to_avi.sh` makes without `STE=1`, and what the README still
  calls the default) runs ~2–4× fast with frames skipped, on both sound
  paths — checked in EmuMD. The fix is to take source samples per refill
  from the AVI's rate (`arate` / VBL rate) and resample to the output count
  on both paths; until then, encode with `STE=1`.
- **ST high res is untested on an SM124** (see "ST high res").
- **`_` is blank in `font8x8.h`**, so underscores in file names show as
  spaces in the menu.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, `fatfs-sdk/`, `jpegdec/`,
  `emu/emumd/` — submodules, all re-pinned on every build (`rp/build.sh`
  runs `git submodule update --init --recursive`, which checks each out
  at the commit in the index). FatFs config changes go in
  `rp/src/ff/ffconf.h`; JPEGDEC quirks are absorbed in `jpegdec_core.c` /
  CMake, not the submodule. For EmuMD, change it upstream and update the
  submodule: check out the new commit in `emu/emumd` and `git add
  emu/emumd` before the next `make build`, or the build puts it back.
  Then delete any glue in `emu/` that the newer EmuMD provides (its
  `docs/GUIDE.md` lists them; the link fails with a duplicate symbol
  until you do).
- Don't add features to `main.c` — app code lives in `emul.c` + `mjpeg*`.
- Match existing C style (`.clang-format` / `.clang-tidy` are wired into
  CMake when the binaries are on PATH).
- New/modified source files get the SPDX header:
  `Copyright (C) 2026 Neil Rackett` / `SPDX-License-Identifier: GPL-3.0-or-later`.
- After any change that adds statics, check the map: `__bss_end__` headroom
  vs `0x20030000` (≥ 20 KB target; 19,144 B at v1.1.0, see lesson 2) and that
  `.uninitialized_data` still ends below `0x20008000`.
- **No AI attribution anywhere** — no `Co-Authored-By: Claude`, no
  "Generated with…" in commits, PRs, comments, docs or changelogs. Write as
  the human author. (Also: no `Co-Authored-By` trailers at all.)

## Working style

These bias toward caution over speed; use judgment for trivial tasks.

1. **Think before coding** — state assumptions; if multiple interpretations
   exist, present them; push back when a simpler approach exists.
2. **Simplicity first** — minimum code that solves the problem; no
   speculative features, abstractions or configurability.
3. **Surgical changes** — touch only what the task requires; match existing
   style; mention unrelated dead code, don't delete it.
4. **Goal-driven execution** — define the success check before the change;
   for this repo that usually means: build passes, map budgets hold, and a
   hardware test (UART log + screen) confirms behaviour. When hardware isn't
   available, mirror the exact logic in a host build or Python sim and
   validate against real data (this caught/ruled out entire bug classes).
