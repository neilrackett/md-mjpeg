/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_menu.c
 * Description: Video-selection menu -- enumerates *.AVI in the app
 *              folder and draws a keyboard-navigable list. See
 *              mjpeg_menu.h; the state machine lives in emul.c.
 */

#include "mjpeg_menu.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "palette.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

/* Up to 20 rows fit the 320x200 screen below the title and above the
 * hint. Names are stored truncated -- ~40 chars fit across at 8 px. */
#define MENU_MAX_FILES 20
#define MENU_NAME_LEN 48
#define MENU_TOP 22             /* y of the first list row      */
#define MENU_ROW_H 8            /* font8x8 row height           */
#define MENU_X 16               /* list text left edge          */
#define MENU_BAR_X 0            /* highlight bar: full screen width, aligning */
#define MENU_BAR_W FB_CHUNKED_W /* with the title/credit line edges  */

static char s_names[MENU_MAX_FILES][MENU_NAME_LEN];
static int s_count;
static char s_folder[64];
static char s_path[300];

/* Append every real *.AVI in `folder` to the list (up to the cap).
 * Skips directories, hidden/system entries, and dot-files -- notably
 * the "._name.AVI" AppleDouble sidecars macOS writes to the card,
 * which otherwise match the *.AVI pattern and clutter the menu. */
static void menu_scan(const char *folder) {
  DIR dir;
  FILINFO fno;
  FRESULT fr = f_findfirst(&dir, &fno, folder, "*.AVI");
  while (fr == FR_OK && fno.fname[0] != '\0' && s_count < MENU_MAX_FILES) {
    if (fno.fname[0] != '.' && !(fno.fattrib & (AM_DIR | AM_HID | AM_SYS))) {
      snprintf(s_names[s_count], MENU_NAME_LEN, "%s", fno.fname);
      s_count++;
    }
    fr = f_findnext(&dir, &fno);
  }
  f_closedir(&dir);
}

/* Case-insensitive filename compare, for a friendly A..Z ordering. */
static int name_ci_cmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = tolower((unsigned char)*a);
    int cb = tolower((unsigned char)*b);
    if (ca != cb) return ca - cb;
    if (ca == 0) return 0;
  }
}

/* Insertion sort the (small) list alphabetically. */
static void menu_sort(void) {
  char tmp[MENU_NAME_LEN];
  for (int i = 1; i < s_count; i++) {
    memcpy(tmp, s_names[i], MENU_NAME_LEN);
    int j = i - 1;
    while (j >= 0 && name_ci_cmp(s_names[j], tmp) > 0) {
      memcpy(s_names[j + 1], s_names[j], MENU_NAME_LEN);
      j--;
    }
    memcpy(s_names[j + 1], tmp, MENU_NAME_LEN);
  }
}

void mjpeg_menu_init(const char *folder) {
  s_count = 0;
  snprintf(s_folder, sizeof(s_folder), "%s", folder ? folder : "/mjpeg");
  menu_scan(s_folder);
  if (s_count == 0 && strcmp(s_folder, "/") != 0) {
    /* Fall back to the card root so files put there still show up. */
    snprintf(s_folder, sizeof(s_folder), "/");
    menu_scan(s_folder);
  }
  menu_sort();
}

int mjpeg_menu_count(void) { return s_count; }

const char *mjpeg_menu_path(int idx) {
  if (idx < 0 || idx >= s_count) {
    return "";
  }
  snprintf(s_path, sizeof(s_path), "%s/%s",
           (s_folder[1] == '\0') ? "" : s_folder, s_names[idx]);
  return s_path;
}

void mjpeg_menu_render(int selected) {
  /* Known palette, overriding any quantized palette a previous video
   * left in place. 0 = black (ST border register 0 + background),
   * 1 = pastel blue (title), 2 = medium-dark grey (credit), 15 = white
   * (file list + title slash). */
  palette_init();
  palette_set_entry(0, PALETTE_RGB(0, 0, 0));
  palette_set_entry(1, PALETTE_RGB(3, 4, 7));
  palette_set_entry(2, PALETTE_RGB(2, 2, 2));
  palette_set_entry(15, PALETTE_RGB(7, 7, 7));
  fb_chunked_clear(0);
  font_set_font(&font8x8);

  /* Title top-left: "MD" / "MJPEG" in pastel blue, the slash white. */
  font_align(FONT_ALIGN_LEFT);
  font_set_color(1);
  font_move(4, 4);
  font_print("MD");
  font_set_color(15);
  font_move(4 + 2 * 8, 4);
  font_print("/");
  font_set_color(1);
  font_move(4 + 3 * 8, 4);
  font_print("MJPEG");

  /* Credit top-right, medium-dark grey. */
  font_align(FONT_ALIGN_RIGHT);
  font_set_color(2);
  font_move(FB_CHUNKED_W - 4, 4);
  font_print("x.com/neilrackett");

  if (s_count == 0) {
    font_align(FONT_ALIGN_CENTER);
    font_set_color(15);
    font_move(FB_CHUNKED_W / 2, 96);
    font_print("NO .AVI FILES ON SD CARD");
  } else {
    font_align(FONT_ALIGN_LEFT);
    for (int i = 0; i < s_count; i++) {
      int y = MENU_TOP + i * MENU_ROW_H;
      if (i == selected) {
        fb_fill_rect(MENU_BAR_X, y, MENU_BAR_W, MENU_ROW_H, 15); /* white bar */
        font_set_color(0); /* black text */
      } else {
        font_set_color(15); /* white text */
      }
      font_move(MENU_X, y);
      font_print(s_names[i]);
    }
  }

  font_align(FONT_ALIGN_CENTER);
  font_set_color(2); /* medium-dark grey, matching the credit line */
  font_move(FB_CHUNKED_W / 2, 190);
  font_print("Return/PLAY   Esc/QUIT");

  font_align(FONT_ALIGN_LEFT);
  fb_publish();
}
