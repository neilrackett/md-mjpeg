/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: audio.c
 * Description: Cart-shared audio buffer producer.
 *
 * The m68k reads sample bytes from the cart buffer at
 * CART_AUDIO_BUFFER_OFFSET (1024 B) once per VBL: Timer-B plays them on
 * the YM, or the VBL loop copies them into the STE DMA buffer. The RP
 * refills the buffer from a VBL-synced timer interrupt on Core 1
 * (audio_start_vbl_timer). The library is format-agnostic -- it just
 * dispatches to whatever fill callback the app has installed.
 *
 * See audio.h for the public API and `audio_play_loop` /
 * `audio_set_fill_callback` semantics.
 */

#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "fb_chunked.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* Audio back-end is chosen at RUNTIME from the m68k's _SND detection,
 * reported over the cart bus (see fb.c / userfw.s). audio_set_mode()
 * switches the per-VBL refill size, and the fill callback keys off
 * audio_get_mode() to produce the matching format:
 *  - AUDIO_MODE_YM: 5,585 Hz, 112 samples/VBL x 2 B (vA,vB) = 224 B.
 *  - AUDIO_MODE_DMA: 25,033 Hz, 1 B (signed) a sample, as many as the
 *    m68k measures the chip eating per VBL (audio_sndlen_report):
 *    about 501.5 at 50 Hz, 350 at 71 Hz, 500 or 350 until it says.
 *  - AUDIO_MODE_SILENT: zeros until the first report arrives (zeros
 *    are silence for both the DMA and YM readers).
 * Default is SILENT so nothing plays garbage before the m68k reports.
 * In ST high res (audio_set_mono) a VBL is 71.47 Hz: 79 YM pairs (one
 * more than Timer-B's ~78.2, so a VBL that takes 79 never replays a
 * stale pair). */
#define AUDIO_FILL_BYTES_YM       224u
#define AUDIO_FILL_BYTES_DMA      500u
#define AUDIO_FILL_BYTES_YM_MONO  158u
#define AUDIO_FILL_BYTES_DMA_MONO 350u

/* The DMA length report is biased by the m68k's minimum so it fits one
 * byte; this must match STE_SND_LEN_MIN in userfw.s. A value outside
 * the range it steers in (STE_SND_LEN_MAX = 512) is a corrupt sample
 * rather than a length, and is ignored. */
#define AUDIO_SNDLEN_BIAS 320u
#define AUDIO_SNDLEN_MAX  512u

static audio_mode_t s_audio_mode = AUDIO_MODE_SILENT;
static bool s_st_mono;
static volatile uint32_t s_dma_bytes; /* the m68k's length; 0 = not yet */

/* The per-VBL refill size for the current back-end and VBL rate. */
static uint32_t fill_bytes(void) {
  if (s_audio_mode == AUDIO_MODE_YM) {
    return s_st_mono ? AUDIO_FILL_BYTES_YM_MONO : AUDIO_FILL_BYTES_YM;
  }
  if (s_dma_bytes) {
    return s_dma_bytes;
  }
  return s_st_mono ? AUDIO_FILL_BYTES_DMA_MONO : AUDIO_FILL_BYTES_DMA;
}

void audio_set_mode(audio_mode_t mode) {
  if (mode == s_audio_mode) {
    return;
  }
  s_audio_mode = mode;
  DPRINTF("audio_set_mode: %s (%u B/VBL)\n",
          mode == AUDIO_MODE_DMA ? "STE DMA" : mode == AUDIO_MODE_YM
                                                   ? "YM"
                                                   : "silent",
          (unsigned)fill_bytes());
}

void audio_set_mono(bool mono) {
  if (mono == s_st_mono) {
    return;
  }
  s_st_mono = mono;
  DPRINTF("audio_set_mono: %s VBLs (%u B/VBL)\n", mono ? "71 Hz" : "50 Hz",
          (unsigned)fill_bytes());
}

void audio_sndlen_report(uint32_t report) {
  const uint32_t bytes = AUDIO_SNDLEN_BIAS + report;
  if (bytes <= AUDIO_SNDLEN_MAX) {
    s_dma_bytes = bytes;
  }
}

audio_mode_t audio_get_mode(void) { return s_audio_mode; }

uint32_t audio_get_fill_bytes(void) { return fill_bytes(); }

static uint8_t *s_audio_buf;
static audio_fill_cb_t s_fill_cb;
static spin_lock_t *s_lock;

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_fill_cb = NULL;
  s_lock = spin_lock_init(spin_lock_claim_unused(true));

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the cart
   * buffer (= silence on YM). With no callback installed, the
   * buffer stays zero until an app calls audio_play_loop() or
   * audio_set_fill_callback(). */

  DPRINTF("audio_init: cart buffer %u B at offset $%04X, %u B/VBL refill\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE,
          (unsigned)CART_AUDIO_BUFFER_OFFSET, (unsigned)fill_bytes());
}

uint32_t audio_lock(void) { return spin_lock_blocking(s_lock); }

void audio_unlock(uint32_t saved) { spin_unlock(s_lock, saved); }

void audio_set_fill_callback(audio_fill_cb_t cb) {
  const uint32_t saved = audio_lock();
  s_fill_cb = cb;
  audio_unlock(saved);
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
  const uint32_t saved = audio_lock();
  s_loop_data = data;
  s_loop_bytes = bytes;
  s_loop_pos = 0;
  s_fill_cb = audio_loop_cb;
  audio_unlock(saved);
}

/* VBL-synced refill, after MD/DOOM. Every 1 ms the timer peeks at the
 * ROM3 ring (commemul_scan, with a cursor of its own, separate from
 * commemul_poll's) for the m68k's end-of-copy ack, and fills right
 * after it: the m68k has just copied the buffer out (to the DMA buffer,
 * or, for the YM, Timer-B is near its end), and won't again for a whole
 * VBL. Paced by the ST, not by the RP's clock, it can neither fall
 * behind and replay a buffer nor overwrite one before it is copied, and
 * the main loop being busy (a decode, a wait in fb_publish) doesn't
 * delay it. The whole $84xx window counts, so in mono the half-frame
 * ack ($FB8401) refills too: the m68k copies a buffer every VBL, not
 * every frame. A fill is never repeated within 5 ms, in case an ack is
 * seen twice across a scan boundary. */
#define AUDIO_VBLSYNC_HIBYTE  0x8400u
#define AUDIO_MIN_FILL_GAP_US 5000u

static repeating_timer_t s_vbl_timer;
static uint32_t s_scan_cursor;
static uint32_t s_last_fill_us;
static volatile uint32_t s_fills; /* since the last audio_take_fills() */

static bool __not_in_flash_func(audio_vbl_timer_cb)(repeating_timer_t *rt) {
  (void)rt;
  const uint32_t now = time_us_32();
  if (commemul_scan(&s_scan_cursor, AUDIO_VBLSYNC_HIBYTE) &&
      now - s_last_fill_us >= AUDIO_MIN_FILL_GAP_US) {
    s_last_fill_us = now;
    const uint32_t saved = audio_lock();
    if (s_fill_cb) {
      s_fill_cb(s_audio_buf, fill_bytes());
      s_fills++;
    }
    audio_unlock(saved);
  }
  return true;
}

/* Runs on Core 1 (as a framework job): a Core 1 alarm pool binds its
 * interrupt to Core 1, so the refill never interrupts Core 0. (MD/DOOM
 * measured the same timer on Core 0 stalling compute for 100-200 ms at
 * a time.) */
static void core1_start_timer_job(void *arg) {
  (void)arg;
  alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(4);
  alarm_pool_add_repeating_timer_us(pool, -1000, audio_vbl_timer_cb, NULL,
                                    &s_vbl_timer);
}

void audio_start_vbl_timer(void) {
  s_scan_cursor = 0;
  fb_core1_dispatch(core1_start_timer_job, NULL);
  fb_core1_wait();
  DPRINTF("audio: VBL-synced refill timer started on core 1\n");
}

uint32_t audio_take_fills(void) {
  const uint32_t n = s_fills;
  s_fills = 0;
  return n;
}
