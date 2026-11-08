/*
 * nes.h - Public API for the portable NES (Famicom) emulator core.
 *
 * This header is platform-neutral (only <stdint.h>); the BTRON frontend
 * (emu_main.c) talks to the emulator exclusively through these functions, while
 * the core modules (cpu/ppu/apu/cart/nes) share the internal state via
 * nesint.h.
 *
 * Video output is a fixed 256x240 framebuffer of 32-bit 0x00RRGGBB pixels
 * (opaque). Audio is not produced in this revision (the APU is emulated for
 * timing/$4015 only).
 */
#ifndef FCEMU_NES_H
#define FCEMU_NES_H

#include <stdint.h>

#define NES_W 256
#define NES_H 240

/* Controller button bits passed to nes_set_input(). */
enum {
	NES_BTN_A      = 0x01,
	NES_BTN_B      = 0x02,
	NES_BTN_SELECT = 0x04,
	NES_BTN_START  = 0x08,
	NES_BTN_UP     = 0x10,
	NES_BTN_DOWN   = 0x20,
	NES_BTN_LEFT   = 0x40,
	NES_BTN_RIGHT  = 0x80
};

/*
 * Load an iNES ROM image (the full .nes file bytes, including the 16-byte
 * header). The core takes a private copy, so the caller may free its buffer
 * afterwards.
 *
 * Returns 0 on success, an EMU_ERR_ code on error (see nes_last_error()).
 * On success the machine is reset and ready for nes_run_frame().
 */
/* Why a ROM was not taken */
#define EMU_ERR_NOROM	(-1)	/* not a ROM of this console */
#define EMU_ERR_NOMEM	(-2)	/* no memory for it */
#define EMU_ERR_NOCHIP	(-3)	/* it needs hardware on its cartridge that is not emulated */

int  nes_load(const uint8_t *rom, long len);

/* Power-on / reset the currently loaded machine. */
void nes_reset(void);

/* Run exactly one video frame, updating the framebuffer. No-op if no ROM. */
void nes_run_frame(void);

/* Set the button state for pad 0 or pad 1 (bitmask of NES_BTN_*). */
void nes_set_input(int pad, uint8_t buttons);

/* The 256x240 framebuffer (row-major, valid after nes_run_frame()). */
const uint32_t *nes_framebuffer(void);

/* Enable/disable audio synthesis. OFF by default: when off the APU mixer adds
 * no per-cycle cost. Turn on once an audio output (the "ｓｏｕｎｄ" device) is
 * available to consume nes_audio_read(). */
void nes_audio_enable(int on);

/* Drain pending audio: up to `max` 48kHz / 16-bit signed mono samples into
 * `out`; returns the number written (0..max). Call after nes_run_frame(). */
int  nes_audio_read(int16_t *out, int max);

/* Debug: per-channel mute mask (bit0=pulse1,1=pulse2,2=triangle,3=noise,4=dmc;
 * 0x1F=all on). Lets the frontend isolate which channel is misbehaving. */
void nes_audio_set_mask(int mask);
int  nes_audio_get_mask(void);

/* Sprite-per-scanline limit: 1 = hardware-accurate 8/line (flicker), 0 = draw
 * all sprites on a line (no flicker). Default 1. */
void nes_set_sprite_limit(int on);
int  nes_get_sprite_limit(void);

/* ---- save state (full machine snapshot) -------------------------------- *
 * A snapshot captures the entire dynamic machine state (CPU, RAM, PPU incl.
 * VRAM/OAM/palette, APU, and all mapper/bank/SRAM state) but NOT the ROM image
 * or the regenerated framebuffer/audio ring. The blob is self-describing
 * (magic + ROM identity) so loading verifies it belongs to the running ROM.
 *
 * nes_state_size() returns the exact (constant for this build) blob size, so the
 * caller can size a fixed record layout. nes_save_state() writes up to `max`
 * bytes and returns the count (or <0). nes_load_state() returns 0 on success,
 * <0 on error (-2 = not a state blob, -3 = different ROM). */
int  nes_state_size(void);
int  nes_save_state(uint8_t *buf, int max);
int  nes_load_state(const uint8_t *buf, int len);

/* ---- battery save-data (cart PRG-RAM $6000-$7FFF) ----------------------- *
 * Independent of save states: the 8KB work/save RAM that battery-backed games
 * persist. nes_sram_size() is the byte count (8192). */
int  nes_sram_size(void);
int  nes_sram_save(uint8_t *buf, int max);
int  nes_sram_load(const uint8_t *buf, int len);

/* Non-zero once a ROM has been successfully loaded. */
int  nes_loaded(void);

/* Human-readable description of the last load error (never NULL). */
const char *nes_last_error(void);

/* Diagnostic snapshot of the live PPU/mapper state for an on-screen debug overlay.
 * Any pointer may be NULL. nt_nz[] receives the nonzero-tile count of each of the
 * four nametables (0..960). */
void nes_dbg_state(int *mirror, unsigned *ctrl, unsigned *v, int nt_nz[4]);

/* Diagnostic: used-ROM length + FNV32 (computed at load), and the currently
 * mapped 8KB CHR bank number + its live FNV32. Any pointer may be NULL. */
void nes_dbg_rom(long *used_len, unsigned *rom_fnv, int *chr_bank, unsigned *chr8_fnv);

#endif /* FCEMU_NES_H */
