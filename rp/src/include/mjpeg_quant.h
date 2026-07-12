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

/* Median-cut the frame histogram into the next frame's palette:
 * publishes it (sorted darkest-first, so palette index 0 -- the ST
 * border colour -- stays dark), rebuilds the pixel map, clears the
 * histogram. Call once after each successful decode. */
void mq_frame_done(void);

#ifdef __cplusplus
}
#endif

#endif /* MJPEG_QUANT_H_INCLUDED */
