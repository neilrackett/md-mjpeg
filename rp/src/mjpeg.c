/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg.c
 * Description: AVI (MJPEG + u8 PCM) streaming player. See mjpeg.h for
 *              the model; mjpeg_quant.c for the colour pipeline.
 *
 * The AVI is played straight off the SD card with no whole-frame RAM
 * buffer: JPEGDEC pulls the '00dc' chunk bytes through a FatFs read
 * callback (a virtual file windowed onto the chunk), and its draw
 * callback dithers each decoded band directly into fb_chunked_buffer.
 * '01wb' chunks feed an 8 KB PCM ring; the audio fill callback pops
 * 112 samples per VBL, maps each through the Ghostbusters (vA, vB)
 * LUT and advances the master sample clock that paces video.
 *
 * ffmpeg interleaves audio in ~0.5 s lumps and typically muxes the
 * first lump *after* the first dozen video frames, so mjpeg_open()
 * pre-scans the movi list to prime the ring (~1 s of audio), then
 * rewinds and skips those bytes when it meets them again in-line.
 */

#include "mjpeg.h"

#include <stdio.h>
#include <string.h>

#include "JPEGDEC.h"
#include "audio.h"
#include "debug.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "mjpeg_menu.h"
#include "mjpeg_quant.h"
#include "pico/time.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

/* jpeg.inl's parser entry, exported by jpegdec_core.c; the rest of
 * the C API is declared by JPEGDEC.h when compiled as C. */
int mjpeg_jpeg_init(JPEGIMAGE *pJPEG);

#define FCC(a, b, c, d)                                                       \
  ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) |                   \
   ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

#define FCC_RIFF FCC('R', 'I', 'F', 'F')
#define FCC_AVI FCC('A', 'V', 'I', ' ')
#define FCC_LIST FCC('L', 'I', 'S', 'T')
#define FCC_HDRL FCC('h', 'd', 'r', 'l')
#define FCC_STRL FCC('s', 't', 'r', 'l')
#define FCC_STRH FCC('s', 't', 'r', 'h')
#define FCC_MOVI FCC('m', 'o', 'v', 'i')
#define FCC_VIDS FCC('v', 'i', 'd', 's')
#define FCC_AUDS FCC('a', 'u', 'd', 's')
#define FCC_00DC FCC('0', '0', 'd', 'c')
#define FCC_00DB FCC('0', '0', 'd', 'b')
#define FCC_01WB FCC('0', '1', 'w', 'b')

/* The m68k Timer-B handler consumes 2 bytes (vA, vB) per sample; the
 * fill callback is invoked with 224 bytes = 112 samples per VBL. */
#define MJPEG_SAMPLES_PER_VBL 112u

/* PCM ring. Must be a power of two. 4 KB = ~1.6 VBL at the 25 kHz DMA
 * rate. That's ample now that tools/video_to_avi.sh interleaves audio
 * tightly (`-max_interleave_delta 0` -> ~100 ms / 2.5 KB chunks): the
 * demuxer buffers one small chunk at a time instead of a 0.5 s lump,
 * so an 8 KB ring is no longer needed -- and the 4 KB saved keeps the
 * heap clear of the settings-library boot allocation (RAM is tight;
 * see the .uninitialized_data note in mjpeg_quant.c). */
#define ARING_SIZE 4096u
/* Prime ~0.5 s (YM) / ~0.12 s (DMA) before starting so the ring never
 * runs dry between interleave chunks. Must stay below ARING_SIZE. */
#define ARING_PREFILL 3072u
/* In 256 colours a frame also waits for the m68k to copy the first half
 * of the picture before the second can go (fb_publish, up to ~38 ms on
 * a real TT, by MD/DOOM's figures), so the decode can hold off the
 * demuxer for ~120 ms -- all of the prefill's lead. And a ring that runs
 * dry is never made up: the clock counts the silence as time, so the
 * audio falls behind the video for good, and ~250 ms of it leaves every
 * frame late and the picture frozen. So there the ring is twice the
 * size, in the scratch the 256-colour tables leave (mq_eight_spare). */
#define ARING_SIZE_EIGHT 8192u
#define ARING_PREFILL_EIGHT 7168u

/* Skip (rather than decode) a due video frame once we are this many
 * frames behind the audio clock. */
#define MJPEG_SKIP_THRESHOLD 2u

/* Ring of recent video-frame file offsets, for backward seeking (MJPEG
 * is all-intra so any offset is a valid resume point). Power of two.
 * 512 frames = ~20 s at 25 fps, comfortably covering a -10 s seek. */
#define SEEK_RING 512u

/* How long the time overlay stays up after a seek / pause key (us). */
#define OVERLAY_US 2500000u

/* 64-entry (vA, vB) YM volume-pair LUT from the 1988 Ghostbusters
 * demo (SAMPLE1 in GHOST.S), indexed by the top 6 bits of an unsigned
 * 8-bit PCM sample. Same table as tools/wav_to_ym4.py --mode
 * dual-ghost; the m68k Timer-B handler plays exactly this layout.
 * Used by the YM (downsampled) fill path. */
static const uint8_t ghost_lut[64][2] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 2},  {2, 3},  {1, 4},  {2, 4},  {2, 5},
    {0, 6},  {2, 6},  {3, 6},  {4, 6},  {2, 7},  {4, 7},  {5, 7},  {2, 8},
    {3, 8},  {4, 8},  {5, 8},  {2, 9},  {3, 9},  {4, 9},  {5, 9},  {6, 9},
    {7, 9},  {3, 10}, {4, 10}, {5, 10}, {6, 10}, {7, 10}, {0, 11}, {1, 11},
    {2, 11}, {4, 11}, {5, 11}, {6, 11}, {7, 11}, {8, 11}, {8, 11}, {9, 11},
    {9, 11}, {0, 12}, {1, 12}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12},
    {8, 12}, {8, 12}, {9, 12}, {9, 12}, {9, 12}, {10, 12}, {0, 13}, {2, 13},
    {3, 13}, {4, 13}, {5, 13}, {6, 13}, {7, 13}, {8, 13}, {8, 13}, {9, 13},
};

/* --- state ------------------------------------------------------- */

static FIL s_fil;
static bool s_open;

static uint32_t s_movi_start; /* offset of the first chunk in movi */
static uint32_t s_movi_end;   /* one past the last movi byte */
static uint32_t s_samples_per_frame; /* audio samples per video frame */
static uint32_t s_total_frames;      /* video frame count (0 = unknown) */
static uint32_t s_vscale, s_vrate;   /* video fps = vrate / vscale */

static uint32_t s_pos;        /* offset of the next chunk header */
static uint32_t s_audio_pos;  /* file offset of pending audio bytes */
static uint32_t s_audio_left; /* pending bytes of the current '01wb' */
static uint32_t s_audio_skip; /* prefilled bytes to skip after rewind */

static uint32_t s_frame_index; /* next frame to show, since rewind */
static volatile uint32_t s_sample_clock; /* advanced by the fill cb */

static bool s_paused;
static bool s_menu; /* the menu is drawn over the video */
static uint32_t s_overlay_until;         /* show overlay until this us */
static uint32_t s_frame_offsets[SEEK_RING]; /* frame index -> file offset */

static uint32_t s_frames_shown, s_frames_skipped;
static uint32_t s_decode_us; /* last decode cost, for the stats line */
static uint32_t s_quant_us;  /* and its palette cut (mq_frame_done) */

/* PCM ring: Core 0 (the demuxer) pushes, the fill callback pops on Core
 * 1 (audio.c's refill interrupt). Single producer, single consumer;
 * resetting it takes audio_lock(). aring is aring_static, or in 256
 * colours ARING_SIZE_EIGHT bytes of the scratch; aring_size is a power
 * of two. Both are chosen by mjpeg_open before the fill can see them. */
static uint8_t aring_static[ARING_SIZE];
static uint8_t *aring = aring_static;
static uint32_t aring_size = ARING_SIZE, aring_prefill = ARING_PREFILL;
static volatile uint32_t aring_head, aring_tail; /* free-running; head - tail = fill */

/* JPEGDEC state is ~18 KB. Placed in .uninitialized_data, which lands
 * in the otherwise-dead gap between .data and the 32 KB-aligned
 * commemul ring at the head of .bss -- keeping the .bss tail (and so
 * the heap) ~18 KB larger. The settings library alone mallocs ~13 KB
 * at boot, and the heap is hard-capped at the cart-region boundary
 * (see __StackLimit in memmap_rp.ld), so this headroom is load-bearing.
 * Not zeroed at boot; decode_frame() memsets it before every use. */
static JPEGIMAGE __uninitialized_ram(s_jpg);
static uint32_t s_jpg_base; /* file offset of the JPEG being decoded */

/* --- audio ------------------------------------------------------- */

static inline uint32_t aring_level(void) { return aring_head - aring_tail; }

/* Source PCM samples one VBL of YM output takes = the 25 kHz source rate
 * / 50 Hz VBL; in ST high res a VBL is 71.47 Hz, so 350. The DMA path
 * takes what the m68k measures the chip eating instead (~501.5, or ~350
 * in mono), so the video clock follows the sound actually played. */
#define MJPEG_SRC_PER_VBL 500u
#define MJPEG_SRC_PER_VBL_MONO 350u

/* Pop one unsigned source PCM sample; 0x80 (silence) on underrun. */
static inline uint8_t aring_pop(void) {
  uint8_t s = 0x80;
  if (aring_head != aring_tail) {
    s = aring[aring_tail & (aring_size - 1u)];
    aring_tail++;
  }
  return s;
}

/* Per-VBL fill callback (audio.c's refill interrupt, on Core 1 under
 * audio_lock()). Produces the format matching the runtime audio mode,
 * then advances the sample clock by one VBL of source samples (silence
 * counts as time, so video keeps pacing before the first audio and
 * across underruns):
 *  - DMA: signed-PCM passthrough, `bytes` samples (the chip's measured
 *    VBL). AVI stores unsigned (silence 0x80), the DMA wants signed
 *    (silence 0x00) -> flip the top bit.
 *  - YM: box-average one VBL of source samples down to bytes/2
 *    outputs (~4.46:1 for a 25 kHz source on a plain ST), each mapped
 *    through the Ghostbusters (vA,vB) LUT.
 *  - SILENT (pre-report): zeros. */
static void mjpeg_audio_fill(uint8_t *buf, uint32_t bytes) {
  if (!s_open || s_paused) {
    /* Nothing playing (or still priming), or paused: silence, and DON'T
     * advance the clock or drain the ring, so the picture and audio both
     * resume exactly where they froze. Zero is silence for both
     * back-ends (signed 0x00 for DMA, volume 0 YM). */
    memset(buf, 0, bytes);
    return;
  }
  uint32_t src = fb_st_mono() ? MJPEG_SRC_PER_VBL_MONO : MJPEG_SRC_PER_VBL;
  switch (audio_get_mode()) {
    case AUDIO_MODE_DMA:
      for (uint32_t i = 0; i < bytes; i++) {
        *buf++ = (uint8_t)(aring_pop() ^ 0x80u);
      }
      src = bytes;
      break;
    case AUDIO_MODE_YM: {
      uint32_t outs = bytes / 2u;
      uint32_t done = 0;
      for (uint32_t i = 0; i < outs; i++) {
        uint32_t next = ((i + 1u) * src) / outs;
        uint32_t cnt = next - done;
        done = next;
        uint32_t sum = 0;
        for (uint32_t j = 0; j < cnt; j++) {
          sum += aring_pop();
        }
        uint8_t avg = cnt ? (uint8_t)(sum / cnt) : 0x80u;
        const uint8_t *pair = ghost_lut[avg >> 2];
        *buf++ = pair[0];
        *buf++ = pair[1];
      }
      break;
    }
    case AUDIO_MODE_SILENT:
    default:
      memset(buf, 0, bytes);
      break;
  }
  s_sample_clock += src;
}

/* Read up to `bytes` from the current file position into the ring.
 * Returns the number of bytes actually pushed. */
static uint32_t aring_push_file(uint32_t bytes) {
  uint32_t pushed = 0;
  while (bytes) {
    uint32_t head = aring_head & (aring_size - 1u);
    uint32_t lin = aring_size - head;
    uint32_t space = aring_size - aring_level();
    if (lin > space) lin = space;
    if (lin > bytes) lin = bytes;
    if (!lin) break;
    UINT n = 0;
    if (f_read(&s_fil, &aring[head], lin, &n) != FR_OK || n == 0) break;
    __sync_synchronize(); /* the bytes land before Core 1 can see them */
    aring_head += n;
    pushed += n;
    bytes -= n;
    if (n < lin) break;
  }
  return pushed;
}

/* --- RIFF plumbing ------------------------------------------------ */

static int rd_u32(uint32_t off, uint32_t *v) {
  UINT n = 0;
  if (f_lseek(&s_fil, off) != FR_OK) return -1;
  if (f_read(&s_fil, v, 4, &n) != FR_OK || n != 4) return -1;
  return 0;
}

static int rd_chunk_hdr(uint32_t off, uint32_t *fcc, uint32_t *size) {
  uint32_t hdr[2];
  UINT n = 0;
  if (f_lseek(&s_fil, off) != FR_OK) return -1;
  if (f_read(&s_fil, hdr, 8, &n) != FR_OK || n != 8) return -1;
  *fcc = hdr[0];
  *size = hdr[1];
  return 0;
}

/* Walk a header LIST range looking for 'strh' chunks (any nesting). */
static void hdrl_walk(uint32_t pos, uint32_t end, int depth, uint32_t *vscale,
                      uint32_t *vrate, uint32_t *arate, uint32_t *vlength) {
  while (pos + 8 <= end) {
    uint32_t fcc, size;
    if (rd_chunk_hdr(pos, &fcc, &size)) return;
    uint32_t data = pos + 8;
    if (fcc == FCC_LIST && depth < 4) {
      hdrl_walk(data + 4, data + size, depth + 1, vscale, vrate, arate, vlength);
    } else if (fcc == FCC_STRH && size >= 28) {
      /* AVISTREAMHEADER: fccType @0, dwScale @20, dwRate @24,
       * dwLength @32 (frame count for the video stream). */
      uint32_t type, scale, rate, length = 0;
      if (!rd_u32(data, &type) && !rd_u32(data + 20, &scale) &&
          !rd_u32(data + 24, &rate)) {
        if (size >= 36) rd_u32(data + 32, &length);
        if (type == FCC_VIDS && rate) {
          *vscale = scale ? scale : 1;
          *vrate = rate;
          *vlength = length;
        } else if (type == FCC_AUDS && rate) {
          *arate = rate / (scale ? scale : 1);
        }
      }
    }
    pos = data + size + (size & 1u);
  }
}

static int avi_parse(void) {
  uint32_t fcc, size, ltype;
  if (rd_chunk_hdr(0, &fcc, &size) || fcc != FCC_RIFF) return -1;
  if (rd_u32(8, &ltype) || ltype != FCC_AVI) return -1;
  uint32_t riff_end = 8 + size;

  uint32_t vscale = 1, vrate = 25, arate = 5600, vlength = 0;
  s_movi_start = s_movi_end = 0;

  uint32_t pos = 12;
  while (pos + 8 <= riff_end) {
    if (rd_chunk_hdr(pos, &fcc, &size)) break;
    uint32_t data = pos + 8;
    if (fcc == FCC_LIST) {
      if (rd_u32(data, &ltype)) break;
      if (ltype == FCC_HDRL) {
        hdrl_walk(data + 4, data + size, 0, &vscale, &vrate, &arate, &vlength);
      } else if (ltype == FCC_MOVI) {
        s_movi_start = data + 4;
        s_movi_end = data + size;
        break; /* hdrl always precedes movi */
      }
    }
    pos = data + size + (size & 1u);
  }
  if (!s_movi_start) return -1;

  /* Audio samples per video frame; 5600 Hz at 25 fps -> 224. */
  s_samples_per_frame =
      (uint32_t)(((uint64_t)arate * vscale + vrate / 2) / vrate);
  if (!s_samples_per_frame) s_samples_per_frame = 224;
  s_vscale = vscale;
  s_vrate = vrate;
  s_total_frames = vlength; /* for the "cur / total" overlay */
  DPRINTF("AVI: movi %lu..%lu, %lu samples/frame (a %lu Hz, v %lu/%lu)\n",
          (unsigned long)s_movi_start, (unsigned long)s_movi_end,
          (unsigned long)s_samples_per_frame, (unsigned long)arate,
          (unsigned long)vscale, (unsigned long)vrate);
  return 0;
}

/* Prime the PCM ring from the head of movi (video chunks skipped),
 * then note how many audio-stream bytes to skip when the in-line
 * demux meets them again. */
static void prefill_audio(void) {
  uint32_t pos = s_movi_start;
  uint32_t got = 0;
  while (got < aring_prefill && pos + 8 <= s_movi_end) {
    uint32_t fcc, size;
    if (rd_chunk_hdr(pos, &fcc, &size)) break;
    uint32_t data = pos + 8;
    if (fcc == FCC_LIST) {
      pos = data + 4; /* descend into 'rec ' style lists */
      continue;
    }
    if (fcc == FCC_01WB && size) {
      uint32_t want = aring_prefill - got;
      if (want > size) want = size;
      got += aring_push_file(want);
    }
    pos = data + size + (size & 1u);
  }
  s_audio_skip = got;
}

/* --- video ------------------------------------------------------- */

static int32_t jpg_read(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t remain = pFile->iSize - pFile->iPos;
  if (iLen > remain) iLen = remain;
  if (iLen <= 0) return 0;
  FSIZE_t want = (FSIZE_t)s_jpg_base + (FSIZE_t)pFile->iPos;
  if (f_tell(&s_fil) != want && f_lseek(&s_fil, want) != FR_OK) return 0;
  UINT n = 0;
  if (f_read(&s_fil, pBuf, (UINT)iLen, &n) != FR_OK) return 0;
  pFile->iPos += (int32_t)n;
  return (int32_t)n;
}

static int32_t jpg_seek(JPEGFILE *pFile, int32_t iPosition) {
  if (iPosition < 0) iPosition = 0;
  if (iPosition > pFile->iSize) iPosition = pFile->iSize;
  pFile->iPos = iPosition;
  f_lseek(&s_fil, (FSIZE_t)s_jpg_base + (FSIZE_t)iPosition);
  return iPosition;
}

/* --- time overlay ------------------------------------------------ */

static inline bool overlay_active(void) {
  /* Signed diff so it survives the time_us_32() wraparound. */
  return s_paused || (int32_t)(s_overlay_until - time_us_32()) > 0;
}

/* Format a frame index as "M:SS" into out (>= 12 bytes). */
static void fmt_time(uint32_t frame, char *out) {
  uint32_t sec = s_vrate ? (uint32_t)(((uint64_t)frame * s_vscale) / s_vrate) : 0;
  snprintf(out, 12, "%lu:%02lu", (unsigned long)(sec / 60u),
           (unsigned long)(sec % 60u));
}

/* Draw "cur / total" bottom-left over the current frame. Uses the
 * quantized palette's luminance extremes -- index 0 (always black) for
 * the box, 15 (lightest) for the text -- so it reads on any frame. */
static void draw_overlay(void) {
  char cur[12], line[28];
  fmt_time(s_frame_index, cur);
  if (s_total_frames) {
    char tot[12];
    fmt_time(s_total_frames, tot);
    snprintf(line, sizeof(line), "%s / %s", cur, tot);
  } else {
    snprintf(line, sizeof(line), "%s", cur);
  }
  int w = (int)strlen(line) * 8 + 8;
  fb_fill_rect(4, 183, w, 13, 0);
  font_set_font(&font8x8);
  font_align(FONT_ALIGN_LEFT);
  font_set_color(15);
  font_move(8, 185);
  font_print(line);
}

static void decode_frame(uint32_t data, uint32_t size) {
  uint32_t t0 = time_us_32();
  memset(&s_jpg, 0, sizeof(s_jpg));
  /* ST high res: luma only, which skips the chroma IDCT and colour
   * conversion, into grey pens instead of the colour quantizer. */
  const bool mono = fb_st_mono();
  /* The quantizer took its colours from the display when the video
   * opened. A video opened before the m68k's first report (a key it
   * passed on in its first VBL, ahead of the report) gets 256 colours
   * from the first frame after. */
  if (!mono && fb_st_eight() != mq_eight()) mq_init();
  s_jpg.pfnRead = jpg_read;
  s_jpg.pfnSeek = jpg_seek;
  s_jpg.pfnDraw = mono ? mq_draw_mono : mq_eight() ? mq_draw8 : mq_draw;
  s_jpg.iMaxMCUs = 1000;
  s_jpg.JPEGFile.fHandle = &s_fil;
  s_jpg.JPEGFile.iSize = (int32_t)size;
  s_jpg_base = data;
  if (f_lseek(&s_fil, data) != FR_OK) return;
  if (!mjpeg_jpeg_init(&s_jpg)) {
    DPRINTF("JPEGInit failed (err %d) frame %lu\n", s_jpg.iError,
            (unsigned long)s_frame_index);
    return;
  }
  /* ucPixelType is already RGB565_LITTLE_ENDIAN (0) from the memset. */
  if (mono) s_jpg.ucPixelType = EIGHT_BIT_GRAYSCALE;
  if (!JPEG_decode(&s_jpg, 0, 0, 0)) {
    DPRINTF("JPEG_decode failed (err %d) frame %lu\n", s_jpg.iError,
            (unsigned long)s_frame_index);
    /* Publish whatever bands landed; better than a stall. */
  }
  s_decode_us = time_us_32() - t0;
  /* 256 colours: the last frame's bottom half has gone (the decode sent
   * it, unless it stopped short of row 100) before its palette is
   * replaced or anything else drawn. */
  fb_publish_wait_rows(FB_CHUNKED_H);
  const uint32_t tq = time_us_32();
  if (!mono) mq_frame_done(); /* palette for the NEXT frame, from this frame */
  s_quant_us = time_us_32() - tq;
  if (overlay_active()) draw_overlay();
  if (s_menu) mjpeg_menu_draw_over();
  fb_publish();    /* VBL-synced hand-off to the ST */
}

/* Decode the first video chunk at or after `p` (a chunk header offset)
 * without moving the play position. */
static void decode_at(uint32_t p) {
  while (p + 8 <= s_movi_end) {
    uint32_t fcc, size;
    if (rd_chunk_hdr(p, &fcc, &size)) break;
    uint32_t data = p + 8;
    if (fcc == FCC_LIST) {
      p = data + 4;
      continue;
    }
    if ((fcc == FCC_00DC || fcc == FCC_00DB) && size) {
      decode_frame(data, size);
      break;
    }
    p = data + size + (size & 1u);
  }
}

/* --- player ------------------------------------------------------ */

void mjpeg_service(void) {
  fb_pump_rom3();
  ikbd_pump();
}

uint32_t mjpeg_frames_shown(void) { return s_frames_shown; }

void mjpeg_debug_status(char *buf, int len) {
  /* A: audio refills since the last line (~150 in 3 s, ~214 in mono);
   * L: their size, the DMA length the m68k has settled on; Q: the last
   * frame's palette cut, after its decode (D). In 256 colours, B: the
   * rows of a part the m68k copies a VBL (100 = a part a VBL, so a frame
   * two; a real TT is nearer 75). */
  int n = snprintf(buf, (size_t)len, "F%lu S%lu I%lu E%d R%lu D%luus Q%luus A%lu L%lu",
                   (unsigned long)s_frames_shown, (unsigned long)s_frames_skipped,
                   (unsigned long)s_frame_index, s_jpg.iError,
                   (unsigned long)aring_level(), (unsigned long)s_decode_us,
                   (unsigned long)s_quant_us, (unsigned long)audio_take_fills(),
                   (unsigned long)audio_get_fill_bytes());
  if (fb_st_eight() && n > 0 && n < len) {
    snprintf(buf + n, (size_t)(len - n), " B%u", fb_eight_rows_per_vbl());
  }
}

int mjpeg_open(const char *path) {
  if (s_open) return -1;
  DPRINTF("MJPEG: opening %s\n", path);
  if (f_open(&s_fil, path, FA_READ) != FR_OK) {
    DPRINTF("MJPEG: cannot open %s\n", path);
    return -1;
  }
  DPRINTF("MJPEG: parsing (size %lu)\n", (unsigned long)f_size(&s_fil));
  if (avi_parse() < 0) {
    DPRINTF("MJPEG: %s is not a usable AVI\n", path);
    f_close(&s_fil);
    return -2;
  }
  mq_init();
  /* 256 colours: the deeper ring (see ARING_SIZE_EIGHT). Nothing reads
   * the ring while closed, so it can change here. */
  uint32_t spare = 0;
  uint8_t *deep = mq_eight_spare(&spare);
  if (deep && spare >= ARING_SIZE_EIGHT) {
    aring = deep;
    aring_size = ARING_SIZE_EIGHT;
    aring_prefill = ARING_PREFILL_EIGHT;
  } else {
    aring = aring_static;
    aring_size = ARING_SIZE;
    aring_prefill = ARING_PREFILL;
  }
  s_pos = s_movi_start;
  s_audio_left = 0;
  s_audio_skip = 0;
  s_frame_index = 0;
  s_sample_clock = 0;
  s_frames_shown = s_frames_skipped = 0;
  s_paused = false;
  s_overlay_until = 0;
  aring_head = aring_tail = 0;
  prefill_audio();
  DPRINTF("MJPEG: primed %lu B of audio\n", (unsigned long)aring_level());
  audio_set_fill_callback(mjpeg_audio_fill);
  const uint32_t saved = audio_lock();
  s_open = true; /* the fill starts taking from the primed ring */
  audio_unlock(saved);
  DPRINTF("MJPEG: playing %s\n", path);
  return 0;
}

void mjpeg_close(void) {
  /* Stop the fill (silence from its next refill) and drain the ring. */
  const uint32_t saved = audio_lock();
  const bool was_open = s_open;
  s_open = false;
  aring_head = aring_tail = 0;
  audio_unlock(saved);
  s_menu = false;
  if (was_open) {
    f_close(&s_fil);
    DPRINTF("MJPEG: closed\n");
  }
}

void mjpeg_finish(void) {
  mjpeg_close();
  /* The last frame stays in the chunked buffer and its palette in the
   * quantizer, so the menu can go straight over it, as over a playing
   * video (pen 15 held white). Nothing shown, nothing to keep. */
  if (s_frames_shown) {
    s_menu = true;
    mq_republish();
  }
}

bool mjpeg_tick(void) {
  if (!s_open) return false;
  if (s_paused) return true; /* frozen: no decode, no audio, no clock */
  for (;;) {
    /* Finish draining a partially-consumed audio chunk first. */
    if (s_audio_left) {
      if (aring_level() >= aring_size) return true; /* ring full; later */
      if (f_lseek(&s_fil, s_audio_pos) != FR_OK) {
        return false; /* read error -> treat as end */
      }
      uint32_t n = aring_push_file(s_audio_left);
      s_audio_pos += n;
      s_audio_left -= n;
      if (s_audio_left) return true; /* ring filled up */
      continue;
    }

    if (s_pos + 8 > s_movi_end) {
      DPRINTF("MJPEG: end (shown %lu skipped %lu)\n",
              (unsigned long)s_frames_shown, (unsigned long)s_frames_skipped);
      return false; /* end of movie */
    }
    uint32_t fcc, size;
    if (rd_chunk_hdr(s_pos, &fcc, &size)) {
      return false; /* read error -> treat as end */
    }
    uint32_t data = s_pos + 8;
    uint32_t next = data + size + (size & 1u);

    if (fcc == FCC_LIST) {
      s_pos = data + 4;
      continue;
    }
    if (fcc == FCC_01WB) {
      if (s_audio_skip >= size) {
        s_audio_skip -= size; /* already primed during prefill */
      } else {
        s_audio_pos = data + s_audio_skip;
        s_audio_left = size - s_audio_skip;
        s_audio_skip = 0;
      }
      s_pos = next;
      continue;
    }
    if (fcc == FCC_00DC || fcc == FCC_00DB) {
      s_frame_offsets[s_frame_index & (SEEK_RING - 1u)] = s_pos; /* for seek */
      uint32_t due = s_sample_clock / s_samples_per_frame;
      if (due < s_frame_index) {
        return true; /* not due yet; cursor stays on this chunk */
      }
      if (due >= s_frame_index + MJPEG_SKIP_THRESHOLD) {
        s_frames_skipped++; /* behind: drop this frame, no decode */
      } else if (size) {
        decode_frame(data, size);
        s_frames_shown++;
        if ((s_frames_shown % 50u) == 0) {
          DPRINTF("MJPEG: frame %lu decode %lu us ring %lu skipped %lu\n",
                  (unsigned long)s_frame_index, (unsigned long)s_decode_us,
                  (unsigned long)aring_level(),
                  (unsigned long)s_frames_skipped);
        }
      }
      s_frame_index++;
      s_pos = next;
      return true; /* at most one video frame per tick */
    }
    s_pos = next; /* unknown chunk (index etc.) -- skip */
  }
}

/* --- playback controls ------------------------------------------- */

/* Re-sync the audio ring + master clock to the current s_frame_index /
 * s_pos after a seek. */
static void seek_resync_audio(void) {
  s_audio_left = 0;
  s_audio_skip = 0;
  const uint32_t saved = audio_lock();
  aring_head = aring_tail = 0;
  s_sample_clock = (uint32_t)((uint64_t)s_frame_index * s_samples_per_frame);
  audio_unlock(saved);
}

void mjpeg_seek(int seconds) {
  if (!s_open) return;
  int32_t fps = (s_vscale && s_vrate) ? (int32_t)(s_vrate / s_vscale) : 25;
  if (fps < 1) fps = 25;

  int64_t tgt = (int64_t)s_frame_index + (int64_t)seconds * fps;
  if (tgt < 0) tgt = 0;
  if (s_total_frames && tgt > (int64_t)s_total_frames - 1) {
    tgt = (int64_t)s_total_frames - 1;
  }
  uint32_t target = (uint32_t)tgt;

  if (target < s_frame_index) {
    /* Backward: jump to the stored offset. Clamp to what the ring still
     * holds (a -10 s seek is well within its ~20 s window). */
    uint32_t oldest =
        (s_frame_index > SEEK_RING - 1u) ? (s_frame_index - (SEEK_RING - 1u)) : 0u;
    if (target < oldest) target = oldest;
    s_pos = s_frame_offsets[target & (SEEK_RING - 1u)];
    s_frame_index = target;
  } else if (target > s_frame_index) {
    /* Forward: walk the chunk stream (headers only), recording frame
     * offsets, until we reach the target frame or hit the end. */
    while (s_frame_index < target && s_pos + 8 <= s_movi_end) {
      uint32_t fcc, size;
      if (rd_chunk_hdr(s_pos, &fcc, &size)) break;
      uint32_t data = s_pos + 8;
      uint32_t next = data + size + (size & 1u);
      if (fcc == FCC_LIST) {
        s_pos = data + 4;
        continue;
      }
      if (fcc == FCC_00DC || fcc == FCC_00DB) {
        s_frame_offsets[s_frame_index & (SEEK_RING - 1u)] = s_pos;
        s_frame_index++;
      }
      s_pos = next;
    }
  }

  seek_resync_audio();
  s_overlay_until = time_us_32() + OVERLAY_US;
  DPRINTF("MJPEG: seek %+ds -> frame %lu\n", seconds,
          (unsigned long)s_frame_index);

  /* While paused, redraw immediately -- the tick that would otherwise
   * show the new frame doesn't run. (Playing: the next tick handles it.)
   * Walk to the next video chunk without disturbing s_pos, so unpausing
   * still resumes cleanly from the sought position. */
  if (s_paused) decode_at(s_pos);
}

void mjpeg_toggle_pause(void) {
  if (!s_open) return;
  s_paused = !s_paused;
  s_overlay_until = time_us_32() + OVERLAY_US;
  DPRINTF("MJPEG: %s\n", s_paused ? "paused" : "playing");
}

bool mjpeg_is_paused(void) { return s_paused; }

void mjpeg_render_paused(void) {
  /* The chunked buffer still holds the last decoded frame (nothing
   * overwrites it while paused); draw the overlay over it and republish
   * at VBL rate. Idempotent -- the boxes redraw over the same pixels. */
  fb_publish_wait_rows(FB_CHUNKED_H);
  draw_overlay();
  if (s_menu) mjpeg_menu_draw_over();
  fb_publish();
}

void mjpeg_show_menu(bool on) {
  if (!s_open || on == s_menu) return;
  s_menu = on;
  mq_republish(); /* entry 15 held at white while it is up */
  /* Playing, the next frame comes without it. Paused, the menu is drawn
   * into the frozen frame itself, so decode that frame again (the last
   * one shown, from the seek ring). */
  if (!on && s_paused && s_frame_index > 0) {
    decode_at(s_frame_offsets[(s_frame_index - 1u) & (SEEK_RING - 1u)]);
  }
}

bool mjpeg_menu_shown(void) { return s_menu; }
