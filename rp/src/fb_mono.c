/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: fb_mono.c
 * Description: The chunked buffer as an ST high res picture: 640x400 at
 *              one bit a pixel, for a mono monitor. Each 320x200 pixel
 *              becomes a 2x2 block, its pen given a level 0..16 by the
 *              caller's table, dithered at the output resolution against
 *              the same 32x32 blue noise the colour quantizer uses (a
 *              32-pixel tile row is one output word pair). A set bit is
 *              black ink, the ST's white paper being colour 0 (userfw.s).
 *
 * Shared by the hardware build and EmuMD's: fb_chunked.c (or emu/'s)
 * owns the scratch and the cores, and calls fb_mono_rows for its rows.
 */

#pragma GCC optimize("O3")

#include "bluenoise.h"
#include "fb_chunked.h"

/* 640 output pixels a row, 16 to a word. */
#define MONO_WORDS (2u * FB_CHUNKED_W / 16u)

void fb_mono_rows(uint16_t *scratch, unsigned y0, unsigned y1,
                  const uint8_t levels[16]) {
  for (unsigned y = y0; y < y1; y++) {
    const uint8_t *src = fb_chunked_buffer + y * FB_CHUNKED_W;
    const uint8_t *t0 =
        &bluenoise[((2u * y) & (BLUENOISE_N - 1u)) * BLUENOISE_N];
    const uint8_t *t1 =
        &bluenoise[((2u * y + 1u) & (BLUENOISE_N - 1u)) * BLUENOISE_N];
    uint16_t *d0 = scratch + 2u * y * MONO_WORDS;
    uint16_t *d1 = d0 + MONO_WORDS;
    for (unsigned w = 0; w < MONO_WORDS; w++, src += 8) {
      /* Eight source pixels make one word of each output row; the word
       * covers tile columns 0..15 or 16..31. A pixel is ink where its
       * level does not clear the threshold (levels 0..16, thresholds
       * 0..15, so 0 is all ink and 16 all paper). */
      const uint8_t *a = t0 + (w & 1u) * 16u;
      const uint8_t *b = t1 + (w & 1u) * 16u;
      uint32_t top = 0, bot = 0;
      for (unsigned i = 0; i < 8u; i++) {
        const unsigned l = levels[src[i] & 15u];
        top = (top << 2) | ((uint32_t)(l <= a[2u * i]) << 1) |
              (uint32_t)(l <= a[2u * i + 1u]);
        bot = (bot << 2) | ((uint32_t)(l <= b[2u * i]) << 1) |
              (uint32_t)(l <= b[2u * i + 1u]);
      }
      d0[w] = (uint16_t)top;
      d1[w] = (uint16_t)bot;
    }
  }
}
