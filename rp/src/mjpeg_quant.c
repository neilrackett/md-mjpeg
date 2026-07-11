/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_quant.c
 * Description: Per-frame 16-colour quantizer + 2x2 Bayer dither for
 *              the MJPEG player. See mjpeg_quant.h for the model.
 *
 * Pixel path (mq_draw, hot): RGB565 -> per-channel ordered-dither LUT
 * -> 9-bit RGB333 index -> histogram++ -> 512-entry map -> palette
 * index byte in fb_chunked_buffer. Two table lookups + one map lookup
 * per pixel; no multiplies.
 *
 * Palette path (mq_frame_done, ~1 ms): median cut over the 512-bin
 * RGB333 histogram (the ST can only display 512 colours, so the
 * histogram is exact, not sampled), weighted-centroid palette,
 * luminance sort (index 0 = darkest = ST border colour), nearest-
 * neighbour rebuild of the 512-entry map.
 */

#pragma GCC optimize("O3")

#include "mjpeg_quant.h"

#include <string.h>

#include "fb_chunked.h"
#include "mjpeg.h"
#include "palette.h"
#include "pico/platform.h"

/* The tables below live in .uninitialized_data -- the dead gap between
 * .data and the 32 KB-aligned commemul ring at the head of .bss --
 * keeping the .bss tail small and the heap (hard-capped at the cart
 * boundary) as large as possible. crt0 does NOT zero them; all are
 * fully initialized by mq_init() before first use. */

/* RGB333 histogram of the frame being decoded. 512 bins covers every
 * colour the ST can show, so no sampling error. uint16 is enough:
 * a bin can hold at most one full frame = 64 000 pixels. */
static uint16_t __uninitialized_ram(hist)[512];

/* RGB333 (9-bit) -> palette index, rebuilt per frame. */
static uint8_t __uninitialized_ram(map9)[512];

/* Ordered-dither LUTs, one per 2x2 Bayer position p = ((y&1)<<1)|(x&1).
 * Values are pre-shifted into their RGB333 bit positions so the inner
 * loop is just OR. */
static uint16_t __uninitialized_ram(lutR)[4][32]; /* 5-bit red -> 3, <<6 */
static uint16_t __uninitialized_ram(lutG)[4][64]; /* 6-bit grn -> 3, <<3 */
static uint8_t __uninitialized_ram(lutB)[4][32];  /* 5-bit blu -> 3     */

/* Current palette in 0..7 channel space (matches the published ST
 * palette words). */
static uint8_t pal_r[PALETTE_ENTRIES];
static uint8_t pal_g[PALETTE_ENTRIES];
static uint8_t pal_b[PALETTE_ENTRIES];

/* 2x2 Bayer matrix [[0,2],[3,1]] flattened by ((y&1)<<1)|(x&1). */
static const uint8_t bayer[4] = {0, 2, 3, 1};

/* Nearest-palette-entry map for all 512 RGB333 colours. Distance is
 * weighted (2,4,3) -- green dominates luminance but blue stays
 * relevant. 512 x 16 iterations, ~0.5 ms at O3. */
static void build_map(void) {
  for (unsigned c = 0; c < 512; c++) {
    int r = (int)((c >> 6) & 7u);
    int g = (int)((c >> 3) & 7u);
    int b = (int)(c & 7u);
    unsigned best = 0;
    unsigned best_d = ~0u;
    for (unsigned i = 0; i < PALETTE_ENTRIES; i++) {
      int dr = r - pal_r[i];
      int dg = g - pal_g[i];
      int db = b - pal_b[i];
      unsigned d = (unsigned)(dr * dr * 2 + dg * dg * 4 + db * db * 3);
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    map9[c] = (uint8_t)best;
  }
}

static void publish_palette(void) {
  uint16_t st[PALETTE_ENTRIES];
  for (unsigned i = 0; i < PALETTE_ENTRIES; i++) {
    st[i] = PALETTE_RGB(pal_r[i], pal_g[i], pal_b[i]);
  }
  palette_set(st);
}

void mq_init(void) {
  /* Ordered dither: out = floor(c/cmax * 7 + (2*bayer+1)/8), all
   * integer: (c*7*8 + (2b+1)*cmax) / (cmax*8). */
  for (unsigned p = 0; p < 4; p++) {
    unsigned t = 2u * bayer[p] + 1u;
    for (unsigned c = 0; c < 32; c++) {
      unsigned v = (c * 7u * 8u + t * 31u) / (31u * 8u);
      lutR[p][c] = (uint16_t)(v << 6);
      lutB[p][c] = (uint8_t)v;
    }
    for (unsigned c = 0; c < 64; c++) {
      unsigned v = (c * 7u * 8u + t * 63u) / (63u * 8u);
      lutG[p][c] = (uint16_t)(v << 3);
    }
  }

  /* Bootstrap greyscale ramp; the first decoded frame replaces it. */
  for (unsigned i = 0; i < PALETTE_ENTRIES; i++) {
    unsigned v = (i * 7u + 7u) / 15u;
    pal_r[i] = pal_g[i] = pal_b[i] = (uint8_t)v;
  }
  publish_palette();
  build_map();
  memset(hist, 0, sizeof(hist));
}

int __not_in_flash_func(mq_draw)(JPEGDRAW *pDraw) {
  int w = pDraw->iWidthUsed > 0 ? pDraw->iWidthUsed : pDraw->iWidth;
  int x0 = pDraw->x;
  if (x0 < FB_CHUNKED_W) {
    if (x0 + w > FB_CHUNKED_W) {
      w = FB_CHUNKED_W - x0;
    }
    for (int row = 0; row < pDraw->iHeight; row++) {
      int y = pDraw->y + row;
      if (y >= FB_CHUNKED_H) {
        break;
      }
      const uint16_t *s = pDraw->pPixels + (uint32_t)row * pDraw->iWidth;
      uint8_t *d = fb_chunked_buffer + (uint32_t)y * FB_CHUNKED_W + x0;
      /* Two interleaved Bayer columns for this row. */
      unsigned pa = (unsigned)(((y & 1) << 1) | (x0 & 1));
      unsigned pb = pa ^ 1u;
      const uint16_t *ra = lutR[pa], *rb = lutR[pb];
      const uint16_t *ga = lutG[pa], *gb = lutG[pb];
      const uint8_t *ba = lutB[pa], *bb = lutB[pb];
      int i = 0;
      for (; i + 1 < w; i += 2) {
        uint32_t p1 = s[i];
        uint32_t p2 = s[i + 1];
        unsigned c1 = ra[(p1 >> 11) & 31] | ga[(p1 >> 5) & 63] | ba[p1 & 31];
        unsigned c2 = rb[(p2 >> 11) & 31] | gb[(p2 >> 5) & 63] | bb[p2 & 31];
        hist[c1]++;
        hist[c2]++;
        d[i] = map9[c1];
        d[i + 1] = map9[c2];
      }
      if (i < w) {
        uint32_t p1 = s[i];
        unsigned c1 = ra[(p1 >> 11) & 31] | ga[(p1 >> 5) & 63] | ba[p1 & 31];
        hist[c1]++;
        d[i] = map9[c1];
      }
    }
  }
  mjpeg_service(); /* audio + IKBD stay alive during the decode */
  return 1;
}

/* --- median cut ------------------------------------------------- */

typedef struct {
  uint8_t r0, r1, g0, g1, b0, b1; /* inclusive bounds in 0..7 space */
  uint32_t cnt;
} box_t;

/* Tighten a box to its occupied bins and recount it. */
static void box_shrink(box_t *bx) {
  unsigned rmin = 7, rmax = 0, gmin = 7, gmax = 0, bmin = 7, bmax = 0;
  uint32_t cnt = 0;
  for (unsigned r = bx->r0; r <= bx->r1; r++) {
    for (unsigned g = bx->g0; g <= bx->g1; g++) {
      const uint16_t *row = &hist[(r << 6) | (g << 3)];
      for (unsigned b = bx->b0; b <= bx->b1; b++) {
        uint32_t n = row[b];
        if (n) {
          cnt += n;
          if (r < rmin) rmin = r;
          if (r > rmax) rmax = r;
          if (g < gmin) gmin = g;
          if (g > gmax) gmax = g;
          if (b < bmin) bmin = b;
          if (b > bmax) bmax = b;
        }
      }
    }
  }
  bx->cnt = cnt;
  if (cnt) {
    bx->r0 = (uint8_t)rmin;
    bx->r1 = (uint8_t)rmax;
    bx->g0 = (uint8_t)gmin;
    bx->g1 = (uint8_t)gmax;
    bx->b0 = (uint8_t)bmin;
    bx->b1 = (uint8_t)bmax;
  }
}

/* Marginal count of one slice of a box along the chosen axis
 * (0 = r, 1 = g, 2 = b). */
static uint32_t box_slice(const box_t *bx, int axis, unsigned v) {
  uint32_t n = 0;
  switch (axis) {
    case 0:
      for (unsigned g = bx->g0; g <= bx->g1; g++)
        for (unsigned b = bx->b0; b <= bx->b1; b++)
          n += hist[(v << 6) | (g << 3) | b];
      break;
    case 1:
      for (unsigned r = bx->r0; r <= bx->r1; r++)
        for (unsigned b = bx->b0; b <= bx->b1; b++)
          n += hist[(r << 6) | (v << 3) | b];
      break;
    default:
      for (unsigned r = bx->r0; r <= bx->r1; r++)
        for (unsigned g = bx->g0; g <= bx->g1; g++)
          n += hist[(r << 6) | (g << 3) | v];
      break;
  }
  return n;
}

void mq_frame_done(void) {
  box_t boxes[PALETTE_ENTRIES];
  boxes[0] = (box_t){0, 7, 0, 7, 0, 7, 0};
  box_shrink(&boxes[0]);
  if (boxes[0].cnt == 0) {
    return; /* nothing decoded; keep the current palette */
  }

  unsigned nb = 1;
  while (nb < PALETTE_ENTRIES) {
    /* Split the most-populated box that still has extent. */
    int bi = -1;
    uint32_t bc = 0;
    for (unsigned i = 0; i < nb; i++) {
      const box_t *bx = &boxes[i];
      if ((bx->r1 > bx->r0 || bx->g1 > bx->g0 || bx->b1 > bx->b0) &&
          bx->cnt > bc) {
        bc = bx->cnt;
        bi = (int)i;
      }
    }
    if (bi < 0) {
      break;
    }
    box_t *bx = &boxes[bi];
    unsigned lr = (unsigned)(bx->r1 - bx->r0);
    unsigned lg = (unsigned)(bx->g1 - bx->g0);
    unsigned lb = (unsigned)(bx->b1 - bx->b0);
    int axis;
    unsigned lo, hi;
    if (lg >= lr && lg >= lb) {
      axis = 1;
      lo = bx->g0;
      hi = bx->g1;
    } else if (lr >= lb) {
      axis = 0;
      lo = bx->r0;
      hi = bx->r1;
    } else {
      axis = 2;
      lo = bx->b0;
      hi = bx->b1;
    }
    /* Weighted median: cut after the slice that crosses half the
     * population, keeping both halves non-empty (cut < hi). */
    uint32_t half = bx->cnt / 2;
    uint32_t acc = 0;
    unsigned cut = lo;
    for (unsigned v = lo; v < hi; v++) {
      acc += box_slice(bx, axis, v);
      cut = v;
      if (acc >= half) {
        break;
      }
    }
    box_t hi_box = *bx;
    switch (axis) {
      case 0:
        bx->r1 = (uint8_t)cut;
        hi_box.r0 = (uint8_t)(cut + 1);
        break;
      case 1:
        bx->g1 = (uint8_t)cut;
        hi_box.g0 = (uint8_t)(cut + 1);
        break;
      default:
        bx->b1 = (uint8_t)cut;
        hi_box.b0 = (uint8_t)(cut + 1);
        break;
    }
    box_shrink(bx);
    box_shrink(&hi_box);
    boxes[nb++] = hi_box;
  }

  /* Weighted centroid of each box -> palette entry. */
  uint8_t tr[PALETTE_ENTRIES], tg[PALETTE_ENTRIES], tb[PALETTE_ENTRIES];
  for (unsigned i = 0; i < nb; i++) {
    const box_t *bx = &boxes[i];
    uint32_t sr = 0, sg = 0, sb = 0;
    for (unsigned r = bx->r0; r <= bx->r1; r++) {
      for (unsigned g = bx->g0; g <= bx->g1; g++) {
        for (unsigned b = bx->b0; b <= bx->b1; b++) {
          uint32_t n = hist[(r << 6) | (g << 3) | b];
          sr += n * r;
          sg += n * g;
          sb += n * b;
        }
      }
    }
    uint32_t c = bx->cnt ? bx->cnt : 1;
    tr[i] = (uint8_t)((sr + c / 2) / c);
    tg[i] = (uint8_t)((sg + c / 2) / c);
    tb[i] = (uint8_t)((sb + c / 2) / c);
  }
  for (unsigned i = nb; i < PALETTE_ENTRIES; i++) {
    tr[i] = tr[nb - 1];
    tg[i] = tg[nb - 1];
    tb[i] = tb[nb - 1];
  }

  /* Luminance sort, darkest first: palette index 0 doubles as the ST
   * border colour, so keep it as dark as the frame allows. */
  for (unsigned i = 1; i < PALETTE_ENTRIES; i++) {
    uint8_t r = tr[i], g = tg[i], b = tb[i];
    unsigned key = 3u * r + 6u * g + b;
    int j = (int)i - 1;
    while (j >= 0 && (3u * tr[j] + 6u * tg[j] + tb[j]) > key) {
      tr[j + 1] = tr[j];
      tg[j + 1] = tg[j];
      tb[j + 1] = tb[j];
      j--;
    }
    tr[j + 1] = r;
    tg[j + 1] = g;
    tb[j + 1] = b;
  }
  memcpy(pal_r, tr, sizeof(pal_r));
  memcpy(pal_g, tg, sizeof(pal_g));
  memcpy(pal_b, tb, sizeof(pal_b));

  publish_palette();
  build_map();
  memset(hist, 0, sizeof(hist));
}
