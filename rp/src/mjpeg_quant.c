/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_quant.c
 * Description: Per-frame 16-colour quantizer + ordered dither (4x4
 *              Bayer or 32x32 blue noise, build-selectable) for the
 *              MJPEG player. See mjpeg_quant.h for the model.
 *
 * Pixel path (mq_draw, hot): RGB565 -> per-channel ordered-dither LUT
 * -> 9-bit RGB333 index -> histogram++ -> 512-entry map -> palette
 * index byte in fb_chunked_buffer. Two table lookups + one map lookup
 * per pixel; no multiplies.
 *
 * Palette path (mq_frame_done, ~1 ms): median cut over the 512-bin
 * RGB333 histogram (the ST can only display 512 colours, so the
 * histogram is exact, not sampled), weighted-centroid palette,
 * luminance sort (index 0 = darkest = ST border colour), temporal
 * hysteresis (slew toward the new palette, snap on scene cuts -- see
 * MQ_HYSTERESIS below), nearest-neighbour rebuild of the 512-entry map.
 *
 * Palette entries are placed on the STE's finer RGB444 (0..15) grid
 * and published in STE encoding (PALETTE_STE_RGB). Because the STE
 * puts each channel's extra bit at the *top* of the nibble, a plain
 * ST ignores it and reads the top-3-bit RGB333 approximation of the
 * same word -- so one palette is correct on both machines, with no
 * runtime detection. The win on STE is finer *placement* of the 16
 * entries (1/15 vs 1/7 per channel), i.e. more accurate colours and
 * smoother grey ramps -- most visible in gradients. It is NOT more
 * simultaneous colours (still 16), and the number of distinct levels
 * for near-greyscale content is bounded by the RGB333 histogram that
 * feeds the cut, not by the palette depth; the per-pixel dither also
 * still runs in RGB333 (see mjpeg_quant.h). Lifting those needs an
 * RGB444 histogram/dither, a separate and RAM-heavier change.
 */

#pragma GCC optimize("O3")

#include "mjpeg_quant.h"

#include <string.h>

#include "fb_chunked.h"
#include "mjpeg.h"
#include "palette.h"
#include "pico/platform.h"

/* Ordered-dither pattern: 0 = 4x4 Bayer (default), 1 = 32x32 blue
 * noise (void-and-cluster). Both feed the SAME 16-level LUTs and
 * everything downstream, so this is a clean A/B of the threshold
 * PATTERN only -- regular crosshatch vs isotropic film grain. Select
 * at build time without editing source via the MQ_DITHER env var (see
 * rp/src/CMakeLists.txt): `MQ_DITHER=bluenoise ./build.sh ...`. */
#ifndef MQ_DITHER_BLUENOISE
#define MQ_DITHER_BLUENOISE 0
#endif

#if MQ_DITHER_BLUENOISE
#include "bluenoise.h" /* const 32x32 level matrix, lives in flash */
#endif

/* Palette temporal hysteresis. Each frame's median cut produces a
 * slightly different 16-colour palette even when the picture barely
 * moves; publishing it raw makes gradients shimmer/"pump" in motion.
 * With hysteresis on (default), the published palette slews toward the
 * new target by at most MQ_HYST_STEP per channel per frame, holds when
 * within MQ_HYST_DEADBAND (kills 1-step jitter), and snaps outright
 * when the whole-palette change exceeds MQ_HYST_SNAP (a scene cut,
 * where a snap is invisible because everything changed anyway).
 * Build MQ_HYSTERESIS=off to reproduce the raw per-frame behaviour. */
#ifndef MQ_HYSTERESIS
#define MQ_HYSTERESIS 1
#endif
#define MQ_HYST_STEP 1      /* max channel steps/frame (lower = smoother) */
#define MQ_HYST_DEADBAND 1  /* hold if target within this (0..15 space)  */
#define MQ_HYST_SNAP 160    /* sum |dchannel| over 16x3 (0..720) => snap  */

/* Build an ST/STE palette word from three 0..15 (RGB444) channels.
 * The STE stores each channel's low bit at the top of its nibble
 * (bits 11/7/3) and the high 3 bits in the positions the plain ST
 * reads (bits 10-8 / 6-4 / 2-0). So an STE reconstructs the full
 * 4-bit value while a plain ST -- which ignores bits 11/7/3 -- sees
 * the top-3-bit RGB333 truncation of the same word. One encoding,
 * both machines, no detection. (palette.h's PALETTE_RGB is the
 * ST-only 0..7 form; this is the app-side RGB444 sibling.) */
#define PALETTE_STE_RGB(r, g, b)                                              \
  ((uint16_t)(((((uint16_t)(r) >> 1) & 7u) << 8) |                            \
              (((uint16_t)(r) & 1u) << 11) |                                  \
              ((((uint16_t)(g) >> 1) & 7u) << 4) |                            \
              (((uint16_t)(g) & 1u) << 7) |                                   \
              ((((uint16_t)(b) >> 1) & 7u) << 0) |                            \
              (((uint16_t)(b) & 1u) << 3)))

/* The dither LUTs live in .uninitialized_data -- the dead gap between
 * .data and the 32 KB-aligned commemul ring at the head of .bss --
 * alongside the ~18 KB JPEGIMAGE (see mjpeg.c), so the .bss tail (and
 * thus the heap, hard-capped at the cart boundary) stays small. crt0
 * does NOT zero this section; mq_init() fills every entry before use.
 * The hole is nearly full: if a build overflows it, .bss jumps a full
 * 32 KB (ring alignment) and the LINKER fails with a RAM-overflow
 * error (not a silent bomb). Shrink the LUTs -- e.g. store 3-bit
 * values and shift at use -- rather than forcing that jump. */

/* Ordered-dither LUTs, one per 4x4 Bayer position p = ((y&3)<<2)|(x&3).
 * Values are pre-shifted into their RGB333 bit positions so the inner
 * loop is just OR. 16 positions (vs 2x2's 4) give 17 tonal steps
 * between adjacent palette levels instead of 5 -- smoother gradients,
 * far less crosshatch -- at identical per-pixel cost. */
static uint16_t __uninitialized_ram(lutR)[16][32]; /* 5-bit red -> 3, <<6 */
static uint16_t __uninitialized_ram(lutG)[16][64]; /* 6-bit grn -> 3, <<3 */
static uint8_t __uninitialized_ram(lutB)[16][32];  /* 5-bit blu -> 3     */

/* Frame histogram + palette map. Small enough for .bss (zeroed at
 * boot; mq_init / build_map fill them), kept OUT of the tight
 * .uninitialized_data hole so the larger 4x4 dither LUTs fit there. */

/* RGB333 histogram of the frame being decoded. 512 bins covers every
 * colour the ST can show, so no sampling error. uint16 is enough:
 * a bin can hold at most one full frame = 64 000 pixels. */
static uint16_t hist[512];

/* RGB333 (9-bit) -> palette index, rebuilt per frame. */
static uint8_t map9[512];

/* Current palette in 0..15 (RGB444) channel space. Published via
 * PALETTE_STE_RGB, so a plain ST sees the top-3-bit RGB333 form. With
 * hysteresis on, this persists across frames as the slew state. */
static uint8_t pal_r[PALETTE_ENTRIES];
static uint8_t pal_g[PALETTE_ENTRIES];
static uint8_t pal_b[PALETTE_ENTRIES];

#if MQ_HYSTERESIS
/* False until the first frame's palette is committed (which snaps
 * rather than slews from the greyscale bootstrap). */
static uint8_t s_have_palette;

/* Move one channel toward its target: hold within the deadband, else
 * step toward it by at most MQ_HYST_STEP (never overshoot). */
static inline uint8_t slew_ch(uint8_t cur, uint8_t tgt) {
  int d = (int)tgt - (int)cur;
  if (d > MQ_HYST_DEADBAND) {
    return (uint8_t)(cur + (d < MQ_HYST_STEP ? d : MQ_HYST_STEP));
  }
  if (d < -MQ_HYST_DEADBAND) {
    return (uint8_t)(cur + (d > -MQ_HYST_STEP ? d : -MQ_HYST_STEP));
  }
  return cur;
}
#endif

#if !MQ_DITHER_BLUENOISE
/* 4x4 Bayer matrix flattened by ((y&3)<<2)|(x&3). */
static const uint8_t bayer[16] = {
    0,  8,  2,  10,
    12, 4,  14, 6,
    3,  11, 1,  9,
    15, 7,  13, 5,
};
#endif

/* Nearest-palette-entry map for all 512 RGB333 colours. Distance is
 * weighted (2,4,3) -- green dominates luminance but blue stays
 * relevant. The map input is an RGB333 bin (0..7) but the palette is
 * RGB444 (0..15); both are scaled onto a common 0..105 grid (bin*15,
 * pal*7) so the comparison is exact and division-free. 512 x 16
 * iterations, ~0.5 ms at O3. */
static void build_map(void) {
  for (unsigned c = 0; c < 512; c++) {
    int r = (int)((c >> 6) & 7u) * 15;
    int g = (int)((c >> 3) & 7u) * 15;
    int b = (int)(c & 7u) * 15;
    unsigned best = 0;
    unsigned best_d = ~0u;
    for (unsigned i = 0; i < PALETTE_ENTRIES; i++) {
      int dr = r - pal_r[i] * 7;
      int dg = g - pal_g[i] * 7;
      int db = b - pal_b[i] * 7;
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
    st[i] = PALETTE_STE_RGB(pal_r[i], pal_g[i], pal_b[i]);
  }
  palette_set(st);
}

void mq_init(void) {
  /* Ordered dither: out = floor(c/cmax * 7 + (2*level+1)/32), i.e. the
   * 16 normalized thresholds are (level+0.5)/16. Integer form:
   * (c*7*32 + (2*level+1)*cmax) / (cmax*32). For Bayer, the LUT index
   * IS the tile position, so bake bayer[] in (index p -> level
   * bayer[p]); for blue noise the LUT is identity (index = level) and
   * the matrix supplies each cell's level. */
  for (unsigned p = 0; p < 16; p++) {
#if MQ_DITHER_BLUENOISE
    unsigned t = 2u * p + 1u;
#else
    unsigned t = 2u * bayer[p] + 1u;
#endif
    for (unsigned c = 0; c < 32; c++) {
      unsigned v = (c * 7u * 32u + t * 31u) / (31u * 32u);
      lutR[p][c] = (uint16_t)(v << 6);
      lutB[p][c] = (uint8_t)v;
    }
    for (unsigned c = 0; c < 64; c++) {
      unsigned v = (c * 7u * 32u + t * 63u) / (63u * 32u);
      lutG[p][c] = (uint16_t)(v << 3);
    }
  }

  /* Bootstrap greyscale ramp; the first decoded frame replaces it.
   * i in 0..15 is a full 16-step RGB444 ramp. */
  for (unsigned i = 0; i < PALETTE_ENTRIES; i++) {
    pal_r[i] = pal_g[i] = pal_b[i] = (uint8_t)i;
  }
#if MQ_HYSTERESIS
  s_have_palette = 0; /* first mq_frame_done snaps, not slews */
#endif
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
#if MQ_DITHER_BLUENOISE
      /* Blue noise: each cell's dither level comes from the 32x32
       * matrix (tiles every 32 px). lutR/G/B are identity-indexed by
       * level (see mq_init), so the pixel body is otherwise unchanged. */
      const uint8_t *nz =
          &bluenoise[(unsigned)(y & (BLUENOISE_N - 1)) * BLUENOISE_N];
      int i = 0;
      for (; i + 3 < w; i += 4) {
        unsigned l0 = nz[(unsigned)(x0 + i) & (BLUENOISE_N - 1)];
        unsigned l1 = nz[(unsigned)(x0 + i + 1) & (BLUENOISE_N - 1)];
        unsigned l2 = nz[(unsigned)(x0 + i + 2) & (BLUENOISE_N - 1)];
        unsigned l3 = nz[(unsigned)(x0 + i + 3) & (BLUENOISE_N - 1)];
        uint32_t q0 = s[i], q1 = s[i + 1], q2 = s[i + 2], q3 = s[i + 3];
        unsigned c0 = lutR[l0][(q0 >> 11) & 31] | lutG[l0][(q0 >> 5) & 63] | lutB[l0][q0 & 31];
        unsigned c1 = lutR[l1][(q1 >> 11) & 31] | lutG[l1][(q1 >> 5) & 63] | lutB[l1][q1 & 31];
        unsigned c2 = lutR[l2][(q2 >> 11) & 31] | lutG[l2][(q2 >> 5) & 63] | lutB[l2][q2 & 31];
        unsigned c3 = lutR[l3][(q3 >> 11) & 31] | lutG[l3][(q3 >> 5) & 63] | lutB[l3][q3 & 31];
        hist[c0]++;
        hist[c1]++;
        hist[c2]++;
        hist[c3]++;
        d[i] = map9[c0];
        d[i + 1] = map9[c1];
        d[i + 2] = map9[c2];
        d[i + 3] = map9[c3];
      }
      for (; i < w; i++) {
        unsigned l = nz[(unsigned)(x0 + i) & (BLUENOISE_N - 1)];
        uint32_t q = s[i];
        unsigned c = lutR[l][(q >> 11) & 31] | lutG[l][(q >> 5) & 63] | lutB[l][q & 31];
        hist[c]++;
        d[i] = map9[c];
      }
#else
      /* Four Bayer columns for this row (4x4 matrix). rL[k] is the LUT
       * for screen column x0+k; since the inner loop steps by 4 from a
       * multiple of 4, pixel i+k always has phase k. */
      unsigned base = (unsigned)((y & 3) << 2);
      const uint16_t *rL[4], *gL[4];
      const uint8_t *bL[4];
      for (unsigned k = 0; k < 4; k++) {
        unsigned p = base | ((unsigned)(x0 + (int)k) & 3u);
        rL[k] = lutR[p];
        gL[k] = lutG[p];
        bL[k] = lutB[p];
      }
      int i = 0;
      for (; i + 3 < w; i += 4) {
        uint32_t q0 = s[i], q1 = s[i + 1], q2 = s[i + 2], q3 = s[i + 3];
        unsigned c0 = rL[0][(q0 >> 11) & 31] | gL[0][(q0 >> 5) & 63] | bL[0][q0 & 31];
        unsigned c1 = rL[1][(q1 >> 11) & 31] | gL[1][(q1 >> 5) & 63] | bL[1][q1 & 31];
        unsigned c2 = rL[2][(q2 >> 11) & 31] | gL[2][(q2 >> 5) & 63] | bL[2][q2 & 31];
        unsigned c3 = rL[3][(q3 >> 11) & 31] | gL[3][(q3 >> 5) & 63] | bL[3][q3 & 31];
        hist[c0]++;
        hist[c1]++;
        hist[c2]++;
        hist[c3]++;
        d[i] = map9[c0];
        d[i + 1] = map9[c1];
        d[i + 2] = map9[c2];
        d[i + 3] = map9[c3];
      }
      for (unsigned j = 0; i < w; i++, j++) {
        uint32_t q = s[i];
        unsigned c = rL[j][(q >> 11) & 31] | gL[j][(q >> 5) & 63] | bL[j][q & 31];
        hist[c]++;
        d[i] = map9[c];
      }
#endif
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
    /* Weighted centroid is a fraction of the 0..7 bin range; scale it
     * onto the 0..15 RGB444 grid (x15/7) with round-to-nearest so the
     * palette entry uses the STE's full precision. */
    uint32_t c = bx->cnt ? bx->cnt : 1;
    uint32_t den = 7u * c;
    tr[i] = (uint8_t)((sr * 15u + den / 2u) / den);
    tg[i] = (uint8_t)((sg * 15u + den / 2u) / den);
    tb[i] = (uint8_t)((sb * 15u + den / 2u) / den);
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
#if MQ_HYSTERESIS
  /* Total change vs the on-screen palette decides snap (cut) vs slew.
   * Both current and target are luminance-sorted, so slot i tracks the
   * i-th-darkest colour and the comparison is like-for-like. */
  unsigned change = 0;
  for (unsigned i = 0; i < PALETTE_ENTRIES; i++) {
    int dr = (int)pal_r[i] - tr[i], dg = (int)pal_g[i] - tg[i],
        db = (int)pal_b[i] - tb[i];
    change += (unsigned)((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) +
                         (db < 0 ? -db : db));
  }
  if (!s_have_palette || change > MQ_HYST_SNAP) {
    memcpy(pal_r, tr, sizeof(pal_r));
    memcpy(pal_g, tg, sizeof(pal_g));
    memcpy(pal_b, tb, sizeof(pal_b));
    s_have_palette = 1;
  } else {
    for (unsigned i = 0; i < PALETTE_ENTRIES; i++) {
      pal_r[i] = slew_ch(pal_r[i], tr[i]);
      pal_g[i] = slew_ch(pal_g[i], tg[i]);
      pal_b[i] = slew_ch(pal_b[i], tb[i]);
    }
  }
#else
  memcpy(pal_r, tr, sizeof(pal_r));
  memcpy(pal_g, tg, sizeof(pal_g));
  memcpy(pal_b, tb, sizeof(pal_b));
#endif

  publish_palette();
  build_map();
  memset(hist, 0, sizeof(hist));
}
