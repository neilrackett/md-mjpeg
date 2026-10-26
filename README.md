# MD/MJPEG

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

<img src="desc/ed.webp" width="640" height="400" alt="Poor quality photo of Elephants Dream playing on Atari ST" />

MD/MJPEG turns your SidecarTridge Multi-device into a video player for your Atari ST: drop an `.AVI` file onto the SD card and it plays full-screen, full-motion video with sound on a stock ST — no hard drive, no accelerator, no extra hardware.

Why? Because why watch videos at 4K resolution in 16.7 million colours when you can watch them at 0.3K in 16!

Every frame is decoded from MJPEG on the SidecarT's RP2040, quantized to a fresh 16-colour palette (median cut, blue-noise dither and a saturation boost, so each frame gets the best colours the ST can show), and blitted to the screen over the cartridge port at up to 25 fps. Sound is 8-bit 25 kHz DMA audio on an STE, or the YM2149 (~5.6 kHz) on a plain ST — the firmware detects which machine it's on and picks the best it can do. Video and audio stay in sync by design: the audio clock paces the video, and busy scenes drop frames rather than drift.

When it starts, the app lists the `.AVI` files in the `/mjpeg` folder of the SD card, below **About** and **Exit to GEM**. Use **Up/Down** to choose one and **Return** to play it. During playback, **Left/Right** skip back/forward 10 seconds and **Space** pauses and resumes (a time readout pops up briefly when you seek or pause). **ESC** opens the list over the video, which carries on playing underneath: pick another video, or press **ESC** again to go back to it. When a video reaches the end it returns to the list. From the list, **Exit to GEM** quits back to GEM, as does **ESC** when nothing is playing.

## Installation

1. Download the latest files from the releases page.
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. On the Booster screen, press ESC for the app list and select the MD/MJPEG app.
4. Copy a converted `.AVI` file (see below) into the `/mjpeg` folder of the SD card.
5. To return to Booster, power on while holding SELECT.

### Why Motion MJPEG (MJPEG)?

MJPEG was chosen because it's a perfect fit for the SidecarT's hardware constraints: each frame is a self-contained JPEG image that can be decoded independently without requiring complex motion prediction or reference frames. This means the RP2040 can stream video at 25 fps by simply decoding one JPEG per frame, which is far simpler and more reliable than full-motion codecs like H264 or H265 that need to maintain state between frames.

## Converting a video

Videos need converting to the format the firmware streams: 320×200 MJPEG at 25 fps with 8-bit mono PCM audio, in a standard AVI container. The `tools/video_to_avi.sh` script does it all with [ffmpeg](https://ffmpeg.org) (`brew install ffmpeg` on macOS, `apt install ffmpeg` on Debian/Ubuntu).

For example, to convert the open-movie classic [Elephants Dream](https://archive.org/download/ElephantsDream):

```bash
# 1. Download the source video (any resolution works; it gets
#    centre-cropped and scaled to fill the ST's 320x200 screen)
curl -LO https://archive.org/download/ElephantsDream/ed_1024_512kb.mp4

# 2. Convert it
tools/video_to_avi.sh ed_1024_512kb.mp4 ED.AVI

# 3. Copy ED.AVI to the /mjpeg folder of your SidecarT's SD card
```

The output is a normal AVI — you can preview it in VLC or any desktop player before copying it over.

An optional third argument sets the MJPEG quality (2 = best, 31 = worst, default 9). It's a speed dial as much as a quality dial: the RP2040 decodes every frame in real time, so _lower_ numbers make frames slower to decode and can cause dropped frames, while the ST's 16-colour dither hides most of the difference anyway. If playback looks choppy, try a higher number (e.g. 11).

By default the audio is encoded at 5.6 kHz for the YM2149. If you have an STE, prefix `STE=1` (e.g. `STE=1 tools/video_to_avi.sh in.mp4 out.avi`) to encode 25 kHz audio for the STE's DMA sound chip — it still plays on a plain ST (downsampled), so it's the safe choice if any of your machines is an STE.

## Running it on your computer

You can also run MD/MJPEG on your Mac or Linux PC, no SidecarT required, using [EmuMD](https://github.com/neilrackett/emumd) in the `emu/emumd` submodule. It builds the firmware for your computer and runs it in a version of [Hatari](https://www.hatari-emu.org) with a Multi-device on its cartridge port, with a folder standing in for the microSD card. You'll need the tools in [EmuMD's README](https://github.com/neilrackett/emumd#getting-started), and your converted videos in `emu/sd/mjpeg`, then:

```bash
git submodule update --init emu/emumd  # once, if you cloned without it
emu/emumd/tools/mdfw hatari            # once: builds EmuMD's Hatari, downloads EmuTOS
make emu                               # builds and runs MD/MJPEG in Hatari (an STE)
make emu MACHINE=st                    # a plain ST, with YM sound
make emu MONO=1                        # a mono monitor (ST high res)
make emu RECORD=mjpeg.avi              # records picture and sound until you quit
```

EmuMD boots EmuTOS unless you give it another TOS image with `TOS=/path/to/tos.img`. Add `-V` to `emu/emumd/tools/mdfw run` to see the firmware's debug output. It's great for testing the menu, the AVI parser, the colour pipeline and the code that talks to the ST, but the emulated Multi-device has all the speed and memory it wants, so decode times, dropped frames and running out of RAM still need real hardware.

## Known limitations

- On a mono monitor (ST high res) videos play in black and white, dithered to 640×400. This hasn't been tried on a real SM124 yet.
- Sound is mono. An STE plays 8-bit 25 kHz DMA audio; a plain ST falls back to the YM2149 (~5.6 kHz, effectively 6-bit — think telephone, not hi-fi).
- Very busy scenes can exceed the per-frame decode budget; the player skips frames to keep the audio smooth and the picture in sync.
- The menu lists up to 20 files.

## What's next?

A few ideas we've had (no promises we'll do them all):

- Auto-downloading an example video when the SD card is empty?
- Palette tricks to go beyond 16 colours?
- Overscan to go full screen?

Think you can help? Got an idea of your own? We'd love to hear from you, so why not let me know on [X](https://x.com/neilrackett) or submit a PR.

## Credits

Built from the [md-framebuffer-template](https://github.com/sidecartridge/md-framebuffer-template) for the SidecarTridge Multi-device, with JPEG decoding by Larry Bank's excellent [JPEGDEC](https://github.com/bitbank2/JPEGDEC) (Apache-2.0).

## License

The source code of the project is licensed under the GNU General Public License v3.0. The full license is accessible in the [LICENSE](LICENSE) file.
