/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mjpeg_menu.c
 * Description: Video-selection menu -- enumerates *.AVI in the app
 *              folder and draws it as a MiSTer-style OSD box, the same
 *              layout as MD/Lynx's: a strip down the left edge carries
 *              the title, the settings rows sit on top and the videos
 *              below. The same box is drawn on its own or over a playing
 *              video. See mjpeg_menu.h; the MENU <-> PLAYING state
 *              machine lives in emul.c.
 */

#include "mjpeg_menu.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "palette.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

/* Names are stored truncated; the box shows MENU_TEXT_COLS of them. */
#define MENU_MAX_FILES 20
#define MENU_NAME_LEN 48

/* Font cell metrics (font8x8): 40 columns x 25 rows over 320x200. */
#define CELL_W 8
#define CELL_H 8

/* Menu palette. Index 0 also drives the ST border, so black takes it:
 * the screen around the box and the border beyond it are then the
 * same, and the box reads as a box. The menu itself is two pens, navy
 * and white (see struct menu_style). */
#define PEN_BG 0
#define PEN_INK 1
#define PEN_PANEL 6

static const uint16_t MenuPalette[PALETTE_ENTRIES] = {
    PALETTE_RGB(0, 0, 0), /*  0 black -- background + ST border */
    PALETTE_RGB(7, 7, 7), /*  1 white -- ink                    */
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 2), /*  6 navy  -- box fill               */
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0),
};

/* ST high res (fb_publish_levels): black and white alone would lose the
 * box against the screen around it, so the screen takes a third tone, a
 * 50% grey dither, and the box is black with white ink. */
static const uint8_t MenuMonoLevels[PALETTE_ENTRIES] = {
    [PEN_BG] = 8,
    [PEN_INK] = 16,
    [PEN_PANEL] = 0,
};

static char s_names[MENU_MAX_FILES][MENU_NAME_LEN];
static int s_count;
static char s_home[64];   /* the configured folder, for the empty message */
static char s_folder[64]; /* where the list came from (may be the root) */
static char s_path[300];
static int s_row;         /* the selected row (see Navigation) */
static bool s_about;      /* the About page is up */

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

/* ============================== Navigation =============================== */

/* The menu is one flat list, as in MD/Lynx: the settings rows on top,
 * then -- when there are videos -- a blank, unselectable row, then the
 * videos. The selection is simply a row. */
#define ROW_EXIT 0
#define ROW_ABOUT 1
#define ROW_SETTINGS 2

/* Each setting reads the way MiSTer's do: its name on the left and its
 * value, if it has one, right-aligned against the far edge. */
static const char *const SettingsLabel[ROW_SETTINGS] = {"EXIT TO GEM",
                                                        "ABOUT"};
static const char *const SettingsValue[ROW_SETTINGS] = {"", RELEASE_VERSION};

static int first_video_row(void) { return ROW_SETTINGS + (s_count > 0); }
static int menu_rows_total(void) { return first_video_row() + s_count; }
static bool row_is_gap(int row) { return s_count > 0 && row == ROW_SETTINGS; }

/* Up or down one row, wrapping, and over the gap (never at either end). */
static void ui_move(int dir) {
  const int total = menu_rows_total();
  int row = (s_row + dir + total) % total;
  if (row_is_gap(row)) row += dir;
  s_row = row;
}

/* ================================ Drawing ================================ */

/* MENU_ROWS rows of text, inset MENU_PAD from the top and bottom of the
 * box. The box is 30 cells wide: the strip takes two of them and the
 * list's text keeps one cell clear at either end, which leaves
 * MENU_TEXT_COLS for a video's name. */
#define MENU_ROWS 16
#define MENU_PAD 4
#define MENU_W (30 * CELL_W)
#define MENU_H (MENU_ROWS * CELL_H + 2 * MENU_PAD)
#define MENU_X ((FB_CHUNKED_W - MENU_W) / 2)
#define MENU_Y ((FB_CHUNKED_H - MENU_H) / 2)

#define MENU_STRIP_W (2 * CELL_W)
#define MENU_LIST_X (MENU_X + MENU_STRIP_W)
#define MENU_LIST_W (MENU_W - MENU_STRIP_W)
#define MENU_TEXT_X (MENU_LIST_X + CELL_W)     /* left-aligned text  */
#define MENU_TEXT_R (MENU_X + MENU_W - CELL_W) /* right-aligned text */
#define MENU_TEXT_COLS ((MENU_TEXT_R - MENU_TEXT_X) / CELL_W)
#define MENU_ROW_Y(i) (MENU_Y + MENU_PAD + (i) * CELL_H)

/* Two pens, like MiSTer's OSD: everything is ink on the fill, and the
 * strip and the selection bar are the fill on ink. */
struct menu_style {
  uint8_t bg;  /* box fill, and the ink on the strip and selection bar */
  uint8_t ink; /* text, the strip and the selection bar               */
};

/* On its own the menu has its own palette's navy and white. Over a
 * video it has the frame's pens 0 and 15: the quantizer's 0 is always
 * black and it holds 15 at white while the menu is up (mq_republish),
 * and mono's grey ramp makes them black and white anyway. */
static const struct menu_style MenuOwn = {PEN_PANEL, PEN_INK};
static const struct menu_style MenuOverVideo = {0, PALETTE_ENTRIES - 1};

/* Text at a pixel position; `x` is the left edge, the centre or the
 * right edge according to `align`. */
static void text_px(int x, int y, uint8_t pen, const char *s,
                    enum FONT_ALIGNMENT align) {
  font_set_font(&font8x8);
  font_set_color(pen);
  font_set_border(0, 0);
  font_align(align);
  font_move((unsigned)x, (unsigned)y);
  font_print(s);
}

/* Text turned a quarter-turn anticlockwise, reading upwards from
 * `y_bottom`, the way MiSTer labels its OSD strip. Each glyph row
 * becomes a column, the top of the glyph facing left. */
static void text_up(int x, int y_bottom, uint8_t pen, const char *s) {
  const struct FB_FONT *f = &font8x8;
  for (; *s; s++, y_bottom -= f->w) {
    const int ch = (unsigned char)*s - f->first_char;
    if (ch < 0 || ch >= f->num_chars) continue;
    const unsigned char *glyph = &f->data[ch * f->h];
    for (int gy = 0; gy < f->h; gy++) {
      for (int gx = 0; gx < f->w; gx++) {
        if (glyph[gy] & (1u << gx)) {
          fb_chunked_buffer[(y_bottom - gx) * FB_CHUNKED_W + x + gy] = pen;
        }
      }
    }
  }
}

/* Centre `s` across the list. */
static void menu_centred(const struct menu_style *st, int row, const char *s) {
  text_px(MENU_LIST_X + MENU_LIST_W / 2, MENU_ROW_Y(row), st->ink, s,
          FONT_ALIGN_CENTER);
}

/* A small solid triangle centred on `cx`, its first row at `y`: the
 * strip's hint that the list carries on that way. */
static void draw_arrow(int cx, int y, bool down, uint8_t pen) {
  for (int i = 0; i < 4; i++) {
    const int half = down ? 3 - i : i;
    fb_fill_rect(cx - half, y + i, 2 * half + 1, 1, pen);
  }
}

/* The box and its strip, with an arrow on the strip wherever the list
 * runs on above or below what is shown. */
static void draw_box(const struct menu_style *st, bool more_above,
                     bool more_below) {
  static const char title[] = "MD/MJPEG";
  const int len = (int)(sizeof(title) - 1) * CELL_W;

  fb_fill_rect(MENU_X, MENU_Y, MENU_STRIP_W, MENU_H, st->ink);
  fb_fill_rect(MENU_LIST_X, MENU_Y, MENU_LIST_W, MENU_H, st->bg);
  text_up(MENU_X + (MENU_STRIP_W - CELL_H) / 2,
          MENU_Y + (MENU_H + len) / 2 - 1, st->bg, title);

  const int cx = MENU_X + MENU_STRIP_W / 2;
  if (more_above) draw_arrow(cx, MENU_Y + MENU_PAD, false, st->bg);
  if (more_below) {
    draw_arrow(cx, MENU_Y + MENU_H - MENU_PAD - 4, true, st->bg);
  }
}

/* A video's name without its extension, cut to the box's width. */
static void display_name(const char *name, char *out, size_t cap) {
  snprintf(out, cap, "%s", name);
  char *dot = strrchr(out, '.');
  if (dot) *dot = '\0';
  if (strlen(out) > (size_t)MENU_TEXT_COLS) out[MENU_TEXT_COLS] = '\0';
}

static void draw_menu(const struct menu_style *st) {
  const int total = menu_rows_total();
  int first = s_row - MENU_ROWS / 2;
  if (first > total - MENU_ROWS) first = total - MENU_ROWS;
  if (first < 0) first = 0;

  draw_box(st, first > 0, first + MENU_ROWS < total);

  char line[MENU_NAME_LEN];
  for (int i = 0; i < MENU_ROWS; i++) {
    const int row = first + i;
    if (row >= total) break;
    if (row_is_gap(row)) continue; /* separator: nothing to draw */
    const int y = MENU_ROW_Y(i);

    /* The selected row is inverted right across the list, hard against
     * the strip, as MiSTer draws it. */
    uint8_t pen = st->ink;
    if (row == s_row) {
      fb_fill_rect(MENU_LIST_X, y, MENU_LIST_W, CELL_H, st->ink);
      pen = st->bg;
    }

    if (row >= ROW_SETTINGS) {
      display_name(s_names[row - first_video_row()], line, sizeof(line));
      text_px(MENU_TEXT_X, y, pen, line, FONT_ALIGN_LEFT);
    } else {
      text_px(MENU_TEXT_X, y, pen, SettingsLabel[row], FONT_ALIGN_LEFT);
      text_px(MENU_TEXT_R, y, pen, SettingsValue[row], FONT_ALIGN_RIGHT);
    }
  }

  if (s_count == 0) {
    menu_centred(st, ROW_SETTINGS + 1, "NO .AVI FILES");
    menu_centred(st, ROW_SETTINGS + 2, s_home);
  }
}

/* Kept inside MENU_TEXT_COLS so it fits the same box as the menu. */
static void draw_about(const struct menu_style *st) {
  draw_box(st, false, false);

  int y = 0;
  menu_centred(st, y++, "MD/MJPEG " RELEASE_VERSION);
  y++;
  menu_centred(st, y++, "MJPEG VIDEO PLAYER");
  menu_centred(st, y++, "neilrackett.com");
  y += 2;
  menu_centred(st, y++, "JPEG: JPEGDEC, APACHE-2.0");
  menu_centred(st, y++, "(C) LARRY BANK");
  y++;

  switch (audio_get_mode()) {
    case AUDIO_MODE_DMA:
      menu_centred(st, y++, "SOUND: STE DMA");
      break;
    case AUDIO_MODE_YM:
      menu_centred(st, y++, "SOUND: YM2149");
      break;
    default:
      menu_centred(st, y++, "SOUND: DETECTING");
      break;
  }

  menu_centred(st, MENU_ROWS - 1, "PRESS ANY KEY");
}

static void draw(const struct menu_style *st) {
  if (s_about) {
    draw_about(st);
  } else {
    draw_menu(st);
  }
}

/* ================================== API ================================== */

void mjpeg_menu_init(const char *folder) {
  s_count = 0;
  snprintf(s_home, sizeof(s_home), "%s", folder ? folder : "/mjpeg");
  snprintf(s_folder, sizeof(s_folder), "%s", s_home);
  menu_scan(s_folder);
  if (s_count == 0 && strcmp(s_folder, "/") != 0) {
    /* Fall back to the card root so files put there still show up. */
    snprintf(s_folder, sizeof(s_folder), "/");
    menu_scan(s_folder);
  }
  menu_sort();
  /* Open on the first video, with the settings scrolled above it. */
  s_row = (s_count > 0) ? first_video_row() : ROW_ABOUT;
  s_about = false;
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

int mjpeg_menu_key(uint8_t scancode) {
  /* The About page is modal: any key dismisses it and is swallowed. */
  if (s_about) {
    s_about = false;
    return MJPEG_MENU_NONE;
  }
  switch (scancode) {
    case ST_SC_UP:
      ui_move(-1);
      break;
    case ST_SC_DOWN:
      ui_move(1);
      break;
    case ST_SC_RETURN:
    case ST_SC_KP_ENTER:
    case ST_SC_SPACE:
      if (s_row >= ROW_SETTINGS) return s_row - first_video_row();
      if (s_row == ROW_ABOUT) {
        s_about = true;
      } else {
        ikbd_request_boot_gem(); /* ROW_EXIT */
      }
      break;
    case ST_SC_ESC:
      return MJPEG_MENU_BACK; /* the caller's: back to the video, or GEM */
    default:
      break;
  }
  return MJPEG_MENU_NONE;
}

void mjpeg_menu_render(void) {
  /* Known palette, overriding any quantized palette a previous video
   * left in place. */
  palette_set(MenuPalette);
  fb_chunked_clear(PEN_BG);
  draw(&MenuOwn);
  fb_publish_levels(MenuMonoLevels);
}

void mjpeg_menu_draw_over(void) { draw(&MenuOverVideo); }
