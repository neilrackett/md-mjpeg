/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: audio.h
 * Description: Cart-shared audio buffer producer + app-facing API.
 *
 * The m68k (target/atarist/src/userfw.s) reads sample bytes from a
 * 1024-byte cart buffer at CART_AUDIO_BUFFER_OFFSET once per VBL --
 * Timer-B plays them on the YM2149, or the VBL loop copies them into the
 * STE DMA sound buffer. The RP refills the buffer once per VBL from a
 * timer interrupt on Core 1 that waits for the m68k's end-of-copy ack
 * (audio_start_vbl_timer).
 *
 * Apps install audio content one of two ways:
 *
 *   1. audio_play_loop(data, bytes) -- convenience wrapper. The
 *      library installs a built-in callback that loops the given
 *      static buffer indefinitely. Typical use case for a baked-in
 *      jingle or sound effect.
 *
 *   2. audio_set_fill_callback(cb) -- low-level. The library
 *      invokes `cb(buf, bytes)` once per VBL refill with
 *      `bytes` set to the m68k's per-VBL consumption; the
 *      callback writes exactly that many bytes into `buf`. Use for
 *      streaming sources (e.g. SD-backed PCM, buffered by the app:
 *      the callback runs in an interrupt and must not touch the card).
 *
 * Sample format follows audio_get_mode(): (vA, vB) Ghostbusters-LUT
 * pairs for the YM, signed 8-bit PCM for STE DMA sound. The library is
 * format-agnostic; it just hands the callback the cart buffer.
 *
 * If no callback is installed, the refill is a no-op and the cart
 * buffer stays whatever audio_init() left it (zero = silence). Calling
 * audio_set_fill_callback(NULL) re-enters this silent state.
 */

#ifndef AUDIO_H_INCLUDED
#define AUDIO_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-VBL fill callback. The library invokes this from the refill
 * interrupt with `buf` pointing into the cart audio buffer and `bytes`
 * set to the m68k's per-VBL consumption (224 = 112 YM pairs at Timer-B
 * 5,585 Hz, or the DMA length the m68k measures, ~501). The callback
 * must write exactly that many bytes; the library does not zero on
 * entry.
 *
 * Called once per VBL on Core 1, in interrupt context and holding
 * audio_lock(): it must be quick, must not block, and must not touch
 * the SD card or anything else Core 0 uses unguarded. */
typedef void (*audio_fill_cb_t)(uint8_t *buf, uint32_t bytes);

/* Audio back-end, chosen at runtime from the m68k's _SND detection
 * (reported over the cart bus, decoded in fb.c). Governs the per-VBL
 * refill size and the format the fill callback produces. Defaults to
 * SILENT until the first report arrives. */
typedef enum {
  AUDIO_MODE_SILENT = 0, /* zeros -- pre-report / no audio    */
  AUDIO_MODE_YM,         /* 224 B/VBL: 112 (vA,vB) pairs (158 B mono) */
  AUDIO_MODE_DMA         /* ~501 B/VBL of signed PCM (~350 B mono)     */
} audio_mode_t;

/* Set the back-end (idempotent). Called by the cart-bus capability
 * decoder; changes the refill size the fill callback is asked for. */
void audio_set_mode(audio_mode_t mode);

/* ST high res (idempotent): VBLs are 71.47 Hz there, so the refill comes
 * every ~14 ms and is smaller. Called by fb.c's display-report decoder. */
void audio_set_mono(bool mono);

/* STE DMA sound length: the m68k's report (SNDLEN_WINDOW_BASE in
 * userfw.s), the bytes it measures the chip eating per VBL less its
 * bias. Called by fb.c's decoder; sets the DMA refill size. */
void audio_sndlen_report(uint32_t report);

/* Current back-end, for the fill callback to pick its output format. */
audio_mode_t audio_get_mode(void);

/* The current per-VBL refill size, in bytes. */
uint32_t audio_get_fill_bytes(void);

/* Initialise the cart audio buffer pointer; clear any previously
 * installed callback. Call once during boot. */
void audio_init(void);

/* Start the VBL-synced refill: a 1 ms timer on Core 1 that runs the
 * fill callback after each m68k end-of-copy ack. Call once at boot,
 * after fb_init() has launched Core 1 and after audio_init(). */
void audio_start_vbl_timer(void);

/* The lock the fill callback runs under. Take it around any change to
 * state the callback reads (a ring reset, a clock reset); keep it brief
 * and never wait on Core 1 while holding it. Masks interrupts on the
 * calling core; audio_unlock() restores them. */
uint32_t audio_lock(void);
void audio_unlock(uint32_t saved);

/* Refills since the last call, for a heartbeat (~50 a second, ~71 in
 * mono). */
uint32_t audio_take_fills(void);

/* Install (or clear, if cb == NULL) the fill callback. */
void audio_set_fill_callback(audio_fill_cb_t cb);

/* Convenience: register a built-in callback that loops a static
 * byte buffer indefinitely. `data` must remain live for as long
 * as playback continues (typically a `static const uint8_t[]`
 * baked into the firmware -- e.g. audio_sample_data[] generated
 * by tools/wav_to_ym4.py). `bytes` is the total length of one
 * loop iteration; playback wraps at byte `bytes-1` back to byte 0.
 *
 * Replaces any previously installed callback. */
void audio_play_loop(const uint8_t *data, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H_INCLUDED */
