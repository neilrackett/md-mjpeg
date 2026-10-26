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

/* One 16-pixel run of a tile row as ink masks, one per level 0..16:
 * masks[l] has bit 15 - x set where the threshold t[x] >= l, i.e. where
 * a pixel at level l is ink (0 all ink, 16 all paper). */
static void ink_masks(const uint8_t *t, uint16_t masks[17]) {
  uint16_t at[16] = {0};
  for (unsigned x = 0; x < 16u; x++) {
    at[t[x]] |= (uint16_t)(0x8000u >> x);
  }
  masks[16] = 0;
  for (int l = 15; l >= 0; l--) {
    masks[l] = masks[l + 1] | at[l];
  }
}

/* For each half of the 32-pixel tile, each pen as ink bits across the
 * half's 16 output pixels of source row y: the top output row in the
 * high half of the word, the bottom row in the low. Once a row, so it
 * stays in flash; only the per-pixel loop below is RAM-resident. */
static void __attribute__((noinline))
row_pen_bits(unsigned y, const uint8_t levels[16], uint32_t pen_bits[2][16]) {
  for (unsigned h = 0; h < 2u; h++) {
    uint16_t top[17], bot[17];
    ink_masks(&bluenoise[((2u * y) & (BLUENOISE_N - 1u)) * BLUENOISE_N +
                         16u * h],
              top);
    ink_masks(&bluenoise[((2u * y + 1u) & (BLUENOISE_N - 1u)) * BLUENOISE_N +
                         16u * h],
              bot);
    for (unsigned p = 0; p < 16u; p++) {
      pen_bits[h][p] = ((uint32_t)top[levels[p]] << 16) | bot[levels[p]];
    }
  }
}

void __not_in_flash_func(fb_mono_rows)(uint16_t *scratch, unsigned y0,
                                       unsigned y1, const uint8_t levels[16]) {
  for (unsigned y = y0; y < y1; y++) {
    /* Eight source pixels make one word of each output row, each taking
     * its two columns of its pen's bits. */
    uint32_t pen_bits[2][16];
    row_pen_bits(y, levels, pen_bits);
    const uint8_t *src = fb_chunked_buffer + y * FB_CHUNKED_W;
    uint16_t *d0 = scratch + 2u * y * MONO_WORDS;
    uint16_t *d1 = d0 + MONO_WORDS;
    for (unsigned w = 0; w < MONO_WORDS; w++, src += 8) {
      const uint32_t *pb = pen_bits[w & 1u];
      uint32_t acc = 0, m = 0xC000C000u;
      for (unsigned i = 0; i < 8u; i++, m >>= 2) {
        acc |= pb[src[i] & 15u] & m;
      }
      d0[w] = (uint16_t)(acc >> 16);
      d1[w] = (uint16_t)acc;
    }
  }
}
