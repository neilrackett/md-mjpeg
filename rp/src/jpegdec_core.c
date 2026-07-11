/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: jpegdec_core.c
 * Description: Instantiates the portable C core of bitbank2/JPEGDEC
 *              (the jpegdec/ submodule) as plain C.
 *
 * JPEGDEC ships jpeg.inl (the decoder, written in C) wrapped by a C++
 * class in JPEGDEC.cpp. Compiled from a .c file, JPEGDEC.h's
 * JPEG_STATIC expands empty, so jpeg.inl's JPEG_* functions (and
 * JPEGInit, used by mjpeg.c to run the callback-driven open path)
 * get external linkage -- no C++ anywhere in the build.
 *
 * O3 on purpose: this is the hot Huffman/IDCT path; the project-wide
 * MinSizeRel (-Os) costs real milliseconds per frame here.
 */

#pragma GCC optimize("O3")

#include "JPEGDEC.h"

#include "jpeg.inl"

/* jpeg.inl keeps its parser entry (JPEGInit) hard-static; export it
 * for mjpeg.c's callback-driven open path (custom pfnRead/pfnSeek on
 * a chunk window of the AVI file, which none of the canned JPEG_open*
 * variants support). */
int mjpeg_jpeg_init(JPEGIMAGE *pJPEG) { return JPEGInit(pJPEG); }
