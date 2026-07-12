/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_menu.h
 * Description: Video-selection menu for the MJPEG player. Enumerates
 *              the .AVI files in the app folder into a list and draws
 *              a keyboard-navigable menu into the framebuffer. The
 *              MENU <-> PLAYING state machine and key handling live in
 *              emul.c; this module just owns the file list and its
 *              rendering.
 */

#ifndef MJPEG_MENU_H_INCLUDED
#define MJPEG_MENU_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* Scan `folder` (and, if empty, the card root) for *.AVI files and
 * build the menu list. Call once at boot, or again to rescan. */
void mjpeg_menu_init(const char *folder);

/* Number of files found (0 = none; the menu shows a placeholder). */
int mjpeg_menu_count(void);

/* Full path of entry `idx` (folder + name), for mjpeg_open(). Returns
 * a pointer to a shared buffer valid until the next call. */
const char *mjpeg_menu_path(int idx);

/* Draw the menu into fb_chunked_buffer with entry `selected`
 * highlighted, and publish it. Sets its own known palette, so it is
 * safe to call straight after the quantized video palette. */
void mjpeg_menu_render(int selected);

#ifdef __cplusplus
}
#endif

#endif /* MJPEG_MENU_H_INCLUDED */
