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
AUDIO_RATE=5600   # must match the m68k Timer-B consumption rate

if [ "$IN" = "$OUT" ]; then
  echo "error: output would overwrite input ($IN)" >&2
  exit 1
fi

# Centre crop-fill to 16:10, then scale to 320x200. The crop keeps
# whichever dimension already fits and trims the other, so any input
# aspect ratio fills the frame with no letterboxing.
ffmpeg -y -hide_banner -i "$IN" \
  -vf "fps=${FPS},crop='min(iw,ih*8/5)':'min(ih,iw*5/8)',scale=320:200:flags=lanczos,hqdn3d=3:2:6:4" \
  -c:v mjpeg -q:v "$QUALITY" -pix_fmt yuvj420p \
  -ac 1 -af dynaudnorm -ar "$AUDIO_RATE" -c:a pcm_u8 \
  "$OUT"

echo
echo "Wrote $OUT -- copy it to the SD card app folder (default /mjpeg)."
