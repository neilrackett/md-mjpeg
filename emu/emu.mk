# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make emu, included from the Makefile. It lives here rather than there so
# that changing it is an emulator-only change.

## Build MD/MJPEG for this computer and run it in Hatari with EmuMD (see
## README.md). TOS= a TOS image (default: EmuMD's EmuTOS); MACHINE=st for
## the YM path (default: ste, DMA sound); MONO=1 for a mono monitor (ST
## high res); RECORD=FILE.avi records picture and sound until you quit;
## EMUMD= another EmuMD checkout.
EMUMD ?= emu/emumd
.PHONY: emu
emu:
	$(EMUMD)/tools/mdfw run $(if $(TOS),--tos $(TOS)) $(if $(MACHINE),--machine $(MACHINE)) $(if $(RECORD),--record $(RECORD)) $(if $(MONO),-- --monitor mono)
