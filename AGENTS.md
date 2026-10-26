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
quantizes each frame to its own 16-colour ST palette (median cut over an
exact RGB333 histogram + blue-noise ordered dither + a saturation/contrast
boost), and blits it to the ST screen over the cartridge bus at up to 25 fps.
Audio is universal: the firmware auto-detects STE DMA sound (25 kHz 8-bit)
and falls back to YM2149 (~5.6 kHz) on a plain ST. A/V sync is audio-master:
the fill callback's sample clock paces video; when decoding falls ≥2 frames
behind, frames are skipped so audio never stalls.

Built from **md-framebuffer-template** (the framework does the heavy
lifting): dual page-flipped framebuffer on the ST side, real 50 Hz VBL lock,
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
| `rp/src/jpegdec_core.c` | instantiates `jpegdec/src/jpeg.inl` (bitbank2/JPEGDEC submodule, Apache-2.0) as plain C; exports `mjpeg_jpeg_init()` |
| `rp/src/emul.c` | boot sequence, FOLDER config self-heal, .AVI discovery, main loop, stall detector, cart-corruption canary |
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
- Bump `version.txt` (`v0.x.y`) for every flashed build — it shows in the
  UART banner and on-screen status line and makes hardware reports traceable.
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
work). `make emu` runs it in a window (`MACHINE=st` for the YM path). For a
check without one:

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
- `fb_chunked.c` is replaced by `emu/fb_chunked.c`: Core 1 jobs and their
  arguments are 64-bit pointers on the host and cannot go through the
  32-bit FIFO, so they wait in a slot with the FIFO as the doorbell, and the
  transpose runs on one core. The Thumb workers in `fb_chunked_asm.S` and
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
   statics; FatFs LFN ops malloc ~0.5 KB each on top of settings).

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
   (~4 KB)** — the LUTs were pulled out to `.bss` when the menu's statics
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
   layered diagnostics that finally worked: UART heartbeat (RP liveness),
   on-screen status/breadcrumbs incl. version string (m68k + fb path
   liveness), cart canary (region integrity), stall detector painting player
   counters (`F` shown / `S` skipped / `I` index / `E` last JPEG error /
   `R` ring bytes / `D` decode µs). Keep all of them.

7. **Per-app config sectors are recycled and can hold stale junk.** This
   app's sector arrived containing `FOLDER=/test` (the upstream template
   default) — and `sdcard_initFilesystem()` *creates* whatever folder it's
   given. `emul_start()` self-heals missing/`/test` FOLDER values to
   `/mjpeg` (before Core 1 launches, so the flash write is safe) but leaves
   genuine custom values alone; the .AVI search also falls back
   `config folder → /mjpeg → /`.

8. **Audio ring sizing is empirical.** The fill callback drains 112
   samples/VBL; ffmpeg interleaves audio in ~0.5 s (2800 B) lumps muxed
   *after* the video frames they accompany, and a worst-case decode blocks
   demuxing for ~76 ms. A 4 KB ring measurably underran (audible dropouts,
   `R0` in the log); 8 KB with ~5.6 KB prefill is safe. `mjpeg_open()`
   pre-scans movi to prime the ring, then skips those bytes inline.

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

### Audio pipeline

RP fills a 1 KB cart buffer (`$FA4100`) with (vA, vB) YM volume pairs each
VBL (224 B consumed per VBL); m68k Timer-B (~5585 Hz) plays them into the
YM2149 volume registers; the VBL handler resets the read cursor (A0) to the
buffer base every frame. MD/MJPEG converts u8 PCM → pairs at runtime through
the 64-entry Ghostbusters LUT (`ghost_lut` in mjpeg.c; same table as
`tools/wav_to_ym4.py --mode dual-ghost`). `audio_render_frame()` self-paces
to ~50 Hz and must be called at least once per VBL — during long decodes the
JPEGDEC draw callback calls `mjpeg_service()` (rom3 pump + ikbd + audio)
every band, which is what keeps audio glitch-free.

On an STE the firmware instead uses **DMA sound** (25 kHz, 8-bit signed): the
RP passes the AVI's PCM through unchanged and `userfw.s` plays it from a
double buffer in the two screen-page tails (`$77D00`/`$7FD00`), swapping on
the DMA frame counter so the m68k write never crosses the read pointer. The
path is auto-detected at boot (`_SND` cookie, reported to the RP each VBL via
`fb_rom3_dispatch` → `audio_set_mode`); on a plain ST the same 25 kHz stream
is box-averaged down for the YM callback. Full detail in the DMA-sound memory
note; both paths consume 500 source samples/VBL so A/V pacing is identical.

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
| `$FA8300` | `FRAMEBUFFER` | 32 000 B | 320×200×4bpp planar FB |

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
| `MD/MJPEG STALLED F0 ... E<n>` on screen | Decoder erroring; `E` = JPEGDEC error code |
| Audio crackle/dropouts every ~0.5 s | PCM ring underrun — check `R` in heartbeat, see lesson 8 |
| Choppy video, many skips | Decode over 40 ms/frame — re-encode at higher `-q` (speed dial), see encoding contract |
| `the input device is not a TTY` from stcmd | Export `STCMD_NO_TTY=1` (build scripts already do) |
| Harmless VASM warnings (`target data type overflow`) | Ignore |
| ST shows garbage but RP works | Stale `target_firmware.h` — confirm `target/atarist/dist/BOOT.BIN` was regenerated by the current build |

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, `fatfs-sdk/`, `jpegdec/`,
  `emu/emumd/` — submodules (the first three are re-pinned on every build).
  FatFs config changes go in `rp/src/ff/ffconf.h`; JPEGDEC quirks are
  absorbed in `jpegdec_core.c` / CMake, not the submodule. For EmuMD,
  change it upstream and update the submodule.
- Don't add features to `main.c` — app code lives in `emul.c` + `mjpeg*`.
- Match existing C style (`.clang-format` / `.clang-tidy` are wired into
  CMake when the binaries are on PATH).
- New/modified source files get the SPDX header:
  `Copyright (C) 2026 Neil Rackett` / `SPDX-License-Identifier: GPL-3.0-or-later`.
- After any change that adds statics, check the map: `__bss_end__` headroom
  vs `0x20030000` (≥ 20 KB) and that `.uninitialized_data` still ends below
  `0x20008000`.
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
