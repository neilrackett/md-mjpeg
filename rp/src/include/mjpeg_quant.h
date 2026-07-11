/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_quant.h
 * Description: Per-frame 16-colour quantizer for the MJPEG player.
 *
 * mq_draw() is the JPEGDEC draw callback: it converts each RGB565
 * band to ST RGB333 with a 2x2 Bayer ordered dither, accumulates an
 * RGB333 histogram, and maps every pixel to a palette index straight
 * into fb_chunked_buffer.
 *
 * mq_frame_done() runs a median cut over the histogram to produce the
 * next frame's 16-colour palette (published via palette_set) and
 * rebuilds the 512-entry RGB333 -> palette-index map. The palette
 * therefore lags the content by one frame -- invisible at 25 fps
 * except for a single-frame glitch on hard scene cuts, and it avoids
 * decoding every JPEG twice.
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
