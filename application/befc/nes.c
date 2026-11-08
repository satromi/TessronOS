/*
 * nes.c - bus wiring, OAM DMA, controllers, and the public nes_* API.
 *
 * Holds the single global machine instance `g`. The CPU reaches every device
 * through cpu_read/cpu_write here; nes_run_frame() advances the CPU one
 * instruction at a time and catches the PPU (3x) and APU (1x) up to it.
 */
#include <string.h>
#include <stdlib.h>
#include <stddef.h>          /* offsetof */
#include "nesint.h"

struct NES g;

static int         loaded = 0;
static const char *last_err = "no ROM loaded";
static uint8_t    *rom_owned = NULL;   /* core-owned copy of the ROM image */

/* ----------------------------------------------------- controllers --- */

static u8 read_pad(int n)
{
	u8 b;
	if (g.pad_strobe) {
		b = (u8)(g.pad[n] & 1);            /* strobe high: live A button */
	} else {
		b = (u8)(g.pad_shift[n] & 1);
		g.pad_shift[n] = (u8)(g.pad_shift[n] >> 1);
		g.pad_shift[n] |= 0x80;            /* official pads read 1 after 8 shifts */
	}
	return (u8)(0x40 | b);                     /* upper bits ~ open bus */
}

void nes_set_input(int pad, uint8_t buttons)
{
	if (pad == 0 || pad == 1)
		g.pad[pad] = buttons;
}

/* ----------------------------------------------------- OAM DMA ------- */

static void oam_dma(u8 page)
{
	u16 base = (u16)(page << 8);
	int i;
#ifdef CPU_TRACE
	{ extern long dbg_oamdma; dbg_oamdma++; }
#endif
	/* OAM DMA stalls the CPU ~513-514 cycles. Burn them through cpu_cycle so the
	 * PPU advances during the transfer (a real frame's NMI handler does its DMA
	 * inside vblank and the timing matters). */
	if (g.cpu.cycles & 1) cpu_cycle();        /* extra idle cycle if odd */
	cpu_cycle();                              /* initial idle cycle */
	for (i = 0; i < 256; i++) {
		u8 d = cpu_read((u16)(base + i));     /* read cycle (ticks once) */
		g.ppu.oam[(u8)(g.ppu.oamaddr + i)] = d;
		cpu_cycle();                          /* matching write cycle */
	}
}

/* ----------------------------------------------------- CPU bus ------- */

/* cpu_cycle() is now a static-inline (nes_cycle) in nesint.h — advancing the
 * PPU(3)+APU(1) per CPU cycle WITHIN each instruction, essential for $2002
 * (vblank) polling vs NMI timing. The `#define cpu_cycle
 * nes_cycle` there keeps every call site below working, now fully inlined. */

u8 cpu_read(u16 a)
{
	u8 v;
#ifdef READ_TRACE
	extern long read_hist[0x10000];
	read_hist[a]++;
#endif
	if (a < 0x2000) {
		v = g.ram[a & 0x07FF];
	} else if (a < 0x4000) {
		v = ppu_reg_read((u16)(0x2000 | (a & 7)));
	} else if (a < 0x4020) {
		switch (a) {
		case 0x4015: v = apu_read_status(); break;
		case 0x4016: v = read_pad(0); break;
		case 0x4017: v = read_pad(1); break;
		default:     v = 0; break;        /* write-only / open bus */
		}
	} else if (a >= 0x8000) {
		v = cart_cpu_read_fast(a);            /* hot PRG-ROM fetch, inlined */
	} else {
		v = cart_cpu_read(a);                 /* PRG-RAM / open bus (cold) */
	}
	cpu_cycle();                              /* this access burns one CPU cycle */
	return v;
}

void cpu_write(u16 a, u8 v)
{
	if (a < 0x2000) {
		g.ram[a & 0x07FF] = v;
	} else if (a < 0x4000) {
		ppu_reg_write((u16)(0x2000 | (a & 7)), v);
	} else if (a < 0x4020) {
		switch (a) {
		case 0x4014:
			/* $4014 write: the write cycle itself, then the DMA copy. Account
			 * the write cycle here, then oam_dma() burns the transfer cycles. */
			cpu_cycle();
			oam_dma(v);
			return;
		case 0x4016:
			g.pad_strobe = v & 1;
			if (g.pad_strobe) {
				g.pad_shift[0] = g.pad[0];
				g.pad_shift[1] = g.pad[1];
			}
			break;
		default:                          /* $4000-$4013, $4015, $4017 */
			apu_write(a, v);
			break;
		}
	} else {
		cart_cpu_write(a, v);
	}
	cpu_cycle();                              /* this access burns one CPU cycle */
}

/* ----------------------------------------------------- public API ---- */

static long     dbg_used   = 0;   /* diagnostic: used-ROM byte count */
static unsigned dbg_romfnv = 0;   /* diagnostic: FNV32 over the used ROM bytes */

int nes_load(const uint8_t *rom, long len)
{
	const char *err = "?";

	if (rom == NULL || len < 16) {
		last_err = "ROM data missing or too small";
		return -1;
	}

	/* take a private copy so the caller may release its buffer and so cart.c
	 * can point PRG/CHR straight into a stable image */
	{
		uint8_t *copy = (uint8_t *)malloc((size_t)len);
		if (copy == NULL) {
			last_err = "out of memory copying ROM";
			return EMU_ERR_NOMEM;
		}
		memcpy(copy, rom, (size_t)len);
		if (rom_owned) free(rom_owned);
		rom_owned = copy;
	}

	{
		int er = cart_load(rom_owned, len, &err);
		if (er != 0) {
			last_err = err;
			loaded = 0;
			return er;
		}
	}

	{	/* diagnostic: FNV32 over the USED portion (16 + trainer + PRG + CHR) */
		long trainer = (rom_owned[6] & 0x04) ? 512 : 0;
		long used = 16 + trainer + (long)rom_owned[4] * 16384 + (long)rom_owned[5] * 8192;
		unsigned h = 2166136261u; long i;
		if (used > len) used = len;
		for (i = 0; i < used; i++) { h ^= rom_owned[i]; h *= 16777619u; }
		dbg_used = used; dbg_romfnv = h;
	}

	memset(g.ram, 0, sizeof(g.ram));
	g.pad[0] = g.pad[1] = 0;
	g.pad_shift[0] = g.pad_shift[1] = 0;
	g.pad_strobe = 0;

	ppu_power();
	apu_power();
	cpu_power();          /* reads reset vector from freshly mapped PRG */

	loaded = 1;
	last_err = "ok";
	return 0;
}

void nes_reset(void)
{
	if (!loaded) return;
	cpu_reset();
	ppu_reset();
	apu_reset();
}

void nes_run_frame(void)
{
	/* Safety cap: a full NTSC frame is ~29781 CPU cycles; bound the loop far
	 * above that so a (hypothetical) state where frame_ready never gets set can
	 * never hang the whole BTRON GUI. */
	long guard = 200000;
	if (!loaded) return;
	g.ppu.frame_ready = 0;
	/* cpu_step() now advances the PPU/APU itself (via cpu_cycle on every bus
	 * access), so the frame is just a bounded run of instructions. */
	while (!g.ppu.frame_ready && guard-- > 0) {
		long c0 = g.cpu.cycles;
		cpu_step();
		if (g.cart.irq_cpu_clocked)          /* FME-7 / RAMBO-1 / VRC per-cycle IRQ */
			cart_irq_clock((int)(g.cpu.cycles - c0));  /* incl. OAM-DMA stall cycles */
	}
}

const uint32_t *nes_framebuffer(void) { return g.ppu.fb; }
int             nes_loaded(void)      { return loaded; }
const char     *nes_last_error(void)  { return last_err; }

void nes_dbg_state(int *mirror, unsigned *ctrl, unsigned *v, int nt_nz[4])
{
	int t, i;
	if (mirror) *mirror = g.cart.mirror;
	if (ctrl)   *ctrl   = g.ppu.ctrl;
	if (v)      *v      = g.ppu.v;
	if (nt_nz) for (t = 0; t < 4; t++) {
		const u8 *p = g.ppu.vram + t * 0x400;
		int n = 0;
		for (i = 0; i < 0x3C0; i++) if (p[i]) n++;
		nt_nz[t] = n;
	}
}

/* diagnostic: used-ROM length + FNV32 (computed at load), and the currently
 * mapped 8KB CHR bank number + its live FNV32 -- to catch device-side ROM/CHR
 * corruption (data differs host vs device) vs a pure render divergence. */
void nes_dbg_rom(long *used_len, unsigned *rom_fnv, int *chr_bank, unsigned *chr8_fnv)
{
	if (used_len) *used_len = dbg_used;
	if (rom_fnv)  *rom_fnv  = dbg_romfnv;
	if (chr_bank) *chr_bank = (int)(g.cart.chr_off[0] / 8192);
	if (chr8_fnv) {
		unsigned h = 2166136261u;
		const u8 *p = g.cart.chr + g.cart.chr_off[0];
		int i;
		for (i = 0; i < 8192; i++) { h ^= p[i]; h *= 16777619u; }
		*chr8_fnv = h;
	}
}

/* ----------------------------------------------------- save state ---- *
 * Explicit block serialization. We copy the live structs as raw bytes but skip
 * the two large regenerated arrays (ppu.fb, apu.smp_buf — both the LAST field
 * of their struct, so offsetof() gives the prefix to save) and fix up the cart
 * ROM pointers on load (g.cart.prg/chr point into rom_owned, which differs every
 * run). Same binary saves and loads, so struct layout is stable; the header
 * stamps the ROM identity so a slot from a different ROM is rejected.            */

#define STATE_MAGIC   0x31534642u            /* 'B','F','S','1' little-endian */
#define STATE_VERSION 2u                     /* v2: CHR-RAM grown to 32KB (m30) */
#define STATE_HDRWORDS 6

#define PPU_SAVE_BYTES  ((int)offsetof(PPU, fb))
#define APU_SAVE_BYTES  ((int)offsetof(APU, smp_buf))

static int blk_put(uint8_t *dst, int *pos, int max, const void *src, int n)
{
	if (n < 0 || *pos + n > max) return -1;
	memcpy(dst + *pos, src, (size_t)n);
	*pos += n;
	return 0;
}
static int blk_get(const uint8_t *src, int *pos, int len, void *dst, int n)
{
	if (n < 0 || *pos + n > len) return -1;
	memcpy(dst, src + *pos, (size_t)n);
	*pos += n;
	return 0;
}

int nes_state_size(void)
{
	return (int)(STATE_HDRWORDS * 4)
	     + (int)sizeof(CPU)
	     + PPU_SAVE_BYTES
	     + APU_SAVE_BYTES
	     + (int)sizeof(CART)
	     + (int)sizeof(g.ram)
	     + (int)(sizeof(g.pad) + sizeof(g.pad_shift) + sizeof(g.pad_strobe));
}

int nes_save_state(uint8_t *buf, int max)
{
	int pos = 0;
	uint32_t hdr[STATE_HDRWORDS];
	if (!loaded || buf == NULL) return -1;
	hdr[0] = STATE_MAGIC;
	hdr[1] = STATE_VERSION;
	hdr[2] = (uint32_t)g.cart.mapper;
	hdr[3] = g.cart.prg_size;
	hdr[4] = g.cart.chr_size;
	hdr[5] = (uint32_t)g.cart.chr_is_ram;
	if (blk_put(buf, &pos, max, hdr, (int)sizeof(hdr))) return -1;
	if (blk_put(buf, &pos, max, &g.cpu, (int)sizeof(CPU))) return -1;
	if (blk_put(buf, &pos, max, &g.ppu, PPU_SAVE_BYTES)) return -1;
	if (blk_put(buf, &pos, max, &g.apu, APU_SAVE_BYTES)) return -1;
	if (blk_put(buf, &pos, max, &g.cart, (int)sizeof(CART))) return -1;
	if (blk_put(buf, &pos, max, g.ram, (int)sizeof(g.ram))) return -1;
	if (blk_put(buf, &pos, max, g.pad, (int)sizeof(g.pad))) return -1;
	if (blk_put(buf, &pos, max, g.pad_shift, (int)sizeof(g.pad_shift))) return -1;
	if (blk_put(buf, &pos, max, &g.pad_strobe, (int)sizeof(g.pad_strobe))) return -1;
	return pos;
}

int nes_load_state(const uint8_t *buf, int len)
{
	int pos = 0;
	uint32_t hdr[STATE_HDRWORDS];
	u8 *cur_prg, *cur_chr;
	u32 cur_prgsz, cur_chrsz;
	int cur_chrisram;
	if (!loaded || buf == NULL) return -1;
	if (blk_get(buf, &pos, len, hdr, (int)sizeof(hdr))) return -1;
	if (hdr[0] != STATE_MAGIC) return -2;                 /* not a state blob */
	if (hdr[1] != STATE_VERSION) return -4;               /* CART struct layout may differ
	                                                       * across builds -> restoring the
	                                                       * blob wholesale would land garbage
	                                                       * in prg_off/chr_off (used unmasked
	                                                       * by the fetch path). Reject. */
	if (hdr[2] != (uint32_t)g.cart.mapper ||
	    hdr[3] != g.cart.prg_size ||
	    hdr[4] != g.cart.chr_size) return -3;             /* belongs to another ROM */

	/* keep the live ROM pointers/sizes (valid for rom_owned); the blob's are stale. */
	cur_prg = g.cart.prg;       cur_chr = g.cart.chr;
	cur_prgsz = g.cart.prg_size; cur_chrsz = g.cart.chr_size;
	cur_chrisram = g.cart.chr_is_ram;

	if (blk_get(buf, &pos, len, &g.cpu, (int)sizeof(CPU))) return -1;
	if (blk_get(buf, &pos, len, &g.ppu, PPU_SAVE_BYTES)) return -1;
	if (blk_get(buf, &pos, len, &g.apu, APU_SAVE_BYTES)) return -1;
	if (blk_get(buf, &pos, len, &g.cart, (int)sizeof(CART))) return -1;
	if (blk_get(buf, &pos, len, g.ram, (int)sizeof(g.ram))) return -1;
	if (blk_get(buf, &pos, len, g.pad, (int)sizeof(g.pad))) return -1;
	if (blk_get(buf, &pos, len, g.pad_shift, (int)sizeof(g.pad_shift))) return -1;
	if (blk_get(buf, &pos, len, &g.pad_strobe, (int)sizeof(g.pad_strobe))) return -1;

	/* restore the ROM pointers the blob overwrote with stale values */
	g.cart.prg = cur_prg;
	g.cart.prg_size = cur_prgsz;
	g.cart.chr_size = cur_chrsz;
	g.cart.chr_is_ram = cur_chrisram;
	g.cart.chr = cur_chrisram ? g.cart.chr_ram : cur_chr;

	/* defense-in-depth: a corrupted (but magic/version/size-matching) blob could carry
	 * out-of-range window bases. The PPU/CPU fetch paths index prg[]/chr[] by these
	 * offsets with only the in-window low bits masked (prg 8KB &0x1FFF, chr 1KB &0x3FF),
	 * so an OOB base reads past the image and can crash the app. Re-clamp every window so
	 * base+windowsize stays within the live image (a legitimate same-build save is already
	 * in range -> this is a no-op for it). */
	{
		int i;
		for (i = 0; i < 4; i++)
			if (cur_prgsz < 8192 || g.cart.prg_off[i] > cur_prgsz - 8192)
				g.cart.prg_off[i] = 0;
		for (i = 0; i < 8; i++)
			if (cur_chrsz < 1024 || g.cart.chr_off[i] > cur_chrsz - 1024)
				g.cart.chr_off[i] = 0;
	}
	/* same rationale for the restored timing fields: render_scanline(line) writes
	 * fb + line*256 for any scanline < 240 (a negative value = wild write above the
	 * framebuffer), and apu_next_delta reads seq[seq_step-1]/seq[seq_step] (4- or
	 * 5-entry tables) unmasked. */
	if (g.ppu.scanline < 0 || g.ppu.scanline > 261) g.ppu.scanline = 261;
	if (g.ppu.dot < 0 || g.ppu.dot > 340) g.ppu.dot = 0;
	if (g.apu.seq_step < 0 || g.apu.seq_step > 4) g.apu.seq_step = 0;

	/* the audio ring (smp_buf and its w/r indices) was not saved; empty it */
	g.apu.smp_w = g.apu.smp_r = 0;
	return 0;
}

/* ----------------------------------------------------- battery SRAM -- */

int nes_sram_size(void) { return (int)sizeof(g.cart.prg_ram); }

int nes_sram_save(uint8_t *buf, int max)
{
	if (!loaded || buf == NULL || max < (int)sizeof(g.cart.prg_ram)) return -1;
	memcpy(buf, g.cart.prg_ram, sizeof(g.cart.prg_ram));
	return (int)sizeof(g.cart.prg_ram);
}

int nes_sram_load(const uint8_t *buf, int len)
{
	if (!loaded || buf == NULL || len < (int)sizeof(g.cart.prg_ram)) return -1;
	memcpy(g.cart.prg_ram, buf, sizeof(g.cart.prg_ram));
	return 0;
}

void nes_audio_enable(int on) { apu_set_audio(on); }

/* per-channel mute mask (debug) -- apu_chan_mask lives in apu.c */
void nes_audio_set_mask(int mask) { extern int apu_chan_mask; apu_chan_mask = mask & 0x1F; }
int  nes_audio_get_mask(void)     { extern int apu_chan_mask; return apu_chan_mask; }
void nes_set_sprite_limit(int on) { extern int ppu_sprite_limit; ppu_sprite_limit = on ? 1 : 0; }
int  nes_get_sprite_limit(void)   { extern int ppu_sprite_limit; return ppu_sprite_limit; }

int nes_audio_read(int16_t *out, int max)
{
	if (!loaded || out == NULL || max <= 0) return 0;
	return apu_read_samples(out, max);
}
