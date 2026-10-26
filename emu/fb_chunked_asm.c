/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: fb_chunked_asm.c
 * Description: rp/src/fb_chunked_asm.S for EmuMD, in C: the two Thumb
 *              workers rp/src/fb_chunked.c calls, which a host build
 *              cannot assemble.
 */

#include <stdint.h>
#include <string.h>

/* Pixels [src, src_end) as 16-pixel blocks of four plane words, the
 * leftmost pixel in bit 15, from the low nibble of each byte. */
void fb_c2p_half(uint16_t *dst, const uint8_t *src, const uint8_t *src_end) {
  for (; src < src_end; src += 16, dst += 4) {
    uint16_t planes[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 16; i++) {
      for (unsigned k = 0; k < 4; k++) {
        planes[k] |= (uint16_t)(((src[i] >> k) & 1u) << (15u - i));
      }
    }
    memcpy(dst, planes, sizeof(planes));
  }
}

/* `count` 48-byte chunks to `dst`, in reverse chunk order from
 * `src_last` (the last chunk): dst chunk k is src chunk count-1-k. */
void fb_chunk_reverse_copy48(uint8_t *dst, const uint8_t *src_last,
                             uint32_t count) {
  for (uint32_t k = 0; k < count; k++) {
    memcpy(dst + 48u * k, src_last - 48u * k, 48u);
  }
}
