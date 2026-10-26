/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg.h
 * Description: AVI (MJPEG video + unsigned 8-bit PCM audio) player.
 *
 * Streams a standard .AVI file from the SD card: '00dc' chunks are
 * baseline JPEG frames decoded straight into fb_chunked_buffer (with a
 * per-frame quantized 16-colour palette + 2x2 Bayer dither, see
 * mjpeg_quant.c), '01wb' chunks are mono u8 PCM pushed into a ring
 * that the audio fill callback converts to YM2149 (vA, vB) volume
 * pairs via the Ghostbusters LUT.
 *
 * A/V sync: the audio consumption clock is the master. The fill
 * callback advances a sample counter at the m68k's fixed consumption
 * rate (112 samples per VBL); video frame N is decoded+published when
 * the counter passes N * samples_per_frame. If decoding falls behind
 * by 2+ frames, frames are skipped (chunk seek, no decode) so audio
 * never stalls.
 *
 * Produce a compatible file with:
 *   ffmpeg -i in.mp4 -vf "fps=25,crop=...,scale=320:200" \
 *          -c:v mjpeg -q:v 7 -pix_fmt yuvj420p \
 *          -ac 1 -ar 5600 -c:a pcm_u8 out.avi
 * (5600 Hz = the 224 bytes/VBL the m68k Timer-B handler consumes.)
 */

#ifndef MJPEG_H_INCLUDED
#define MJPEG_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open an AVI file (absolute path), parse the RIFF headers, prefill
 * the audio ring and install the audio fill callback. Returns 0 on
 * success, negative on failure (open/parse error). */
int mjpeg_open(const char *path);

/* Stop playback: close the file and drain the audio ring to silence.
 * Safe to call when nothing is open. Call before opening another file
 * (mjpeg_open refuses while one is already open). */
void mjpeg_close(void);

/* Advance the player: buffer audio chunks, and decode+publish (or
 * skip) at most one video frame. Call once per main-loop iteration.
 * Returns quickly when no frame is due. Returns true while the movie
 * is still playing, false once it reaches the end (or on an
 * unrecoverable error / when nothing is open) -- the caller then
 * closes and returns to the menu. */
bool mjpeg_tick(void);

/* Keep the platform alive during long operations: drains the ROM3
 * ring (IKBD + VBL sync) and refills the cart audio buffer. Called
 * from the JPEG draw callback every few MCU rows so audio never
 * underruns during a ~30 ms decode. Safe to call at any time. */
void mjpeg_service(void);

/* --- playback controls ------------------------------------------- */

/* Seek `seconds` relative to the current position (negative = back).
 * MJPEG is all-intra so any frame is a valid target: forward walks the
 * chunk stream, backward uses a small ring of recent frame offsets.
 * Clamped to the movie bounds and briefly shows the time overlay. */
void mjpeg_seek(int seconds);

/* Toggle play/pause. Paused freezes the picture, silences the audio,
 * and keeps the time overlay up. */
void mjpeg_toggle_pause(void);
bool mjpeg_is_paused(void);

/* Redraw the frozen frame with the time overlay and publish it. Call
 * once per loop iteration while paused, in place of mjpeg_tick(). */
void mjpeg_render_paused(void);

/* Draw the menu (mjpeg_menu_draw_over) over every frame from now on, or
 * stop. The video carries on underneath, paused or playing; closing it
 * over a paused video redraws the frozen frame without it. No-op with no
 * video open; mjpeg_close() clears it. */
void mjpeg_show_menu(bool on);
bool mjpeg_menu_shown(void);

/* Frames successfully decoded+published since open (across loops). */
uint32_t mjpeg_frames_shown(void);

/* One-line player-state summary ("F.. S.. I.. E.. R.. D..us") for
 * on-screen diagnostics when playback stalls. */
void mjpeg_debug_status(char *buf, int len);

#ifdef __cplusplus
}
#endif

#endif /* MJPEG_H_INCLUDED */
