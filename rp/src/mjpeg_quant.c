/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_quant.c
 * Description: Per-frame 16-colour quantizer + ordered dither (32x32
 *              blue noise or 2x2 Bayer, build-selectable) for the
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
 * plus black, luminance sort (index 0 = that black = the ST border, so
 * the border stays black through every scene), temporal
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

/* Ordered-dither pattern: 1 = 32x32 blue noise (void-and-cluster,
 * default), 0 = 2x2 Bayer. Blue noise is isotropic film grain with
 * MQ_DITHER_N distinct levels -- marginally the best-looking of the
 * three tried on hardware; the 2x2 tile is a tighter 4-level pattern.
 * Both feed the same LUTs and everything downstream, so this is a clean
 * A/B of the threshold PATTERN only. Select at build time without
 * editing source via the MQ_DITHER env var (see rp/src/CMakeLists.txt):
 * `MQ_DITHER=bayer ./build.sh ...`. (An earlier 4x4 Bayer -- 16 levels,
 * dottier in motion on real hardware -- is in git history if wanted.) */
#ifndef MQ_DITHER_BLUENOISE
#define MQ_DITHER_BLUENOISE 1
#endif

#if MQ_DITHER_BLUENOISE
#include "bluenoise.h" /* const 32x32 level matrix, lives in flash */
#define MQ_DITHER_N 16 /* blue noise quantizes to 16 threshold levels */
#else
#define MQ_DITHER_N 4  /* 2x2 Bayer: 4 tile positions / threshold levels */
#endif
#define MQ_DITHER_2N (2u * MQ_DITHER_N) /* threshold denominator scale */

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

/* Palette saturation + contrast boost. Median-cut palette entries are
 * box centroids (population means), which pull toward grey and leave the
 * palette looking washed out. With this on (default), each entry is
 * pushed away from its own grey level (saturation) and away from
 * mid-grey (contrast) before the luminance sort. Both gains are integer
 * num/den (no float); tune here. MQ_SATURATE=off publishes raw centroids
 * for an A/B. Applied to the median-cut TARGET, so hysteresis then slews
 * toward the boosted palette exactly as before. */
#ifndef MQ_SATURATE
#define MQ_SATURATE 1
#endif
#define MQ_SAT_NUM 5  /* saturation gain 5/4 = 1.25x                      */
#define MQ_SAT_DEN 4
#define MQ_CON_NUM 9  /* contrast gain 9/8 = 1.125x (around mid-grey 7.5) */
#define MQ_CON_DEN 8

/* Histogram / dither channel depth. Default RGB333 = every channel 8
 * levels, 512 bins (exactly the ST's colour count -- no sampling error).
 * MQ_HIST=g4 (RGB343) gives GREEN a 4th bit (16 levels) -> 1024 bins,
 * for finer luminance/greyscale palette selection where it matters most
 * (green is ~59% of luminance); red and blue stay 3-bit. Costs +1.5 KB
 * .bss (hist 2 KB + map 1 KB). A full RGB444 histogram (4096 bins + a
 * 4 KB map, ~10.5 KB) does NOT fit -- it drops heap headroom below the
 * settings library's ~14 KB boot allocation and OOM-panics; green-4 is
 * the affordable win (see the RGB444 note in AGENTS.md). Parameterised
 * so RGB333 compiles byte-identical to before. */
#ifndef MQ_HIST_G4
#define MQ_HIST_G4 0
#endif
#if MQ_HIST_G4
#define MQ_GBITS 4u
#else
#define MQ_GBITS 3u
#endif
#define MQ_GMAX ((1u << MQ_GBITS) - 1u)  /* green max level: 7 or 15      */
#define MQ_GSH 3u                          /* green bit shift (B is 0..2)  */
#define MQ_RSH (3u + MQ_GBITS)             /* red bit shift: 6 or 7        */
#define MQ_BINS (8u * (MQ_GMAX + 1u) * 8u) /* 512 or 1024                  */
#define MQ_GSCALE (105u / MQ_GMAX)         /* green bin -> 0..105: 15 or 7 */

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

/* Ordered-dither LUTs, one per dither level: the 2x2 Bayer tile's 4
 * positions p = ((y&1)<<1)|(x&1), or blue noise's 16 quantized levels.
 * Values are pre-shifted into their RGB333 bit positions so the inner
 * loop is just OR.
 *
 * These sit in ordinary .bss (zeroed at boot; mq_init fills them). The
 * .uninitialized_data hole between .data and the 32 KB-aligned commemul
 * ring is only just big enough for the ~18 KB JPEGIMAGE alone (see
 * mjpeg.c) -- putting the LUTs there too left almost no margin, and any
 * later .data growth pushed the hole past the ring boundary, jumping
 * .bss a full 32 KB and overflowing RAM. Keeping only s_jpg in the hole
 * restores comfortable margin. */
static uint16_t lutR[MQ_DITHER_N][32]; /* 5-bit red -> 3, << MQ_RSH */
static uint16_t lutG[MQ_DITHER_N][64]; /* 6-bit grn -> MQ_GBITS, << 3 */
static uint8_t lutB[MQ_DITHER_N][32];  /* 5-bit blu -> 3            */

/* Frame histogram + palette map (also plain .bss). */

/* Histogram of the frame being decoded, one bin per representable
 * dithered colour (MQ_BINS: 512 for RGB333, 1024 for RGB343). uint16 is
 * enough: a bin can hold at most one full frame = 64 000 pixels. */
static uint16_t hist[MQ_BINS];

/* Dithered-colour index -> palette index, rebuilt per frame. */
static uint8_t map9[MQ_BINS];

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
/* 2x2 Bayer matrix flattened by ((y&1)<<1)|(x&1). Four levels 0..3 ->
 * thresholds (level+0.5)/4 spread across the tight 2x2 tile. */
static const uint8_t bayer[MQ_DITHER_N] = {
    0, 2,
    3, 1,
};
#endif

/* Nearest-palette-entry map for all 512 RGB333 colours. Distance is
 * weighted (2,4,3) -- green dominates luminance but blue stays
 * relevant. The map input is an RGB333 bin (0..7) but the palette is
 * RGB444 (0..15); both are scaled onto a common 0..105 grid (bin*15,
 * pal*7) so the comparison is exact and division-free. 512 x 16
 * iterations, ~0.5 ms at O3. */
static void build_map(void) {
  for (unsigned c = 0; c < MQ_BINS; c++) {
    int r = (int)((c >> MQ_RSH) & 7u) * 15;
    int g = (int)((c >> MQ_GSH) & MQ_GMAX) * (int)MQ_GSCALE;
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

/* The menu drawn over a playing video (mjpeg_menu_shown) needs two pens
 * it can rely on. Entry 0 is always black (mq_frame_done); the lightest,
 * which every frame re-sorts, is held at white for as long as it is up --
 * the pixels then map around it like any other entry -- but only in what
 * is published and mapped: pal_r/g/b, and so the hysteresis, keep the
 * real colour. */
static void publish_and_map(void) {
  const unsigned last = PALETTE_ENTRIES - 1u;
  const uint8_t keep[3] = {pal_r[last], pal_g[last], pal_b[last]};
  if (mjpeg_menu_shown()) {
    pal_r[last] = pal_g[last] = pal_b[last] = 15;
  }
  publish_palette();
  build_map();
  pal_r[last] = keep[0];
  pal_g[last] = keep[1];
  pal_b[last] = keep[2];
}

void mq_republish(void) { publish_and_map(); }

void mq_init(void) {
  /* Ordered dither: out = floor(c/cmax * 7 + (2*level+1)/2N), i.e. the
   * N = MQ_DITHER_N normalized thresholds are (level+0.5)/N. Integer
   * form: (c*7*2N + (2*level+1)*cmax) / (cmax*2N). For Bayer, the LUT
   * index IS the tile position, so bake bayer[] in (index p -> level
   * bayer[p]); for blue noise the LUT is identity (index = level) and
   * the matrix supplies each cell's level. */
  for (unsigned p = 0; p < MQ_DITHER_N; p++) {
#if MQ_DITHER_BLUENOISE
    unsigned t = 2u * p + 1u;
#else
    unsigned t = 2u * bayer[p] + 1u;
#endif
    for (unsigned c = 0; c < 32; c++) {
      unsigned v = (c * 7u * MQ_DITHER_2N + t * 31u) / (31u * MQ_DITHER_2N);
      lutR[p][c] = (uint16_t)(v << MQ_RSH);
      lutB[p][c] = (uint8_t)v;
    }
    for (unsigned c = 0; c < 64; c++) {
      unsigned v = (c * MQ_GMAX * MQ_DITHER_2N + t * 63u) / (63u * MQ_DITHER_2N);
      lutG[p][c] = (uint16_t)(v << MQ_GSH);
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
      /* Bayer LUT per screen column for this row (2x2 tile: row phase
       * (y&1), column phase (x&1)). We precompute 4 columns and the
       * inner loop steps by 4, so with the 2-periodic tile rL[0]==rL[2]
       * and rL[1]==rL[3]; pixel i+k (i a multiple of 4) has phase k. */
      unsigned base = (unsigned)((y & 1) << 1);
      const uint16_t *rL[4], *gL[4];
      const uint8_t *bL[4];
      for (unsigned k = 0; k < 4; k++) {
        unsigned p = base | ((unsigned)(x0 + (int)k) & 1u);
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

/* --- ST high res ------------------------------------------------ */

/* One grey byte a pixel in, one of 16 pens out. A mono monitor mixes
 * light, so the pen is linear: Y^2 stands in for the encoded-to-linear
 * curve (gamma 2), otherwise every mid grey would come out too light.
 * (v * v * 15 + 32768) >> 16 is round(15 * (v / 255)^2) to within the
 * 255/256 the shift gives away; 255 maps to 15. */
int __not_in_flash_func(mq_draw_mono)(JPEGDRAW *pDraw) {
  int w = pDraw->iWidthUsed > 0 ? pDraw->iWidthUsed : pDraw->iWidth;
  int x0 = pDraw->x;
  if (x0 < FB_CHUNKED_W) {
    if (x0 + w > FB_CHUNKED_W) {
      w = FB_CHUNKED_W - x0;
    }
    const uint8_t *band = (const uint8_t *)pDraw->pPixels;
    for (int row = 0; row < pDraw->iHeight; row++) {
      int y = pDraw->y + row;
      if (y >= FB_CHUNKED_H) {
        break;
      }
      const uint8_t *s = band + (uint32_t)row * pDraw->iWidth;
      uint8_t *d = fb_chunked_buffer + (uint32_t)y * FB_CHUNKED_W + x0;
      for (int i = 0; i < w; i++) {
        uint32_t v = s[i];
        d[i] = (uint8_t)((v * v * 15u + 32768u) >> 16);
      }
    }
  }
  mjpeg_service(); /* audio + IKBD stay alive during the decode */
  return 1;
}

/* --- median cut ------------------------------------------------- */

typedef struct {
  uint8_t r0, r1, g0, g1, b0, b1; /* inclusive bounds; r,b 0..7, g 0..MQ_GMAX */
  uint32_t cnt;
} box_t;

/* Tighten a box to its occupied bins and recount it. */
static void box_shrink(box_t *bx) {
  unsigned rmin = 7, rmax = 0, gmin = MQ_GMAX, gmax = 0, bmin = 7, bmax = 0;
  uint32_t cnt = 0;
  for (unsigned r = bx->r0; r <= bx->r1; r++) {
    for (unsigned g = bx->g0; g <= bx->g1; g++) {
      const uint16_t *row = &hist[(r << MQ_RSH) | (g << MQ_GSH)];
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
          n += hist[(v << MQ_RSH) | (g << MQ_GSH) | b];
      break;
    case 1:
      for (unsigned r = bx->r0; r <= bx->r1; r++)
        for (unsigned b = bx->b0; b <= bx->b1; b++)
          n += hist[(r << MQ_RSH) | (v << MQ_GSH) | b];
      break;
    default:
      for (unsigned r = bx->r0; r <= bx->r1; r++)
        for (unsigned g = bx->g0; g <= bx->g1; g++)
          n += hist[(r << MQ_RSH) | (g << MQ_GSH) | v];
      break;
  }
  return n;
}

#if MQ_SATURATE
static inline uint8_t clamp15(int v) {
  return (uint8_t)(v < 0 ? 0 : (v > 15 ? 15 : v));
}

/* Push one 0..15 RGB444 palette entry away from grey (saturation) and
 * away from mid-grey (contrast), countering the centroid wash-out. Luma
 * uses the same 3:6:1 weighting as the luminance sort. Worked doubled
 * internally so the 7.5 mid-grey and half-step gains stay exact. */
static void boost_entry(uint8_t *pr, uint8_t *pg, uint8_t *pb) {
  int r = *pr, g = *pg, b = *pb;
  int luma = (3 * r + 6 * g + b + 5) / 10;
  int lc2 = 15 + (2 * luma - 15) * MQ_CON_NUM / MQ_CON_DEN; /* 2x contrast(luma) */
  int r2 = lc2 + 2 * (r - luma) * MQ_SAT_NUM / MQ_SAT_DEN;
  int g2 = lc2 + 2 * (g - luma) * MQ_SAT_NUM / MQ_SAT_DEN;
  int b2 = lc2 + 2 * (b - luma) * MQ_SAT_NUM / MQ_SAT_DEN;
  *pr = clamp15((r2 + 1) / 2);
  *pg = clamp15((g2 + 1) / 2);
  *pb = clamp15((b2 + 1) / 2);
}
#endif

void mq_frame_done(void) {
  box_t boxes[PALETTE_ENTRIES];
  boxes[0] = (box_t){0, 7, 0, (uint8_t)MQ_GMAX, 0, 7, 0};
  box_shrink(&boxes[0]);
  if (boxes[0].cnt == 0) {
    return; /* nothing decoded; keep the current palette */
  }

  /* One entry fewer than the palette: the last is black (below). */
  unsigned nb = 1;
  while (nb < PALETTE_ENTRIES - 1u) {
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
          uint32_t n = hist[(r << MQ_RSH) | (g << MQ_GSH) | b];
          sr += n * r;
          sg += n * g;
          sb += n * b;
        }
      }
    }
    /* Weighted centroid is a fraction of each channel's bin range; scale
     * onto the 0..15 RGB444 grid (r,b x15/7; g x15/MQ_GMAX -- x1 when
     * green is already 4-bit) with round-to-nearest so the palette entry
     * uses the STE's full precision. */
    uint32_t c = bx->cnt ? bx->cnt : 1;
    uint32_t den = 7u * c;      /* red / blue: 0..7 -> 0..15 */
    uint32_t deng = MQ_GMAX * c; /* green: 0..MQ_GMAX -> 0..15 */
    tr[i] = (uint8_t)((sr * 15u + den / 2u) / den);
    tg[i] = (uint8_t)((sg * 15u + deng / 2u) / deng);
    tb[i] = (uint8_t)((sb * 15u + den / 2u) / den);
  }
#if MQ_SATURATE
  for (unsigned i = 0; i < nb; i++) {
    boost_entry(&tr[i], &tg[i], &tb[i]);
  }
#endif
  for (unsigned i = nb; i < PALETTE_ENTRIES - 1u; i++) {
    tr[i] = tr[nb - 1];
    tg[i] = tg[nb - 1];
    tb[i] = tb[nb - 1];
  }
  /* Black, always. Palette index 0 doubles as the ST border colour, and
   * the darkest of each frame's own colours made the border change with
   * every scene; black sorts to index 0 and keeps it black. It is added
   * rather than put in place of the darkest colour, so the frame keeps
   * all fifteen of its own: a bright frame loses one tone it would have
   * had, a dark one usually wanted black anyway. */
  tr[PALETTE_ENTRIES - 1u] = tg[PALETTE_ENTRIES - 1u] =
      tb[PALETTE_ENTRIES - 1u] = 0;

  /* Luminance sort, darkest first, so index 0 is the black. */
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

  publish_and_map();
  memset(hist, 0, sizeof(hist));
}
