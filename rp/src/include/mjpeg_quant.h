/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_quant.h
 * Description: Per-frame 16-colour quantizer for the MJPEG player.
 *
 * mq_draw() is the JPEGDEC draw callback: it converts each RGB565
 * band to ST RGB333 with an ordered dither (4x4 Bayer by default, or
 * 32x32 blue noise -- build-selectable, see mjpeg_quant.c), accumulates
 * an RGB333 histogram, and maps every pixel to a palette index straight
 * into fb_chunked_buffer.
 *
 * mq_frame_done() runs a median cut over the histogram to produce the
 * next frame's 16-colour palette (published via palette_set) and
 * rebuilds the 512-entry RGB333 -> palette-index map. The palette
 * therefore lags the content by one frame -- invisible at 25 fps
 * except for a single-frame glitch on hard scene cuts, and it avoids
 * decoding every JPEG twice. Temporal hysteresis (on by default) then
 * slews the published palette toward that target so it doesn't shimmer
 * frame-to-frame, snapping on scene cuts -- see mjpeg_quant.c.
 *
 * The palette entries are RGB444 (0..15/channel), published in STE
 * encoding so a plain ST reads the RGB333 truncation of the same word
 * (one palette, both machines, no detection). This gives STE finer
 * colour placement, not more simultaneous colours -- the dither and
 * histogram remain RGB333. See mjpeg_quant.c.
 */

#ifndef MJPEG_QUANT_H_INCLUDED
#define MJPEG_QUANT_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "JPEGDEC.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the Bayer dither LUTs and install a bootstrap greyscale
 * palette (replaced by the real quantized palette after the first
 * decoded frame). Call once before the first decode. */
void mq_init(void);

/* JPEGDEC draw callback (JPEG_DRAW_CALLBACK). Dithers + maps the band
 * into fb_chunked_buffer and calls mjpeg_service(). */
int mq_draw(JPEGDRAW *pDraw);

/* ST high res: the JPEGDEC draw callback for a luma-only decode
 * (EIGHT_BIT_GRAYSCALE). Writes each pixel as one of 16 grey pens, 0
 * black to 15 white in linear light -- the grey ramp fb_publish() draws
 * in mono -- and calls mjpeg_service(). No histogram or palette:
 * nothing reads the palette in mono. */
int mq_draw_mono(JPEGDRAW *pDraw);

/* 256 colours (a TT or a Falcon, fb_st_eight): true when mq_init chose
 * them, and then mq_draw8 is the draw callback. It dithers to RGB444,
 * counts a 4096-bin histogram and maps each pixel to one of the frame's
 * 240 colours (palette indices 16..255; 0..15 are a grey ramp, black to
 * white, for the overlay and the menu). mq_frame_done median-cuts the
 * histogram into the next frame's 240, and mq_republish does nothing:
 * pens 0 and 15 are black and white already. See mjpeg_quant.c. */
bool mq_eight(void);
int mq_draw8(JPEGDRAW *pDraw);

/* 256 colours: the part of fb_eight_scratch() the tables leave free, 4-byte
 * aligned, its size in `bytes` (about 11 KB). NULL and 0 unless
 * mq_eight(). Valid until the next mq_init. */
uint8_t *mq_eight_spare(uint32_t *bytes);

/* Publish the palette and rebuild the map again, now: for when the menu
 * opens or closes over the video (while it is shown, entry 15 is held at
 * white -- entry 0 is always black -- so its box and ink stay readable
 * whatever the frame), and a paused frame won't be decoded to do it. */
void mq_republish(void);

/* Median-cut the frame histogram into the next frame's palette: fifteen
 * colours plus black, sorted darkest-first so palette index 0 -- the ST
 * border colour -- is always black. Publishes it, rebuilds the pixel map,
 * clears the histogram. Call once after each successful decode. */
void mq_frame_done(void);

#ifdef __cplusplus
}
#endif

#endif /* MJPEG_QUANT_H_INCLUDED */
