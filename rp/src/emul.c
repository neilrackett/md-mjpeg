/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: emul.c
 * Description: MD/MJPEG -- streams an MJPEG+PCM .AVI from the SD card
 *              to the Atari ST screen at up to 25 fps with a per-frame
 *              quantized 16-colour palette and YM2149 audio.
 *
 * Boot block (cart bus, framebuffer, SD, SELECT) is the template's;
 * the app is mjpeg.c / mjpeg_quant.c driven from the main loop. The
 * first .AVI found in the app folder is played on loop; ESC exits to
 * GEM (default ikbd.c behaviour).
 */

#include "emul.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aconfig.h"
#include "audio.h"
#include "commemul.h"
#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "memfunc.h"
#include "mjpeg.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"

extern const struct FB_FONT font8x8; /* defined in fb.c */

/* Cart-corruption canary: the first 16 bytes of the served cart region
 * (the m68k cartridge header + entry) must stay byte-identical to the
 * embedded image after COPY_FIRMWARE_TO_RAM. Any change means something
 * on the RP wrote into the region the m68k is executing from -- which
 * crashes the ST with bombs. Logs the first corruption with a stage
 * tag so the culprit can be localized. */
static bool cart_check(const char *stage) {
  static bool ok = true;
  if (ok &&
      memcmp((const void *)&__rom_in_ram_start__, target_firmware, 16) != 0) {
    ok = false;
    const volatile uint16_t *c = (const volatile uint16_t *)&__rom_in_ram_start__;
    DPRINTF("CART REGION CORRUPTED (%s): %04X %04X %04X %04X\n", stage, c[0],
            c[1], c[2], c[3]);
  }
  return ok;
}

/* Centered status line on a black screen (used when there is nothing
 * to play -- the video path never draws text). */
static void show_status(const char *line1, const char *line2) {
  fb_chunked_clear(15);
  font_set_font(&font8x8);
  font_set_color(0);
  font_align(FONT_ALIGN_CENTER);
  font_move(FB_CHUNKED_W / 2, 88);
  font_print(line1);
  if (line2) {
    font_move(FB_CHUNKED_W / 2, 104);
    font_print(line2);
  }
  font_align(FONT_ALIGN_LEFT);
  fb_publish();
}

void emul_start() {
  /* Heal the per-app FOLDER config before anything else (and before
   * Core 1 launches, so the flash write is trivially safe). A stored
   * value of "/test" is never legitimate for this app -- it's the
   * upstream template's default leaking in from a recycled config
   * sector -- and sdcard_initFilesystem() would otherwise create that
   * junk folder on the card. Genuine custom folders are left alone. */
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  if (folder == NULL || strcmp(folder->value, "/test") == 0) {
    DPRINTF("Healing FOLDER config ('%s') -> /mjpeg\n",
            folder ? folder->value : "<missing>");
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_FOLDER, "/mjpeg");
    settings_save(aconfig_getContext(), true);
    folder = settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  }
  static char folderName[SETTINGS_MAX_VALUE_LENGTH];
  snprintf(folderName, sizeof(folderName), "%s",
           folder ? folder->value : "/mjpeg");

  /* --- boot sequence (identical to the template's emul.c) --- */
  ERASE_FIRMWARE_IN_RAM();
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  ikbd_init();
  if (init_romemul(false) < 0) {
    panic("init_romemul failed");
  }
  if (commemul_init() < 0) {
    panic("commemul_init failed");
  }
  if (fb_init(&fb_mode_320x200) < 0) {
    panic("fb_init failed");
  }
  palette_init();
  audio_init();

  FATFS fsys;
  cart_check("pre-sd");
  bool sd_ok = sdcard_initFilesystem(&fsys, folderName) == SDCARD_INIT_OK;
  if (!sd_ok) {
    DPRINTF("SD card unavailable.\n");
  }
  cart_check("post-sd");

  select_configure();

  /* --- find and open the first .AVI: app folder first, then root --- */
  bool playing = false;
  if (sd_ok) {
    static char path[300];
    static char msg[80];
    /* Configured folder first, then the app's canonical folder, then
     * the card root -- the config sector can hold a stale FOLDER (it
     * is only as trustworthy as the last app that saved it). */
    const char *dirs[3] = {folderName, "/mjpeg", "/"};
    FRESULT fr[3] = {FR_OK, FR_OK, FR_OK};
    path[0] = '\0';
    for (int i = 0; i < 3 && path[0] == '\0'; i++) {
      DIR dir;
      FILINFO fno;
      fr[i] = f_findfirst(&dir, &fno, dirs[i], "*.AVI");
      if (fr[i] == FR_OK && fno.fname[0] != '\0') {
        snprintf(path, sizeof(path), "%s/%s",
                 (dirs[i][1] == '\0') ? "" : dirs[i], fno.fname);
      }
      f_closedir(&dir);
    }
    if (path[0] != '\0') {
      /* Breadcrumb: visible on the ST while the AVI is opened/parsed,
       * so a stall inside the open path can be told apart from a
       * stall before it (screen would still show the boot pattern). */
      snprintf(msg, sizeof(msg), "OPENING %s", path);
      show_status("MD/MJPEG " RELEASE_VERSION, msg);
      playing = mjpeg_open(path) == 0;
      if (!playing) {
        snprintf(msg, sizeof(msg), "CANNOT PLAY %s", path);
        show_status("MD/MJPEG " RELEASE_VERSION, msg);
      }
    } else {
      /* Show exactly where we looked + the FatFs result codes, so a
       * misconfigured FOLDER (per-app flash config) is visible on the
       * ST screen without needing the UART console. */
      DPRINTF("No .AVI found in %s (fr=%d), /mjpeg (fr=%d) or / (fr=%d)\n",
              folderName, fr[0], fr[1], fr[2]);
      snprintf(msg, sizeof(msg), "NO .AVI IN %s (FR %d) /mjpeg (FR %d) / (FR %d)",
               folderName, fr[0], fr[1], fr[2]);
      show_status("MD/MJPEG " RELEASE_VERSION, msg);
    }
  } else {
    show_status("MD/MJPEG " RELEASE_VERSION, "NO SD CARD");
  }
  cart_check("post-open");

  /* --- main loop --- */
  DPRINTF("Entering main loop\n");
  uint32_t stall_last_shown = 0;
  uint32_t stall_check_us = time_us_32();
  while (true) {
    fb_pump_rom3(); /* ROM3 ring -> IKBD demux + VBL frame-sync */
    ikbd_pump();

    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
      /* ESC exit to GEM is handled inside ikbd.c; nothing else yet. */
      (void)k;
    }

    if (playing) {
      mjpeg_tick();
      /* Stall detector: if no frame lands for 3 s, paint the player
       * counters on the ST screen -- on-target diagnostics that work
       * in release builds where the UART console is compiled out. */
      uint32_t now = time_us_32();
      if (now - stall_check_us > 3000000u) {
        stall_check_us = now;
        uint32_t shown = mjpeg_frames_shown();
        static char dbg[64];
        mjpeg_debug_status(dbg, sizeof(dbg));
        DPRINTF("HB: %s\n", dbg); /* liveness heartbeat on the UART */
        if (shown == stall_last_shown) {
          show_status("MD/MJPEG STALLED", dbg);
        }
        stall_last_shown = shown;
      }
    }
    audio_render_frame();
    cart_check("main-loop");
  }
}
