/**
 * File: fb.h
 * Description: Framebuffer module — owns the single 32 KB cartridge
 *              framebuffer the m68k reads each VBL.
 *
 * Single-FB design: there is exactly one framebuffer in
 * the shared region (`$FA8300`, 32 KB, 320x200x4bpp). Double-buffering
 * happens on the ST side; the RP just writes into this one buffer.
 *
 * This header is introduced alongside fb_font.* and
 * fb_draw.* so the ported font/draw modules have a stable target
 * type. fb.c defines `fb_screen` and provides
 * the init / clear / address-accessor entry points.
 */

#ifndef FB_H
#define FB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FB_MODE {
  unsigned short h_pixels;
  unsigned short v_pixels;
  unsigned char color_bits; /* bits per pixel */
};

/* Mirrors md-sprites-demo's VGA_SCREEN but with a single framebuffer
 * pointer instead of A/B/current/hidden (we don't keep a back buffer in
 * cartridge). The ported text renderer writes into
 * `framebuffer` directly. */
struct FB_SCREEN {
  unsigned int *framebuffer;
  uint16_t width;
  uint16_t height;
  uint8_t color_bits;
  uint8_t _pad;
};

extern struct FB_SCREEN fb_screen;

/* Built-in 320x200 4 bpp Atari ST low-res mode. Defined in fb.c. */
extern const struct FB_MODE fb_mode_320x200;

/**
 * @brief Populate `fb_screen` from a mode descriptor, build the pixel
 *        mask LUT used by the text/draw primitives, and zero the
 *        framebuffer.
 *
 * @return 0 on success, -1 if `mode` is NULL.
 */
int fb_init(const struct FB_MODE *mode);

/** @brief Fill the cartridge framebuffer with 0xFF (solid black in
 *         the default TOS low-res palette). */
void fb_clear(void);

/** @brief Render the static parts of the template's boot UI (centered
 *         title + ESC hint). Called once by fb_init; the dynamic parts
 *         live in fb_render_frame. */
void fb_render_static(void);

/** @brief Render the dynamic parts of the template's boot UI: erase
 *         and redraw only the frame-counter row and the marquee row.
 *         Static title is left intact so we don't churn the parts of
 *         the FB the m68k is currently reading. Safe to call from the
 *         main loop at any cadence. */
void fb_render_frame(void);

/** @brief Publish the current chunked buffer to the cart framebuffer,
 *         synchronized to the Atari's 50 Hz VBL, tear-free.
 *
 *         Three steps: (1) transpose chunked -> planar SCRATCH (RP RAM,
 *         the slow ~1 ms part) -- this overlaps the m68k's blit of the
 *         previous frame since it doesn't touch the cart FB; (2) BLOCK
 *         until the m68k acks it finished that blit (a cart-bus read at
 *         $FB8400 captured by commemul) so the cart FB is free; (3) do
 *         the fast ~120 us chunk-reversed copy scratch -> cart FB and
 *         bump FB_FRAME_COUNTER.
 *
 *         Because only the short copy in step 3 writes the cart FB, and
 *         it runs in the m68k's post-blit slack, the RP never writes
 *         the FB while the m68k reads it -- no tearing, full 50 Hz. A
 *         ~60 ms timeout (safety net for "m68k not running", e.g. at
 *         boot) keeps the RP from hanging. Drawing into
 *         `fb_chunked_buffer` (RP RAM) is unsynchronized; call this
 *         once per frame after drawing. */
void fb_publish(void);

/** @brief Drain the ROM3 commemul ring once, routing each captured
 *         sample to BOTH the IKBD demux and the VBL frame-sync
 *         detector. Call from the main loop in place of a bare
 *         commemul_poll(); fb_publish() also calls it internally while
 *         waiting for the VBL ack, so IKBD/ESC stay responsive during
 *         the wait. */
void fb_pump_rom3(void);

/** @brief True when the ST is in high res on a mono monitor (640x400,
 *         one plane, 71 Hz), from the display report the m68k sends
 *         every VBL. The cart FB is still 32000 bytes in the same
 *         chunk-reversed layout -- 80-byte rows, 400 of them -- and a
 *         frame is two VBLs long; fb_publish() draws the right picture
 *         either way. */
bool fb_st_mono(void);

/** @brief fb_publish(), choosing how each pen is drawn in ST high res:
 *         `levels` is 16 entries of 0..16 (see fb_transpose_mono),
 *         dithered; ignored in colour. fb_publish() itself draws the pens
 *         as an even grey ramp, 0 black to 15 white. */
void fb_publish_levels(const uint8_t levels[16]);

/* --- 256 colours (cart_shared.h, "256 colours") ----------------------
 * True when the ST is a TT or a Falcon showing 8 interleaved bitplanes,
 * 320x200, from the same display report as fb_st_mono(); fb_st_falcon()
 * says which palette format. Latched: the m68k decides it once, at boot.
 * The pixels of fb_chunked_buffer are then palette indices 0..255 as
 * they are, and fb_publish() sends them in two parts of 100 rows. It
 * returns once the first is posted; the second goes from fb_pump_rom3()
 * when the m68k has copied the first (about two VBLs on a real TT), so
 * the next frame can be decoded meanwhile -- see fb_publish_wait_rows. */
bool fb_st_eight(void);
bool fb_st_falcon(void);

/* Before drawing into any row of fb_chunked_buffer below `y_end`: in 256
 * colours rows 100..199 may still be the last frame's, waiting to be sent
 * (see fb_st_eight), and this sends them first, waiting for the m68k if
 * it must. Returns at once when there's nothing outstanding, which
 * outside 256 colours is always. FB_CHUNKED_H covers the whole buffer;
 * call it with that before anything but a top-down decode draws. */
void fb_publish_wait_rows(unsigned y_end);

/* The 256-colour palette, from 8-bit RGB, in effect from the next
 * fb_publish(). Entries 0..15 are not set here: they follow palette_set's
 * 16 ST colours, so that the 16-colour menu, the time overlay and the
 * boot screen draw the same in both modes. Ignored for an index below 16
 * or before fb_st_eight(). */
void fb_eight_set_colour(unsigned idx, uint8_t r, uint8_t g, uint8_t b);

/* Debug: the rows of a part the m68k copies in a VBL, as it measured at
 * boot (100 or more means a part a VBL; 0 before the first report). */
unsigned fb_eight_rows_per_vbl(void);

/** @brief Microseconds the most recent fb_publish() spent in the
 *         chunky-to-planar conversion (dual-core c2p + chunk-reversed
 *         memcpy; in 256 colours both parts' conversions, without the
 *         waits). Updated every fb_publish(); stale-by-one is fine
 *         for an on-screen timing readout. */
uint32_t fb_last_convert_us(void);

#ifdef __cplusplus
}
#endif

#endif /* FB_H */
