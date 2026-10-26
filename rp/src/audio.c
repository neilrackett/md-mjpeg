/**
 * File: audio.c
 * Description: Cart-shared audio buffer producer.
 *
 * The m68k Timer-B IRQ reads sample bytes from the cart buffer at
 * CART_AUDIO_BUFFER_OFFSET (1024 B). The RP refills the buffer
 * once per VBL via audio_render_frame() (paced to ~50 Hz via
 * time_us_32). The library is format-agnostic -- it just dispatches
 * to whatever fill callback the app has installed.
 *
 * See audio.h for the public API and `audio_play_loop` /
 * `audio_set_fill_callback` semantics.
 */

#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* Per-VBL fill cadence. Refill every ~20 ms (one PAL VBL) so the
 * RP keeps pace with the m68k's Timer-B reads without burning more
 * main-loop cycles than necessary. In ST high res a VBL is 71.47 Hz,
 * so ~14 ms. */
#define AUDIO_FRAME_PERIOD_US 20000u
#define AUDIO_FRAME_PERIOD_US_MONO 14000u

/* Audio back-end is chosen at RUNTIME from the m68k's _SND detection,
 * reported over the cart bus (see fb.c / userfw.s). audio_set_mode()
 * switches the per-VBL refill size, and the fill callback keys off
 * audio_get_mode() to produce the matching format:
 *  - AUDIO_MODE_YM: 5,585 Hz, 112 samples/VBL x 2 B (vA,vB) = 224 B.
 *  - AUDIO_MODE_DMA: 25,033 Hz, 500 samples/VBL x 1 B (signed) = 500 B.
 *  - AUDIO_MODE_SILENT: 500 B of zeros until the first report arrives
 *    (zeros are silence for both the DMA and YM readers).
 * Default is SILENT so nothing plays garbage before the m68k reports.
 * In ST high res (audio_set_mono) a VBL is 71.47 Hz: 79 YM pairs (one
 * more than Timer-B's ~78.2, so a VBL that takes 79 never replays a
 * stale pair) and 350 DMA bytes, which userfw.s copies to match. */
#define AUDIO_FILL_BYTES_YM       224u
#define AUDIO_FILL_BYTES_DMA      500u
#define AUDIO_FILL_BYTES_YM_MONO  158u
#define AUDIO_FILL_BYTES_DMA_MONO 350u

static audio_mode_t s_audio_mode = AUDIO_MODE_SILENT;
static uint32_t s_fill_bytes = AUDIO_FILL_BYTES_DMA;
static bool s_st_mono;

static uint32_t fill_bytes_for(audio_mode_t mode) {
  if (mode == AUDIO_MODE_YM) {
    return s_st_mono ? AUDIO_FILL_BYTES_YM_MONO : AUDIO_FILL_BYTES_YM;
  }
  return s_st_mono ? AUDIO_FILL_BYTES_DMA_MONO : AUDIO_FILL_BYTES_DMA;
}

void audio_set_mode(audio_mode_t mode) {
  if (mode == s_audio_mode) {
    return;
  }
  s_audio_mode = mode;
  s_fill_bytes = fill_bytes_for(mode);
  DPRINTF("audio_set_mode: %s (%u B/VBL)\n",
          mode == AUDIO_MODE_DMA ? "STE DMA" : mode == AUDIO_MODE_YM
                                                   ? "YM"
                                                   : "silent",
          (unsigned)s_fill_bytes);
}

void audio_set_mono(bool mono) {
  if (mono == s_st_mono) {
    return;
  }
  s_st_mono = mono;
  s_fill_bytes = fill_bytes_for(s_audio_mode);
  DPRINTF("audio_set_mono: %s VBLs (%u B/VBL)\n", mono ? "71 Hz" : "50 Hz",
          (unsigned)s_fill_bytes);
}

audio_mode_t audio_get_mode(void) { return s_audio_mode; }

static uint8_t *s_audio_buf;
static uint32_t s_last_frame_us;
static audio_fill_cb_t s_fill_cb;

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

/* .YMS-file streaming state. audio_play_yms_file() validates the
 * 16-byte header, stores the data offset, and installs audio_yms_cb
 * as the per-VBL fill callback. The file is kept open for the
 * lifetime of playback. */
#define AUDIO_YMS_HEADER_SIZE     16u
#define AUDIO_YMS_MODE_DUAL_GHOST 1u
/* Must match TIMERB_COUNT in target/atarist/src/userfw.s:
 *   2.4576 MHz / 4 / 110 = 5,585.45 Hz */
#define AUDIO_NATIVE_RATE_HZ      5585u

static FIL s_yms_file;
static bool s_yms_open;
static FSIZE_t s_yms_data_offset;

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_last_frame_us = 0;
  s_fill_cb = NULL;
  s_yms_open = false;

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the cart
   * buffer (= silence on YM). With no callback installed, the
   * buffer stays zero until an app calls audio_play_loop() or
   * audio_set_fill_callback(). */

  DPRINTF("audio_init: cart buffer %u B at offset $%04X, %u B/VBL refill\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE,
          (unsigned)CART_AUDIO_BUFFER_OFFSET, (unsigned)s_fill_bytes);
}

void audio_set_fill_callback(audio_fill_cb_t cb) {
  s_fill_cb = cb;
}

static void audio_loop_cb(uint8_t *buf, uint32_t bytes) {
  uint32_t pos = s_loop_pos;
  const uint32_t total = s_loop_bytes;
  const uint8_t *src = s_loop_data;
  for (uint32_t i = 0; i < bytes; i++) {
    buf[i] = src[pos];
    pos++;
    if (pos >= total) {
      pos = 0;  /* loop */
    }
  }
  s_loop_pos = pos;
}

void audio_play_loop(const uint8_t *data, uint32_t bytes) {
  s_loop_data = data;
  s_loop_bytes = bytes;
  s_loop_pos = 0;
  s_fill_cb = audio_loop_cb;
}

static void audio_yms_cb(uint8_t *buf, uint32_t bytes) {
  UINT br = 0;
  FRESULT res = f_read(&s_yms_file, buf, bytes, &br);
  if (res != FR_OK) {
    /* I/O error -- silence until the next call. The cursor is in
     * an undefined state, so seek back to the data start for the
     * next attempt. */
    for (uint32_t i = 0; i < bytes; i++) {
      buf[i] = 0;
    }
    f_lseek(&s_yms_file, s_yms_data_offset);
    return;
  }
  if (br < bytes) {
    /* EOF mid-fill: wrap to data start and read the remainder. */
    f_lseek(&s_yms_file, s_yms_data_offset);
    UINT br2 = 0;
    f_read(&s_yms_file, buf + br, bytes - br, &br2);
    /* If the file body is shorter than one VBL's worth, pad the
     * still-unfilled tail with silence rather than reading the
     * header bytes. */
    for (uint32_t i = br + br2; i < bytes; i++) {
      buf[i] = 0;
    }
  }
}

int audio_play_yms_file(const char *path) {
  /* Close any previously open YMS file. Idempotent on a fresh init. */
  if (s_yms_open) {
    f_close(&s_yms_file);
    s_yms_open = false;
  }

  FRESULT res = f_open(&s_yms_file, path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("audio_play_yms_file: f_open('%s') failed (%d)\n", path, (int)res);
    return -1;
  }

  uint8_t hdr[AUDIO_YMS_HEADER_SIZE];
  UINT br = 0;
  res = f_read(&s_yms_file, hdr, sizeof(hdr), &br);
  if (res != FR_OK || br != sizeof(hdr)) {
    DPRINTF("audio_play_yms_file: short header read (%d, %u/%u)\n",
            (int)res, (unsigned)br, (unsigned)sizeof(hdr));
    f_close(&s_yms_file);
    return -1;
  }

  if (memcmp(hdr, "YMS1", 4) != 0) {
    DPRINTF("audio_play_yms_file: bad magic %02X%02X%02X%02X\n",
            hdr[0], hdr[1], hdr[2], hdr[3]);
    f_close(&s_yms_file);
    return -1;
  }

  uint32_t rate = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8)
                | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (rate != AUDIO_NATIVE_RATE_HZ) {
    DPRINTF("audio_play_yms_file: rate mismatch (file %lu, expected %u)\n",
            (unsigned long)rate, (unsigned)AUDIO_NATIVE_RATE_HZ);
    f_close(&s_yms_file);
    return -1;
  }

  if (hdr[12] != AUDIO_YMS_MODE_DUAL_GHOST) {
    DPRINTF("audio_play_yms_file: unsupported mode tag %u (need %u)\n",
            (unsigned)hdr[12], (unsigned)AUDIO_YMS_MODE_DUAL_GHOST);
    f_close(&s_yms_file);
    return -1;
  }

  s_yms_open = true;
  s_yms_data_offset = AUDIO_YMS_HEADER_SIZE;
  s_fill_cb = audio_yms_cb;

  DPRINTF("audio_play_yms_file: '%s' streaming (rate %lu Hz, mode %u)\n",
          path, (unsigned long)rate, (unsigned)hdr[12]);
  return 0;
}

void audio_render_frame(void) {
  if (s_fill_cb == NULL) {
    return;
  }

  uint32_t now_us = time_us_32();
  const uint32_t period_us =
      s_st_mono ? AUDIO_FRAME_PERIOD_US_MONO : AUDIO_FRAME_PERIOD_US;
  if (s_last_frame_us != 0 && (now_us - s_last_frame_us) < period_us) {
    return;
  }
  s_last_frame_us = now_us;

  s_fill_cb(s_audio_buf, s_fill_bytes);
}
