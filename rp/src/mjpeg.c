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
#include "ff.h"
#include "ikbd.h"
#include "mjpeg_quant.h"
#include "pico/time.h"

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

/* PCM ring: 4 KB = ~0.7 s at 5600 Hz. Must be a power of two. Sized
 * to hold one full ~0.5 s ffmpeg interleave lump (2800 B) ON TOP of
 * the drain that accumulates across worst-case (~76 ms) decodes --
 * a 4 KB ring measurably underran every few lumps on hardware. */
#define ARING_SIZE 8192u
/* Prime ~1 s of audio before starting so the ring never runs dry
 * between ffmpeg's ~0.5 s interleave lumps. */
#define ARING_PREFILL 5600u

/* Skip (rather than decode) a due video frame once we are this many
 * frames behind the audio clock. */
#define MJPEG_SKIP_THRESHOLD 2u

/* 64-entry (vA, vB) YM volume-pair LUT from the 1988 Ghostbusters
 * demo (SAMPLE1 in GHOST.S), indexed by the top 6 bits of an unsigned
 * 8-bit PCM sample. Same table as tools/wav_to_ym4.py --mode
 * dual-ghost; the m68k Timer-B handler plays exactly this layout. */
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

static uint32_t s_pos;        /* offset of the next chunk header */
static uint32_t s_audio_pos;  /* file offset of pending audio bytes */
static uint32_t s_audio_left; /* pending bytes of the current '01wb' */
static uint32_t s_audio_skip; /* prefilled bytes to skip after rewind */

static uint32_t s_frame_index; /* next frame to show, since rewind */
static volatile uint32_t s_sample_clock; /* advanced by the fill cb */

static uint32_t s_frames_shown, s_frames_skipped, s_loops;
static uint32_t s_decode_us; /* last decode cost, for the stats line */

static uint8_t aring[ARING_SIZE];
static uint32_t aring_head, aring_tail; /* free-running; head - tail = fill */

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

/* Per-VBL fill callback (audio.c): pop one PCM byte per sample, map
 * through the Ghostbusters LUT, always advance the sample clock --
 * silence counts as time, which is what lets video start before the
 * first audio chunk and keep pacing across underruns. */
static void mjpeg_audio_fill(uint8_t *buf, uint32_t bytes) {
  uint32_t samples = bytes / 2u;
  for (uint32_t i = 0; i < samples; i++) {
    uint8_t s = 0x80; /* PCM midpoint = silence */
    if (aring_head != aring_tail) {
      s = aring[aring_tail & (ARING_SIZE - 1u)];
      aring_tail++;
    }
    const uint8_t *pair = ghost_lut[s >> 2];
    *buf++ = pair[0];
    *buf++ = pair[1];
  }
  s_sample_clock += samples;
}

/* Read up to `bytes` from the current file position into the ring.
 * Returns the number of bytes actually pushed. */
static uint32_t aring_push_file(uint32_t bytes) {
  uint32_t pushed = 0;
  while (bytes) {
    uint32_t head = aring_head & (ARING_SIZE - 1u);
    uint32_t lin = ARING_SIZE - head;
    uint32_t space = ARING_SIZE - aring_level();
    if (lin > space) lin = space;
    if (lin > bytes) lin = bytes;
    if (!lin) break;
    UINT n = 0;
    if (f_read(&s_fil, &aring[head], lin, &n) != FR_OK || n == 0) break;
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
                      uint32_t *vrate, uint32_t *arate) {
  while (pos + 8 <= end) {
    uint32_t fcc, size;
    if (rd_chunk_hdr(pos, &fcc, &size)) return;
    uint32_t data = pos + 8;
    if (fcc == FCC_LIST && depth < 4) {
      hdrl_walk(data + 4, data + size, depth + 1, vscale, vrate, arate);
    } else if (fcc == FCC_STRH && size >= 28) {
      /* AVISTREAMHEADER: fccType @0, dwScale @20, dwRate @24. */
      uint32_t type, scale, rate;
      if (!rd_u32(data, &type) && !rd_u32(data + 20, &scale) &&
          !rd_u32(data + 24, &rate)) {
        if (type == FCC_VIDS && rate) {
          *vscale = scale ? scale : 1;
          *vrate = rate;
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

  uint32_t vscale = 1, vrate = 25, arate = 5600;
  s_movi_start = s_movi_end = 0;

  uint32_t pos = 12;
  while (pos + 8 <= riff_end) {
    if (rd_chunk_hdr(pos, &fcc, &size)) break;
    uint32_t data = pos + 8;
    if (fcc == FCC_LIST) {
      if (rd_u32(data, &ltype)) break;
      if (ltype == FCC_HDRL) {
        hdrl_walk(data + 4, data + size, 0, &vscale, &vrate, &arate);
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
  while (got < ARING_PREFILL && pos + 8 <= s_movi_end) {
    uint32_t fcc, size;
    if (rd_chunk_hdr(pos, &fcc, &size)) break;
    uint32_t data = pos + 8;
    if (fcc == FCC_LIST) {
      pos = data + 4; /* descend into 'rec ' style lists */
      continue;
    }
    if (fcc == FCC_01WB && size) {
      uint32_t want = ARING_PREFILL - got;
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

static void decode_frame(uint32_t data, uint32_t size) {
  uint32_t t0 = time_us_32();
  memset(&s_jpg, 0, sizeof(s_jpg));
  s_jpg.pfnRead = jpg_read;
  s_jpg.pfnSeek = jpg_seek;
  s_jpg.pfnDraw = mq_draw;
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
  if (!JPEG_decode(&s_jpg, 0, 0, 0)) {
    DPRINTF("JPEG_decode failed (err %d) frame %lu\n", s_jpg.iError,
            (unsigned long)s_frame_index);
    /* Publish whatever bands landed; better than a stall. */
  }
  s_decode_us = time_us_32() - t0;
  mq_frame_done(); /* palette for the NEXT frame, from this frame */
  fb_publish();    /* VBL-synced hand-off to the ST */
}

/* --- player ------------------------------------------------------ */

static void mjpeg_rewind(void) {
  s_pos = s_movi_start;
  s_audio_left = 0;
  s_frame_index = 0;
  s_sample_clock = 0;
  aring_head = aring_tail = 0;
  prefill_audio();
  s_loops++;
  DPRINTF("MJPEG: loop %lu (shown %lu skipped %lu)\n",
          (unsigned long)s_loops, (unsigned long)s_frames_shown,
          (unsigned long)s_frames_skipped);
}

void mjpeg_service(void) {
  fb_pump_rom3();
  ikbd_pump();
  audio_render_frame();
}

uint32_t mjpeg_frames_shown(void) { return s_frames_shown; }

void mjpeg_debug_status(char *buf, int len) {
  snprintf(buf, (size_t)len, "F%lu S%lu I%lu E%d R%lu D%luus",
           (unsigned long)s_frames_shown, (unsigned long)s_frames_skipped,
           (unsigned long)s_frame_index, s_jpg.iError,
           (unsigned long)aring_level(), (unsigned long)s_decode_us);
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
  s_pos = s_movi_start;
  s_audio_left = 0;
  s_audio_skip = 0;
  s_frame_index = 0;
  s_sample_clock = 0;
  s_frames_shown = s_frames_skipped = s_loops = 0;
  aring_head = aring_tail = 0;
  prefill_audio();
  DPRINTF("MJPEG: primed %lu B of audio\n", (unsigned long)aring_level());
  audio_set_fill_callback(mjpeg_audio_fill);
  s_open = true;
  DPRINTF("MJPEG: playing %s\n", path);
  return 0;
}

void mjpeg_tick(void) {
  if (!s_open) return;
  for (;;) {
    /* Finish draining a partially-consumed audio chunk first. */
    if (s_audio_left) {
      if (aring_level() >= ARING_SIZE) return; /* ring full; later */
      if (f_lseek(&s_fil, s_audio_pos) != FR_OK) {
        mjpeg_rewind();
        return;
      }
      uint32_t n = aring_push_file(s_audio_left);
      s_audio_pos += n;
      s_audio_left -= n;
      if (s_audio_left) return; /* ring filled up */
      continue;
    }

    if (s_pos + 8 > s_movi_end) {
      mjpeg_rewind();
      return;
    }
    uint32_t fcc, size;
    if (rd_chunk_hdr(s_pos, &fcc, &size)) {
      mjpeg_rewind();
      return;
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
      uint32_t due = s_sample_clock / s_samples_per_frame;
      if (due < s_frame_index) {
        return; /* not due yet; cursor stays on this chunk */
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
      return; /* at most one video frame per tick */
    }
    s_pos = next; /* unknown chunk (index etc.) -- skip */
  }
}
