#!/usr/bin/env bash
#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/video_to_avi.sh
#
# Convert any ffmpeg-readable video into the .AVI format the MD/MJPEG
# firmware streams from the SD card:
#
#   video: MJPEG (baseline, yuvj420p), 320x200 @ 25 fps, centre-cropped
#          (not letterboxed) to the 16:10 frame
#   audio: unsigned 8-bit PCM, mono, 5600 Hz -- exactly the 224 bytes
#          per VBL the m68k Timer-B handler consumes, loudness-
#          normalised (the YM2149 path is effectively 6-bit, so quiet
#          tracks would otherwise vanish)
#
# The output is a plain AVI: verify it in any desktop player, then
# copy it into the app folder on the SD card (default /mjpeg). The
# firmware plays the first *.AVI it finds there, on loop.
#
# Usage:
#   tools/video_to_avi.sh input.mp4 [output.avi] [quality]
#
#   quality: ffmpeg -q:v for MJPEG, 2 (best) .. 31 (worst). Default 9.
#            The RP2040 decodes each frame in real time, and decode
#            cost scales with JPEG coefficient count -- lower values
#            (better quality) make frames bigger AND slower to decode,
#            causing dropped frames. Measured on hardware: q7 ~60 ms
#            per frame (~16 fps effective), q11 ~40 ms (~25 fps). The
#            16-colour dither masks most of the difference, so prefer
#            higher numbers. A mild denoise (hqdn3d) is always applied:
#            it cuts coefficient count sharply on grainy sources and is
#            visually free after quantization.

set -euo pipefail

if [ $# -lt 1 ]; then
  sed -n '2,28p' "$0" | sed 's/^#//; s/^ //'
  exit 1
fi

IN="$1"
OUT="${2:-${IN%.*}.avi}"
QUALITY="${3:-9}"
FPS=25

# Audio target. Default = 5600 Hz for the YM2149 path. STE=1 selects
# 25033 Hz for the STE DMA-sound path (the universal firmware also
# plays this rate on a plain ST by downsampling, so 25 kHz is the
# right choice if you have an STE-capable build).
if [ "${STE:-0}" = "1" ]; then
  AUDIO_RATE=25033
else
  AUDIO_RATE=5600
fi

if [ "$IN" = "$OUT" ]; then
  echo "error: output would overwrite input ($IN)" >&2
  exit 1
fi

# Notes on the flags that matter for smooth playback:
#  - `-max_interleave_delta 0`: force TIGHT audio/video interleaving.
#    Without it ffmpeg writes audio in ~0.5 s lumps; the firmware's
#    ~0.33 s audio ring can't hold one, so the demuxer stalls on each
#    lump and the video visibly stutters. Tight interleave -> ~100 ms
#    audio chunks that fit the ring.
#  - `loudnorm` (not dynaudnorm): dynaudnorm pumps the gain up in quiet
#    passages, amplifying the noise floor -- very audible on the 8/6-bit
#    output. loudnorm (EBU R128) normalises without that pumping.
ffmpeg -y -hide_banner -i "$IN" \
  -vf "fps=${FPS},crop='min(iw,ih*8/5)':'min(ih,iw*5/8)',scale=320:200:flags=lanczos,hqdn3d=3:2:6:4" \
  -c:v mjpeg -q:v "$QUALITY" -pix_fmt yuvj420p \
  -ac 1 -af "loudnorm=I=-16:TP=-1.5" -ar "$AUDIO_RATE" -c:a pcm_u8 \
  -max_interleave_delta 0 \
  "$OUT"

echo
echo "Wrote $OUT (${AUDIO_RATE} Hz audio) -- copy it to the SD card app folder (default /mjpeg)."
