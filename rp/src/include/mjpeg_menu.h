/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_menu.h
 * Description: Video-selection menu for the MJPEG player. Enumerates
 *              the .AVI files in the app folder and draws them, below
 *              the ABOUT and EXIT TO GEM rows, as a keyboard-navigable
 *              MiSTer-style box (MD/Lynx's layout), on its own or over a
 *              playing video. The MENU <-> PLAYING state machine lives in
 *              emul.c; this module owns the file list, the selection, the
 *              About page and their rendering.
 */

#ifndef MJPEG_MENU_H_INCLUDED
#define MJPEG_MENU_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scan `folder` (and, if empty, the card root) for *.AVI files and
 * build the menu list, selecting the first video. Call once at boot,
 * or again to rescan. */
void mjpeg_menu_init(const char *folder);

/* Number of files found (0 = none; the menu shows a placeholder). */
int mjpeg_menu_count(void);

/* Full path of entry `idx` (folder + name), for mjpeg_open(). Returns
 * a pointer to a shared buffer valid until the next call. */
const char *mjpeg_menu_path(int idx);

/* mjpeg_menu_key() results that are not a video index. */
#define MJPEG_MENU_NONE (-1) /* dealt with by the menu (or nothing to do) */
#define MJPEG_MENU_BACK (-2) /* ESC: back to the video, or to GEM if none */

/* Handle a key press (an IKBD make scancode) while the menu is up:
 * Up/Down move, Return/Enter/Space select, and any key closes the About
 * page. Returns the index of the video to play, MJPEG_MENU_BACK for ESC,
 * or MJPEG_MENU_NONE when the menu dealt with the key itself (including
 * ABOUT and EXIT TO GEM). */
int mjpeg_menu_key(uint8_t scancode);

/* Draw the menu (or the About page) on its own, on a cleared screen, and
 * publish it. Sets its own known palette, so it is safe to call straight
 * after the quantized video palette. */
void mjpeg_menu_render(void);

/* Draw the menu (or the About page) over the frame in fb_chunked_buffer,
 * in the frame's pens 0 and 15 (see mq_republish); the caller
 * publishes. For the menu over a playing video. */
void mjpeg_menu_draw_over(void);

#ifdef __cplusplus
}
#endif

#endif /* MJPEG_MENU_H_INCLUDED */
