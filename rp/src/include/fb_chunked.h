/**
 * File: fb_chunked.h
 * Description: Chunked-pixel framebuffer + chunky-to-planar publish path.
 *
 * App code draws into `fb_chunked_buffer` (one byte per pixel, palette
 * index in the low nibble). A single conversion pass per frame
 * (`fb_chunky_to_planar`) transposes the chunked bytes into the Atari
 * ST 4 bpp planar layout that lives in the shared cart framebuffer at
 * $FA8300, where the m68k VBL blit picks it up.
 *
 * The conversion writes 16-bit plane WORDS into cart-mirrored RP RAM,
 * so the cart-bus byte-swap is transparent at the word level (same
 * property the older direct-planar `pixel_masks_flat` writes relied
 * on). Only the low 4 bits of each chunked byte are used; the high 4
 * bits are dropped during transposition.
 */

#ifndef FB_CHUNKED_H_INCLUDED
#define FB_CHUNKED_H_INCLUDED

#include <stdint.h>

#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Chunked framebuffer dimensions. Matches the Atari ST low-res screen
 * the planar conversion targets. */
#define FB_CHUNKED_W 320
#define FB_CHUNKED_H 200
#define FB_CHUNKED_SIZE (FB_CHUNKED_W * FB_CHUNKED_H)

/* Storage lives in regular RP RAM (`RAM` region at 0x20000000), not the
 * cart-mirrored ROM_IN_RAM region. Apps poke pixels here directly. */
extern uint8_t fb_chunked_buffer[FB_CHUNKED_SIZE];

/* Launch Core 1 with the chunky-to-planar worker loop. Must be called
 * exactly once, from Core 0, before the first call to
 * fb_chunky_to_planar. fb_init() does this. A second call would
 * deadlock inside pico-sdk's launch handshake. */
void fb_chunked_init(void);

/* Run a job on Core 1, in parallel with Core 0 (dual-core).
 * `fb_core1_dispatch` hands `job(arg)` to the parked Core 1 worker and
 * returns immediately; the caller then does its own half of the work and
 * calls `fb_core1_wait()` to join. The fn+arg are passed through the
 * inter-core FIFO, whose push/pop carry the cross-core memory barriers,
 * so data prepared before dispatch is visible to the job and the job's
 * writes are visible after wait. Each dispatch MUST be paired with one
 * wait before the next dispatch (the c2p in fb_transpose uses this too,
 * so demos must join their own job before fb_publish). */
typedef void (*fb_core1_job_t)(void *arg);
void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg);
void __not_in_flash_func(fb_core1_wait)(void);

/* Fill the entire chunked buffer with a single palette index. */
void fb_chunked_clear(uint8_t color);

/* Bounds-checked single-pixel plot; mostly useful for diagnostics. */
static inline void fb_chunked_plot(unsigned int x, unsigned int y,
                                   uint8_t color) {
  if (x < FB_CHUNKED_W && y < FB_CHUNKED_H) {
    fb_chunked_buffer[y * FB_CHUNKED_W + x] = color;
  }
}

/**
 * Transpose the chunked buffer into Atari ST 4 bpp planar format.
 *
 * Output layout per 16-pixel block (matches ST low-res):
 *   word 0 = plane 0 (LSB of palette index)
 *   word 1 = plane 1
 *   word 2 = plane 2
 *   word 3 = plane 3 (MSB)
 * Within each word, bit (15 - i) corresponds to pixel `i` of the block.
 *
 * **Must be called from Core 0 only.** The implementation dispatches
 * the bottom half of the buffer to Core 1 via the inter-core FIFO
 * and blocks on Core 1's completion signal. Calling from Core 1
 * deadlocks (Core 1 would push to a FIFO it isn't reading from, then
 * block on a pop that never resolves).
 *
 * @param planar  Destination, must point to the cart framebuffer at
 *                $FA8300 (= 32 000 bytes of cart-mirrored RP RAM).
 *                Cart-bus byte-swap is invisible because we write
 *                16-bit values, not raw bytes.
 */
void __not_in_flash_func(fb_chunky_to_planar)(uint16_t *planar);

/* Split halves of the above, so the slow transpose can run before the
 * VBL wait (overlapping the m68k blit) and only the fast cart-FB write
 * runs after it. fb.c's fb_publish() uses these directly. */

/* Dual-core transpose: chunked buffer -> internal planar scratch (RP
 * RAM). Does NOT touch the cart FB; safe to run while the m68k blits. */
void __not_in_flash_func(fb_transpose)(void);

/* Chunk-reversed copy of the last transpose's scratch -> `planar` (the
 * cart FB). The ONLY cart-FB write; ~120 us. Call after fb_transpose()
 * and after the m68k has finished blitting the previous frame. */
void __not_in_flash_func(fb_planar_publish)(uint16_t *planar);

/* ST high res (a mono monitor): the chunked buffer as a 640x400 one-bit
 * picture in the same scratch, in place of fb_transpose(). Each pixel is
 * doubled both ways and its pen given a level by `levels` (16 entries,
 * 0..16: 0 is black ink, 16 white paper, and between that fraction of
 * the pixels white), dithered at the output resolution against the
 * 32x32 blue noise. It is the same 32000 bytes -- 400 rows of 40 words,
 * the leftmost pixel in bit 15, a set bit black -- so fb_planar_publish()
 * copies it out unchanged. Dual-core, like fb_transpose(). */
void fb_transpose_mono(const uint8_t levels[16]);

/* fb_transpose_mono's worker (fb_mono.c): chunked rows [y0, y1) into
 * output rows 2*y0 .. 2*y1-1 of `scratch`. */
void fb_mono_rows(uint16_t *scratch, unsigned y0, unsigned y1,
                  const uint8_t levels[16]);

/* 256 colours (cart_shared.h): rows part * 100 .. part * 100 + 99 of the
 * chunked buffer, whole bytes as palette indices, to 8 interleaved
 * planes straight into `cart_fb` (the cart FB, which the caller has
 * made sure the m68k is not reading). Each 16 pixels make 8 words,
 * plane 0 first, the leftmost pixel in bit 15, in natural row order.
 * Dual-core, like fb_transpose(). */
void fb_c2p8_part(uint16_t *cart_fb, unsigned part);

/* The planar scratch, which only the 16-colour and mono pictures use:
 * CART_FRAMEBUFFER_SIZE bytes, 4-byte aligned. Once fb_st_eight() is
 * true (it is latched) nothing here touches it again, and an app drawing
 * in 256 colours may keep its own tables there. */
uint8_t *fb_eight_scratch(void);

#ifdef __cplusplus
}
#endif

#endif /* FB_CHUNKED_H_INCLUDED */
