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
 * the app is mjpeg.c / mjpeg_quant.c / mjpeg_menu.c driven from the
 * main loop, a MENU <-> PLAYING state machine: the menu lists the
 * .AVI files in the app folder below ABOUT and EXIT TO GEM, and reads
 * its own keys (mjpeg_menu_key); Up/Down + Return picks a video to
 * play. ESC during a video opens the menu over it (the video carries
 * on), and ESC again closes it; from the menu on its own, with nothing
 * playing, ESC quits to GEM.
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
#include "mjpeg_menu.h"
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
  audio_start_vbl_timer(); /* Core 1 refills the audio after each m68k ack */

  FATFS fsys;
  cart_check("pre-sd");
  bool sd_ok = sdcard_initFilesystem(&fsys, folderName) == SDCARD_INIT_OK;
  if (!sd_ok) {
    DPRINTF("SD card unavailable.\n");
  }
  cart_check("post-sd");

  select_configure();

  /* The app owns the ESC key: ESC during a video opens the menu over it,
   * and quits to GEM only from the menu with nothing playing. */
  ikbd_set_esc_auto_exit(false);
  mjpeg_menu_init(sd_ok ? folderName : "/");
  cart_check("post-menu-init");

  enum { STATE_MENU, STATE_PLAYING } state = STATE_MENU;

  /* --- main loop --- */
  DPRINTF("Entering main loop (%d file(s))\n", mjpeg_menu_count());
  uint32_t hb_us = time_us_32();
  while (true) {
    fb_pump_rom3(); /* ROM3 ring -> IKBD demux + VBL frame-sync */
    ikbd_pump();

    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
      if (!k.is_press) {
        continue;
      }
      if (state == STATE_MENU || mjpeg_menu_shown()) {
        /* The menu, on its own or over the video, reads the keys. ESC
         * goes back to the video if one is playing, else quits to GEM;
         * picking a video (the playing one too) starts it afresh. */
        int sel = mjpeg_menu_key(k.scancode);
        if (sel == MJPEG_MENU_BACK) {
          if (state == STATE_PLAYING) {
            mjpeg_show_menu(false);
          } else {
            ikbd_request_boot_gem();
          }
        } else if (sel >= 0) {
          mjpeg_close();
          state = (mjpeg_open(mjpeg_menu_path(sel)) == 0) ? STATE_PLAYING
                                                          : STATE_MENU;
        }
      } else { /* STATE_PLAYING */
        switch (k.scancode) {
          case ST_SC_ESC:
            mjpeg_show_menu(true); /* the menu over the video */
            break;
          case ST_SC_LEFT:
            mjpeg_seek(-10);
            break;
          case ST_SC_RIGHT:
            mjpeg_seek(10);
            break;
          case ST_SC_SPACE:
            mjpeg_toggle_pause();
            break;
          default:
            break;
        }
      }
    }

    if (state == STATE_PLAYING) {
      if (mjpeg_is_paused()) {
        mjpeg_render_paused(); /* frozen frame + overlay, VBL-paced */
      } else if (mjpeg_tick()) {
        uint32_t now = time_us_32();
        if (now - hb_us > 3000000u) { /* UART liveness heartbeat */
          hb_us = now;
          static char dbg[64];
          mjpeg_debug_status(dbg, sizeof(dbg));
          DPRINTF("HB: %s\n", dbg);
        }
      } else {
        /* Reached the end -- stop and return to the menu, over the last
         * frame. */
        mjpeg_finish();
        state = STATE_MENU;
      }
    } else {
      mjpeg_menu_render(); /* draws + publishes, VBL-paced */
    }
    cart_check("main-loop");
  }
}
