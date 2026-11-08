/*
 * cart.c - iNES ROM parsing and mappers.
 *
 * Supported: 0 NROM, 1 MMC1, 2 UNROM, 3 CNROM, 4 MMC3, 7 AxROM, 8 FFE-F3xxx,
 *   9 MMC2, 10 MMC4, 11 ColorDreams, 34 BNROM, 38 PCI556, 66 GxROM, 69 FME-7,
 *   70/152 Bandai, 71 Camerica, 78 Irem 74HC161/32, 87/140 Jaleco, 89 Sunsoft-2,
 *   93 Sunsoft-3R, 94 UN1ROM, 180 reversed-UxROM, 184 Sunsoft, 185 CNROM-protect,
 *   206 DxROM, 88/95/154 Namcot-118, 118 TxSROM, 33/48 Taito TC0190/TC0690,
 *   64 Tengen RAMBO-1, 75 VRC1, 21/22/23/25 Konami VRC2/VRC4, 24/26 VRC6, 85 VRC7,
 *   73 VRC3, 16/159 Bandai FCG/LZ93D50, 18 Jaleco SS88006, 32 Irem G-101,
 *   65 Irem H3001, 67 Sunsoft-3, 68 Sunsoft-4, 19 Namco 163, 5 MMC5,
 *   76 Namco-109, 80/207 Taito X1-005, 82 Taito X1-017, 210 Namco 175/340,
 *   97 Irem TAM-S1, 232 Camerica BF9096, 15 K-1029 multicart, 79/146 NINA-03/06,
 *   113 HES-NINA, 36 TXC, 30 UNROM-512.
 * Expansion audio (Phase 6): VRC6 (2 pulse+saw), Sunsoft 5B (3 square AY),
 *   Namco163 (wavetable), MMC5 (2 pulse+PCM), VRC7 (OPLL FM). MMC5
 *   vertical-split and the VRC7 KSL/LFO are deferred (documented).
 *
 * Banking model: the CPU PRG space $8000-$FFFF is four 8KB windows whose byte
 * offsets into the PRG image live in g.cart.prg_off[0..3]; the PPU pattern
 * space $0000-$1FFF is eight 1KB windows in g.cart.chr_off[0..7]. Each mapper
 * write recomputes these offsets, so the read paths stay branch-light.
 */
#include <string.h>
#include "nesint.h"

#define PRG8K  8192u
#define CHR1K  1024u

static u32 prg_banks8(void) { return g.cart.prg_size / PRG8K; }
static u32 chr_banks1(void) { return g.cart.chr_size / CHR1K; }

/* set the 8KB PRG window `slot` (0..3) to absolute 8KB bank number */
static void set_prg8(int slot, u32 bank)
{
	u32 n = prg_banks8();
	if (n == 0) n = 1;
	g.cart.prg_off[slot] = (bank % n) * PRG8K;
}
/* set a 16KB PRG bank across two windows; bank is in 16KB units */
static void set_prg16(int slot16, u32 bank16) /* slot16: 0=$8000, 1=$C000 */
{
	set_prg8(slot16 * 2,     bank16 * 2);
	set_prg8(slot16 * 2 + 1, bank16 * 2 + 1);
}
static void set_prg32(u32 bank32)
{
	set_prg8(0, bank32 * 4 + 0);
	set_prg8(1, bank32 * 4 + 1);
	set_prg8(2, bank32 * 4 + 2);
	set_prg8(3, bank32 * 4 + 3);
}
/* set the 1KB CHR window `slot` (0..7) to absolute 1KB bank number */
static void set_chr1(int slot, u32 bank)
{
	u32 n = chr_banks1();
	if (n == 0) n = 1;
	g.cart.chr_off[slot] = (bank % n) * CHR1K;
}
static void set_chr8(u32 bank8)
{
	int i;
	for (i = 0; i < 8; i++) set_chr1(i, bank8 * 8 + i);
}
static void set_chr4(int half, u32 bank4) /* half: 0=$0000, 1=$1000 */
{
	int i;
	for (i = 0; i < 4; i++) set_chr1(half * 4 + i, bank4 * 4 + i);
}

/* ------------------------------------------------------------- MMC1 ------ */

static void mmc1_apply(void)
{
	int prgmode = (g.cart.m1_ctrl >> 2) & 3;
	int chrmode = (g.cart.m1_ctrl >> 4) & 1;

	switch (g.cart.m1_ctrl & 3) {
	case 0: g.cart.mirror = MIR_SINGLE0; break;
	case 1: g.cart.mirror = MIR_SINGLE1; break;
	case 2: g.cart.mirror = MIR_VERT;    break;
	case 3: g.cart.mirror = MIR_HORZ;    break;
	}

	if (chrmode == 0) {
		set_chr8((g.cart.m1_chr0 >> 1));     /* 8KB: ignore low bit */
	} else {
		set_chr4(0, g.cart.m1_chr0);
		set_chr4(1, g.cart.m1_chr1);
	}

	/* SUROM/SXROM (PRG > 256KB): the PRG-A18 outer 256KB-block select comes from
	 * bit 4 of the CHR bank register. Below 256KB that bit is a CHR bank bit, so
	 * only fold it into the PRG bank for large carts. `outer` is in 16KB banks. */
	{
		u32 outer = (g.cart.prg_size > 256u * 1024u && (g.cart.m1_chr0 & 0x10)) ? 16 : 0;
		switch (prgmode) {
		case 0: case 1:
			set_prg32(((g.cart.m1_prg & 0x0E) >> 1) + outer / 2);  /* 32KB */
			break;
		case 2: /* fix first bank at $8000, switch $C000 */
			set_prg16(0, outer);
			set_prg16(1, (g.cart.m1_prg & 0x0F) + outer);
			break;
		case 3: /* switch $8000, fix last bank at $C000 */
			set_prg16(0, (g.cart.m1_prg & 0x0F) + outer);
			set_prg16(1, ((prg_banks8() / 2) - 1));
			break;
		}
	}
}

static void mmc1_write(u16 a, u8 v)
{
	if (v & 0x80) {                  /* reset */
		g.cart.m1_shift = 0x10;
		g.cart.m1_count = 0;
		g.cart.m1_ctrl |= 0x0C;      /* PRG mode 3 */
		mmc1_apply();
		return;
	}
	g.cart.m1_shift = (g.cart.m1_shift >> 1) | ((v & 1) << 4);
	if (++g.cart.m1_count == 5) {
		u8 val = g.cart.m1_shift & 0x1F;
		switch ((a >> 13) & 3) {
		case 0: g.cart.m1_ctrl = val; break;
		case 1: g.cart.m1_chr0 = val; break;
		case 2: g.cart.m1_chr1 = val; break;
		case 3: g.cart.m1_prg  = val; break;
		}
		g.cart.m1_shift = 0x10;
		g.cart.m1_count = 0;
		mmc1_apply();
	}
}

/* ------------------------------------------------------------- MMC3 ------ */

static void mmc3_apply(void)
{
	u32 last = prg_banks8();
	if (last == 0) last = 1;

	if (g.cart.m4_prg_mode == 0) {
		set_prg8(0, g.cart.m4_regs[6]);
		set_prg8(1, g.cart.m4_regs[7]);
		set_prg8(2, last - 2);
		set_prg8(3, last - 1);
	} else {
		set_prg8(0, last - 2);
		set_prg8(1, g.cart.m4_regs[7]);
		set_prg8(2, g.cart.m4_regs[6]);
		set_prg8(3, last - 1);
	}

	if (g.cart.m4_chr_mode == 0) {
		set_chr1(0, g.cart.m4_regs[0] & 0xFE);
		set_chr1(1, g.cart.m4_regs[0] | 0x01);
		set_chr1(2, g.cart.m4_regs[1] & 0xFE);
		set_chr1(3, g.cart.m4_regs[1] | 0x01);
		set_chr1(4, g.cart.m4_regs[2]);
		set_chr1(5, g.cart.m4_regs[3]);
		set_chr1(6, g.cart.m4_regs[4]);
		set_chr1(7, g.cart.m4_regs[5]);
	} else {
		set_chr1(0, g.cart.m4_regs[2]);
		set_chr1(1, g.cart.m4_regs[3]);
		set_chr1(2, g.cart.m4_regs[4]);
		set_chr1(3, g.cart.m4_regs[5]);
		set_chr1(4, g.cart.m4_regs[0] & 0xFE);
		set_chr1(5, g.cart.m4_regs[0] | 0x01);
		set_chr1(6, g.cart.m4_regs[1] & 0xFE);
		set_chr1(7, g.cart.m4_regs[1] | 0x01);
	}

	if (g.cart.mapper == 118) {
		/* TxSROM/TLSROM: CHR bank bit7 (CHR A17) is wired to CIRAM A10, giving
		 * per-region nametable selection; the $A000 mirror register is bypassed. */
		g.cart.nt_custom = 1;
		if (g.cart.m4_chr_mode == 0) {
			g.cart.nt_map[0] = g.cart.nt_map[1] = (g.cart.m4_regs[0] >> 7) & 1;
			g.cart.nt_map[2] = g.cart.nt_map[3] = (g.cart.m4_regs[1] >> 7) & 1;
		} else {
			g.cart.nt_map[0] = (g.cart.m4_regs[2] >> 7) & 1;
			g.cart.nt_map[1] = (g.cart.m4_regs[3] >> 7) & 1;
			g.cart.nt_map[2] = (g.cart.m4_regs[4] >> 7) & 1;
			g.cart.nt_map[3] = (g.cart.m4_regs[5] >> 7) & 1;
		}
	}
}

static void mmc3_write(u16 a, u8 v)
{
	switch (a & 0xE001) {
	case 0x8000:
		g.cart.m4_bank_select = v;
		g.cart.m4_prg_mode = (v >> 6) & 1;
		g.cart.m4_chr_mode = (v >> 7) & 1;
		mmc3_apply();
		break;
	case 0x8001:
		g.cart.m4_regs[g.cart.m4_bank_select & 7] = v;
		mmc3_apply();
		break;
	case 0xA000:
		if (g.cart.mirror != MIR_FOUR)
			g.cart.mirror = (v & 1) ? MIR_HORZ : MIR_VERT;
		break;
	case 0xA001:
		break;                       /* PRG-RAM protect: ignored */
	case 0xC000:
		g.cart.m4_irq_latch = v;
		break;
	case 0xC001:
		g.cart.m4_irq_reload = 1;
		g.cart.m4_irq_counter = 0;
		break;
	case 0xE000:
		g.cart.m4_irq_enable = 0;
		g.cart.m4_irq_reload = 0;
		g.cpu.irq_line &= ~IRQ_MAPPER;  /* acknowledge */
		break;
	case 0xE001:
		g.cart.m4_irq_enable = 1;
		break;
	}
}

/* One tick of the MMC3-style down-counter (shared by MMC3, TxSROM, TC0690 and
 * RAMBO-1): reload from latch at 0 (or on a pending reload), else decrement;
 * assert IRQ when the counter reaches 0 with IRQs enabled. */
static void mmc3_irq_tick(void)
{
	if (g.cart.m4_irq_counter == 0 || g.cart.m4_irq_reload) {
		g.cart.m4_irq_counter = g.cart.m4_irq_latch;
		g.cart.m4_irq_reload = 0;
	} else {
		g.cart.m4_irq_counter--;
	}
	if (g.cart.m4_irq_counter == 0 && g.cart.m4_irq_enable)
		g.cpu.irq_line |= IRQ_MAPPER;
}

void cart_ppu_scanline(void)
{
	switch (g.cart.mapper) {
	case 4:                          /* MMC3                                  */
	case 118:                        /* TxSROM                                */
	case 48:                         /* Taito TC0690                          */
		mmc3_irq_tick();
		break;
	case 64:                         /* RAMBO-1: scanline mode only here      */
		if (g.cart.m64_irq_mode == 0)
			mmc3_irq_tick();
		break;
	default:
		break;
	}
}

/* ----------------------------------- Namcot 118 family (206/88/95/154) --- */
/* DxROM/Namcot-108: an MMC3 banking subset with NO PRG/CHR mode bits (always
 * "mode 0"), NO scanline IRQ, and (on 206) NO mirror register. Derivatives add:
 *   88  : the four 1KB CHR banks come from the upper CHR half (bank | 0x40)
 *   154 : like 88, plus a 1-screen mirror select (bit6 of the bank-select reg)
 *   95  : per-region nametable driven by bit5 of the 2KB CHR bank registers
 * Banking reuses m4_bank_select / m4_regs[] (only R0..R7, mode forced to 0). */

static void namcot_apply(void)
{
	u32 last = prg_banks8();
	if (last == 0) last = 1;

	set_prg8(0, g.cart.m4_regs[6]);          /* $8000 switchable */
	set_prg8(1, g.cart.m4_regs[7]);          /* $A000 switchable */
	set_prg8(2, last - 2);                   /* $C000 fixed second-to-last */
	set_prg8(3, last - 1);                   /* $E000 fixed last */

	set_chr1(0, g.cart.m4_regs[0] & 0xFE);   /* R0/R1 are 2KB (even/odd pair) */
	set_chr1(1, g.cart.m4_regs[0] | 0x01);
	set_chr1(2, g.cart.m4_regs[1] & 0xFE);
	set_chr1(3, g.cart.m4_regs[1] | 0x01);
	set_chr1(4, g.cart.m4_regs[2]);          /* R2..R5 are 1KB */
	set_chr1(5, g.cart.m4_regs[3]);
	set_chr1(6, g.cart.m4_regs[4]);
	set_chr1(7, g.cart.m4_regs[5]);

	if (g.cart.mapper == 88 || g.cart.mapper == 154) {
		/* CHR A16 = PPU A12: the 2KB banks stay in the low 64KB half, the four
		 * 1KB banks are forced into the upper half (value & 0x3F | 0x40). */
		set_chr1(0, (g.cart.m4_regs[0] & 0x3E));
		set_chr1(1, (g.cart.m4_regs[0] & 0x3E) | 0x01);
		set_chr1(2, (g.cart.m4_regs[1] & 0x3E));
		set_chr1(3, (g.cart.m4_regs[1] & 0x3E) | 0x01);
		set_chr1(4, (g.cart.m4_regs[2] & 0x3F) | 0x40);
		set_chr1(5, (g.cart.m4_regs[3] & 0x3F) | 0x40);
		set_chr1(6, (g.cart.m4_regs[4] & 0x3F) | 0x40);
		set_chr1(7, (g.cart.m4_regs[5] & 0x3F) | 0x40);
	}
	if (g.cart.mapper == 95) {
		/* bit5 of the 2KB CHR regs selects the nametable for its half of the
		 * screen. R0 -> NT0/NT1, R1 -> NT2/NT3. */
		g.cart.nt_custom = 1;
		g.cart.nt_map[0] = g.cart.nt_map[1] = (g.cart.m4_regs[0] >> 5) & 1;
		g.cart.nt_map[2] = g.cart.nt_map[3] = (g.cart.m4_regs[1] >> 5) & 1;
	}
}

static void namcot_write(u16 a, u8 v)
{
	if ((a & 0x0001) == 0) {                  /* even: bank select (low 3 bits) */
		g.cart.m4_bank_select = v;
		if (g.cart.mapper == 154)             /* bit6 = 1-screen nametable select */
			g.cart.mirror = (v & 0x40) ? MIR_SINGLE1 : MIR_SINGLE0;
	} else {                                  /* odd: bank data (bit6 is bank data,*/
		g.cart.m4_regs[g.cart.m4_bank_select & 7] = v;  /* NOT a mirror control)  */
	}
	namcot_apply();
}

/* ------------------------------------ Taito TC0190 (33) / TC0690 (48) ---- */
/* $8000 PRG0(8KB@$8000)[+mirror on 33], $8001 PRG1(8KB@$A000), $8002/$8003 2KB
 * CHR @ $0000/$0800, $A000-$A003 1KB CHR @ $1000-$1C00. PRG $C000/$E000 fixed to
 * the last two 8KB banks. TC0690 (48) adds an MMC3-style A12 scanline IRQ at
 * $C000-$C003 and moves mirroring to $E000 bit6. */
static void taito_write(u16 a, u8 v)
{
	switch (a & 0xE003) {
	case 0x8000:
		set_prg8(0, v & 0x3F);
		if (g.cart.mapper == 33)         /* TC0190: bit6 1=horizontal, 0=vertical  */
			g.cart.mirror = (v & 0x40) ? MIR_HORZ : MIR_VERT;
		break;
	case 0x8001: set_prg8(1, v & 0x3F); break;
	case 0x8002: set_chr1(0, v * 2); set_chr1(1, v * 2 + 1); break;  /* 2KB @ $0000 */
	case 0x8003: set_chr1(2, v * 2); set_chr1(3, v * 2 + 1); break;  /* 2KB @ $0800 */
	case 0xA000: set_chr1(4, v); break;                              /* 1KB @ $1000 */
	case 0xA001: set_chr1(5, v); break;                              /* 1KB @ $1400 */
	case 0xA002: set_chr1(6, v); break;                              /* 1KB @ $1800 */
	case 0xA003: set_chr1(7, v); break;                              /* 1KB @ $1C00 */
	case 0xC000:                         /* 48: IRQ latch (ones-complement)         */
		g.cart.m4_irq_latch = (u8)(v ^ 0xFF);
		break;
	case 0xC001:                         /* 48: IRQ reload                          */
		g.cart.m4_irq_reload = 1;
		g.cart.m4_irq_counter = 0;
		break;
	case 0xC002:                         /* 48: IRQ enable                          */
		g.cart.m4_irq_enable = 1;
		break;
	case 0xC003:                         /* 48: IRQ disable + acknowledge           */
		g.cart.m4_irq_enable = 0;
		g.cpu.irq_line &= ~IRQ_MAPPER;
		break;
	case 0xE000:                         /* 48: mirroring (bit6 1=horizontal)       */
		if (g.cart.mapper == 48)
			g.cart.mirror = (v & 0x40) ? MIR_HORZ : MIR_VERT;
		break;
	}
}

/* --------------------------------------------- Tengen RAMBO-1 (64) ------- */
/* MMC3-like banking with extras: a 1KB-CHR mode (K), a PRG mode that swaps the
 * $8000/$C000 windows (P), a CHR A12-invert (C), a third switchable PRG bank
 * (R15), and an IRQ that runs in either A12-scanline OR CPU-cycle (/4) mode.
 * Bank-select byte ($8000, kept in m4_bank_select) = CPKx RRRR. */
static void rambo1_apply(void)
{
	u32 last = prg_banks8();
	u8 *R = g.cart.m64_regs;
	int chr_inv  = (g.cart.m4_bank_select & 0x80) != 0;
	int prg_mode = (g.cart.m4_bank_select & 0x40) != 0;
	int chr_1k   = (g.cart.m4_bank_select & 0x20) != 0;
	int lo = chr_inv ? 4 : 0;          /* $0000 half maps to these 4 1KB slots  */
	int hi = chr_inv ? 0 : 4;          /* $1000 half                            */
	if (last == 0) last = 1;

	if (!prg_mode) {                   /* $8000=R6 $A000=R7 $C000=R15 $E000=last */
		set_prg8(0, R[6]);
		set_prg8(1, R[7]);
		set_prg8(2, R[15]);
	} else {                           /* $8000=R15 $A000=R7 $C000=R6 $E000=last */
		set_prg8(0, R[15]);
		set_prg8(1, R[7]);
		set_prg8(2, R[6]);
	}
	set_prg8(3, last - 1);

	if (!chr_1k) {                     /* 2KB mode: R0,R1 are 2KB banks         */
		set_chr1(lo + 0, R[0] & 0xFE);
		set_chr1(lo + 1, R[0] | 0x01);
		set_chr1(lo + 2, R[1] & 0xFE);
		set_chr1(lo + 3, R[1] | 0x01);
	} else {                           /* 1KB mode: R0,R8,R1,R9 split the half   */
		set_chr1(lo + 0, R[0]);
		set_chr1(lo + 1, R[8]);
		set_chr1(lo + 2, R[1]);
		set_chr1(lo + 3, R[9]);
	}
	set_chr1(hi + 0, R[2]);
	set_chr1(hi + 1, R[3]);
	set_chr1(hi + 2, R[4]);
	set_chr1(hi + 3, R[5]);
}

static void rambo1_write(u16 a, u8 v)
{
	switch (a & 0xE001) {
	case 0x8000:                       /* bank select: CPKx RRRR                */
		g.cart.m4_bank_select = v;
		rambo1_apply();
		break;
	case 0x8001:                       /* bank data into register 0..15         */
		g.cart.m64_regs[g.cart.m4_bank_select & 0x0F] = v;
		rambo1_apply();
		break;
	case 0xA000:                       /* mirroring: 0=vertical, 1=horizontal   */
		g.cart.mirror = (v & 1) ? MIR_HORZ : MIR_VERT;
		break;
	case 0xA001:
		break;                         /* no PRG-RAM on RAMBO-1                  */
	case 0xC000:                       /* IRQ latch (direct, NOT inverted)      */
		g.cart.m4_irq_latch = v;
		break;
	case 0xC001:                       /* reload + bit0 selects IRQ mode        */
		g.cart.m64_irq_mode = v & 1;   /* 0 = scanline (A12), 1 = CPU-cycle     */
		g.cart.m4_irq_reload = 1;
		g.cart.m64_irq_pre = 0;
		break;
	case 0xE000:                       /* disable + acknowledge                 */
		g.cart.m4_irq_enable = 0;
		g.cpu.irq_line &= ~IRQ_MAPPER;
		break;
	case 0xE001:                       /* enable                                */
		g.cart.m4_irq_enable = 1;
		break;
	}
}

/* --------------------------------------------- Konami VRC family --------- */
/* Shared 8-bit VRC IRQ (VRC4 / VRC6 / VRC7): an up-counter clocked either every
 * CPU cycle (M=1) or via a 341/-3 prescaler that averages one tick per scanline
 * (M=0, the 114/114/113 pattern). On overflow from $FF it reloads the latch and
 * asserts IRQ; it is gated entirely by the enable bit E. */
static const u8 vrc_mir[4] = { MIR_VERT, MIR_HORZ, MIR_SINGLE0, MIR_SINGLE1 };

static void vrc_irq_count(void)
{
	if (g.cart.vrc_irq_counter == 0xFF) {
		g.cart.vrc_irq_counter = g.cart.vrc_irq_latch;
		g.cpu.irq_line |= IRQ_MAPPER;
	} else {
		g.cart.vrc_irq_counter++;
	}
}

static void vrc_irq_ctrl_write(u8 v)         /* IRQ control: .... .MEA */
{
	g.cart.vrc_irq_mode    = (v >> 2) & 1;
	g.cart.vrc_irq_enable  = (v >> 1) & 1;
	g.cart.vrc_irq_aenable = v & 1;
	g.cpu.irq_line &= ~IRQ_MAPPER;           /* writing control acknowledges  */
	g.cart.vrc_irq_pre = 341;
	if (g.cart.vrc_irq_enable)
		g.cart.vrc_irq_counter = g.cart.vrc_irq_latch;
}

static void vrc_irq_ack(void)                /* IRQ acknowledge: E <- A */
{
	g.cpu.irq_line &= ~IRQ_MAPPER;
	g.cart.vrc_irq_enable = g.cart.vrc_irq_aenable;
}

static void vrc_irq_clock(int cyc)           /* called per instruction */
{
	int i;
	if (!g.cart.vrc_irq_enable)
		return;
	for (i = 0; i < cyc; i++) {
		if (g.cart.vrc_irq_mode) {           /* cycle mode               */
			vrc_irq_count();
		} else {                             /* scanline prescaler       */
			g.cart.vrc_irq_pre -= 3;
			if (g.cart.vrc_irq_pre <= 0) {
				g.cart.vrc_irq_pre += 341;
				vrc_irq_count();
			}
		}
	}
}

/* ---- VRC1 (75): the earliest Konami VRC — 3x8KB switchable PRG (+ fixed last),
 * two 4KB CHR banks whose 5th bit comes from $9000, and H/V mirroring. No IRQ,
 * no expansion audio. vrc_prg[0..2] hold the three 4-bit PRG selects; vrc_chr[0]
 * /[1] hold the assembled 5-bit 4KB CHR banks. */
static void vrc1_apply(void)
{
	u32 last = prg_banks8();
	if (last == 0) last = 1;
	set_prg8(0, g.cart.vrc_prg[0]);
	set_prg8(1, g.cart.vrc_prg[1]);
	set_prg8(2, g.cart.vrc_prg[2]);
	set_prg8(3, last - 1);                 /* $E000 fixed to last 8KB */
	set_chr4(0, g.cart.vrc_chr[0]);
	set_chr4(1, g.cart.vrc_chr[1]);
}

static void vrc1_write(u16 a, u8 v)
{
	switch (a & 0xF000) {
	case 0x8000: g.cart.vrc_prg[0] = v & 0x0F; break;   /* PRG0 @ $8000 (4 bits) */
	case 0xA000: g.cart.vrc_prg[1] = v & 0x0F; break;   /* PRG1 @ $A000          */
	case 0xC000: g.cart.vrc_prg[2] = v & 0x0F; break;   /* PRG2 @ $C000          */
	case 0x9000:                                        /* mirroring + CHR A4 hi */
		g.cart.mirror = (v & 1) ? MIR_HORZ : MIR_VERT;
		g.cart.vrc_chr[0] = (u16)((g.cart.vrc_chr[0] & 0x0F) | ((v & 0x02) << 3)); /* bit1->A4 */
		g.cart.vrc_chr[1] = (u16)((g.cart.vrc_chr[1] & 0x0F) | ((v & 0x04) << 2)); /* bit2->A4 */
		break;
	case 0xE000: g.cart.vrc_chr[0] = (u16)((g.cart.vrc_chr[0] & 0x10) | (v & 0x0F)); break; /* CHR0 lo */
	case 0xF000: g.cart.vrc_chr[1] = (u16)((g.cart.vrc_chr[1] & 0x10) | (v & 0x0F)); break; /* CHR1 lo */
	default: break;
	}
	vrc1_apply();
}

/* ---- VRC2 / VRC4 (21,22,23,25): PRG/CHR banking + per-board addr scramble.
 * vrc_a0/vrc_a1 select which CPU address bits act as the chip's register-select
 * lines (OR of all submapper variants for a given iNES number). Mapper 22 is
 * VRC2a (no IRQ/swap-mode, and CHR banks drop their low bit). */
static void vrc24_apply(void)
{
	u32 last = prg_banks8();
	int i;
	if (last == 0) last = 1;
	if (g.cart.vrc_prg_mode) {               /* M=1: $8000 fixed, $C000 swap */
		set_prg8(0, last - 2);
		set_prg8(1, g.cart.vrc_prg[1]);
		set_prg8(2, g.cart.vrc_prg[0]);
	} else {                                 /* M=0: $8000/$A000 switchable  */
		set_prg8(0, g.cart.vrc_prg[0]);
		set_prg8(1, g.cart.vrc_prg[1]);
		set_prg8(2, last - 2);
	}
	set_prg8(3, last - 1);
	for (i = 0; i < 8; i++) {
		u32 b = g.cart.vrc_chr[i];
		if (g.cart.mapper == 22) b >>= 1;    /* VRC2a drops CHR A0 */
		set_chr1(i, b);
	}
}

static void vrc24_write(u16 a, u8 v)
{
	int group = (a >> 12) & 0xF;
	int sel = ((a & g.cart.vrc_a0) ? 1 : 0) | ((a & g.cart.vrc_a1) ? 2 : 0);
	int is_vrc4 = (g.cart.mapper != 22);
	switch (group) {
	case 0x8:
		g.cart.vrc_prg[0] = v & 0x1F;
		break;
	case 0x9:
		if (is_vrc4) {
			if (sel < 2) g.cart.mirror = vrc_mir[v & 3];
			else         g.cart.vrc_prg_mode = (v >> 1) & 1;
		} else {
			g.cart.mirror = (v & 1) ? MIR_HORZ : MIR_VERT;
		}
		break;
	case 0xA:
		g.cart.vrc_prg[1] = v & 0x1F;
		break;
	case 0xB: case 0xC: case 0xD: case 0xE: {
		int idx = (group - 0xB) * 2 + (sel >> 1);   /* which of 8 CHR banks */
		if (sel & 1)                                 /* odd: high 5 bits     */
			g.cart.vrc_chr[idx] = (u16)((g.cart.vrc_chr[idx] & 0x00F) | ((v & 0x1F) << 4));
		else                                         /* even: low 4 bits     */
			g.cart.vrc_chr[idx] = (u16)((g.cart.vrc_chr[idx] & 0x1F0) | (v & 0x0F));
		break;
	}
	case 0xF:
		if (!is_vrc4) break;
		switch (sel) {
		case 0: g.cart.vrc_irq_latch = (u8)((g.cart.vrc_irq_latch & 0xF0) | (v & 0x0F)); break;
		case 1: g.cart.vrc_irq_latch = (u8)((g.cart.vrc_irq_latch & 0x0F) | ((v & 0x0F) << 4)); break;
		case 2: vrc_irq_ctrl_write(v); break;
		case 3: vrc_irq_ack(); break;
		}
		break;
	}
	vrc24_apply();
}

/* ---- VRC6 (24,26): 16KB+8KB PRG, 1KB x8 CHR, VRC IRQ. Audio = Phase 6. */
static void vrc6_apply(void)
{
	u32 last = prg_banks8();
	int mode = g.cart.vrc6_ctrl & 3;
	int i;
	if (last == 0) last = 1;
	set_prg8(0, g.cart.vrc_prg[0] * 2);      /* 16KB switch @ $8000-$BFFF */
	set_prg8(1, g.cart.vrc_prg[0] * 2 + 1);
	set_prg8(2, g.cart.vrc_prg[1]);          /* 8KB switch @ $C000-$DFFF  */
	set_prg8(3, last - 1);                    /* fixed last @ $E000        */
	for (i = 0; i < 8; i++) {
		u32 b;
		if (mode == 1) {                      /* 2KB banking via R0-R3 */
			b = g.cart.vrc_chr[i >> 1];
			if (g.cart.vrc6_ctrl & 0x20)      /* bit5: pass PPU A10 through */
				b = (b & 0xFE) | (i & 1);
		} else {                              /* modes 0/2/3: 1KB pattern banks  */
			b = g.cart.vrc_chr[i];            /* (modes 2/3 ROM-nametable banking */
		}                                     /*  unused by any commercial game)  */
		set_chr1(i, b);
	}
	if (mode == 0)
		g.cart.mirror = vrc_mir[(g.cart.vrc6_ctrl >> 2) & 3];
}

static void vrc6_write(u16 a, u8 v)
{
	u16 lo = a & 3;
	u16 reg;
	if (g.cart.mapper == 26)                  /* VRC6b: swap CPU A0<->A1 */
		lo = (u16)(((lo & 1) << 1) | ((lo >> 1) & 1));
	reg = (u16)((a & 0xF000) | lo);
	switch (reg) {
	case 0x8000: case 0x8001: case 0x8002: case 0x8003:
		g.cart.vrc_prg[0] = v & 0x0F; break;  /* 16KB bank (4 bits)        */
	case 0xC000: case 0xC001: case 0xC002: case 0xC003:
		g.cart.vrc_prg[1] = v & 0x1F; break;  /* 8KB bank (5 bits)         */
	case 0xB003: g.cart.vrc6_ctrl = v; break;
	case 0xD000: g.cart.vrc_chr[0] = v; break;
	case 0xD001: g.cart.vrc_chr[1] = v; break;
	case 0xD002: g.cart.vrc_chr[2] = v; break;
	case 0xD003: g.cart.vrc_chr[3] = v; break;
	case 0xE000: g.cart.vrc_chr[4] = v; break;
	case 0xE001: g.cart.vrc_chr[5] = v; break;
	case 0xE002: g.cart.vrc_chr[6] = v; break;
	case 0xE003: g.cart.vrc_chr[7] = v; break;
	case 0xF000: g.cart.vrc_irq_latch = v; break;
	case 0xF001: vrc_irq_ctrl_write(v); break;
	case 0xF002: vrc_irq_ack(); break;
	default: break;                           /* $9/$A/$B0-2 = audio (Ph6) */
	}
	vrc6_apply();
}

/* ---- VRC7 (85): 3x8KB PRG + 1KB x8 CHR + VRC IRQ. FM audio = Phase 6.
 * Register select on the group ($8/$9/.../$F) plus one low line; accept both
 * A4 ($x010, VRC7a) and A3 ($x008, VRC7b) so either board works. */
static void vrc7_apply(void)
{
	u32 last = prg_banks8();
	int i;
	if (last == 0) last = 1;
	set_prg8(0, g.cart.vrc_prg[0]);
	set_prg8(1, g.cart.vrc_prg[1]);
	set_prg8(2, g.cart.vrc_prg[2]);
	set_prg8(3, last - 1);
	for (i = 0; i < 8; i++)
		set_chr1(i, g.cart.vrc_chr[i]);
}

static void vrc7_write(u16 a, u8 v)
{
	int g0 = a & 0xF000;
	int hi = (a & 0x18) != 0;                 /* A4 (VRC7a) or A3 (VRC7b) */
	switch (g0) {
	case 0x8000: g.cart.vrc_prg[hi ? 1 : 0] = v & 0x3F; break;
	case 0x9000: if (!hi) g.cart.vrc_prg[2] = v & 0x3F; break;  /* $9010=FM */
	case 0xA000: g.cart.vrc_chr[hi ? 1 : 0] = v; break;
	case 0xB000: g.cart.vrc_chr[hi ? 3 : 2] = v; break;
	case 0xC000: g.cart.vrc_chr[hi ? 5 : 4] = v; break;
	case 0xD000: g.cart.vrc_chr[hi ? 7 : 6] = v; break;
	case 0xE000:
		if (!hi) g.cart.mirror = vrc_mir[v & 3];   /* $E000 mirror/WRAM   */
		else     g.cart.vrc_irq_latch = v;          /* $E010 IRQ latch     */
		break;
	case 0xF000:
		if (!hi) vrc_irq_ctrl_write(v);             /* $F000 IRQ control   */
		else     vrc_irq_ack();                     /* $F010 IRQ ack       */
		break;
	}
	vrc7_apply();
}

/* ---- VRC3 (73, Salamander): 16-bit IRQ (no prescaler), 16KB PRG switch,
 * CHR-RAM, fixed (header) mirroring. */
static void vrc3_apply(void)
{
	u32 last16 = prg_banks8() / 2;
	if (last16 == 0) last16 = 1;
	set_prg16(0, g.cart.vrc_prg[0]);
	set_prg16(1, last16 - 1);                 /* fixed last 16KB @ $C000 */
}

static void vrc3_write(u16 a, u8 v)
{
	switch (a & 0xF000) {
	case 0x8000: g.cart.vrc3_latch = (u16)((g.cart.vrc3_latch & 0xFFF0) | (v & 0x0F)); break;
	case 0x9000: g.cart.vrc3_latch = (u16)((g.cart.vrc3_latch & 0xFF0F) | ((v & 0x0F) << 4)); break;
	case 0xA000: g.cart.vrc3_latch = (u16)((g.cart.vrc3_latch & 0xF0FF) | ((v & 0x0F) << 8)); break;
	case 0xB000: g.cart.vrc3_latch = (u16)((g.cart.vrc3_latch & 0x0FFF) | ((v & 0x0F) << 12)); break;
	case 0xC000:                              /* IRQ control: .... .MEA (M:1=8bit)*/
		g.cart.vrc_irq_mode    = (v >> 2) & 1;
		g.cart.vrc_irq_enable  = (v >> 1) & 1;
		g.cart.vrc_irq_aenable = v & 1;
		g.cpu.irq_line &= ~IRQ_MAPPER;
		if (g.cart.vrc_irq_enable)
			g.cart.vrc3_counter = g.cart.vrc3_latch;
		break;
	case 0xD000:                              /* IRQ acknowledge: E <- A */
		g.cpu.irq_line &= ~IRQ_MAPPER;
		g.cart.vrc_irq_enable = g.cart.vrc_irq_aenable;
		break;
	case 0xF000:                              /* PRG: 16KB bank @ $8000 */
		g.cart.vrc_prg[0] = v & 0x0F;
		vrc3_apply();
		break;
	}
}

static void vrc3_irq_clock(int cyc)          /* always per CPU cycle, no prescaler */
{
	int i;
	if (!g.cart.vrc_irq_enable)
		return;
	for (i = 0; i < cyc; i++) {
		if (g.cart.vrc_irq_mode) {           /* 8-bit mode (low byte) */
			if ((g.cart.vrc3_counter & 0xFF) == 0xFF) {
				g.cart.vrc3_counter = (u16)((g.cart.vrc3_counter & 0xFF00) |
				                            (g.cart.vrc3_latch & 0x00FF));
				g.cpu.irq_line |= IRQ_MAPPER;
			} else {
				g.cart.vrc3_counter = (u16)((g.cart.vrc3_counter & 0xFF00) |
				                            ((g.cart.vrc3_counter + 1) & 0xFF));
			}
		} else {                             /* 16-bit mode */
			if (g.cart.vrc3_counter == 0xFFFF) {
				g.cart.vrc3_counter = g.cart.vrc3_latch;
				g.cpu.irq_line |= IRQ_MAPPER;
			} else {
				g.cart.vrc3_counter++;
			}
		}
	}
}

/* ====================== Phase 4: Bandai/Jaleco/Irem/Sunsoft/Namco ======== */

/* ---- Bandai FCG-1/2 & LZ93D50 (16, 159): registers decode on the low nibble
 * of $6000-$7FFF (FCG) and $8000-$FFFF (LZ93D50). 16-bit down-counter IRQ,
 * latched (LZ93D50). EEPROM at reg $D is not emulated. */
static void bandai16_write(u16 a, u8 v)
{
	switch (a & 0x000F) {
	case 0x0: case 0x1: case 0x2: case 0x3:
	case 0x4: case 0x5: case 0x6: case 0x7:
		set_chr1(a & 7, v); break;                 /* 8 x 1KB CHR */
	case 0x8: set_prg16(0, v & 0x0F); break;       /* 16KB @ $8000; $C000 fixed */
	case 0x9:
		switch (v & 3) {
		case 0: g.cart.mirror = MIR_VERT;    break;
		case 1: g.cart.mirror = MIR_HORZ;    break;
		case 2: g.cart.mirror = MIR_SINGLE0; break;
		case 3: g.cart.mirror = MIR_SINGLE1; break;
		}
		break;
	case 0xA:                                       /* IRQ control */
		g.cart.p4_irq_enable = v & 1;
		g.cpu.irq_line &= ~IRQ_MAPPER;              /* ack */
		g.cart.p4_irq_counter = g.cart.p4_irq_latch;/* LZ93D50: load latch */
		break;
	case 0xB: g.cart.p4_irq_latch = (u16)((g.cart.p4_irq_latch & 0xFF00) | v); break;
	case 0xC: g.cart.p4_irq_latch = (u16)((g.cart.p4_irq_latch & 0x00FF) | (v << 8)); break;
	default:  break;                                /* $D EEPROM, $E/$F unused */
	}
}

/* ---- Irem G-101 (32): two 8KB PRG regs + a swap-mode bit, 1KB CHR, no IRQ. */
static void irem32_apply(void)
{
	u32 last = prg_banks8();
	if (last == 0) last = 1;
	if (g.cart.p4_layout & 1) {           /* P set: $8000 reg controls $C000   */
		set_prg8(0, last - 2);
		set_prg8(2, g.cart.p4_prg[0]);
	} else {                              /* P clear: $8000 reg controls $8000  */
		set_prg8(0, g.cart.p4_prg[0]);
		set_prg8(2, last - 2);
	}
	set_prg8(1, g.cart.p4_prg[1]);        /* $A000 always reg1 */
	set_prg8(3, last - 1);                /* $E000 always last */
}

static void irem32_write(u16 a, u8 v)
{
	switch (a & 0xF000) {
	case 0x8000: g.cart.p4_prg[0] = v & 0x1F; irem32_apply(); break;
	case 0x9000:
		g.cart.mirror   = (v & 1) ? MIR_HORZ : MIR_VERT;
		g.cart.p4_layout = (v >> 1) & 1;
		irem32_apply();
		break;
	case 0xA000: g.cart.p4_prg[1] = v & 0x1F; irem32_apply(); break;
	case 0xB000: set_chr1(a & 7, v); break;        /* $B000-$B007 1KB CHR */
	}
}

/* ---- Irem H3001 (65): like G-101 but bit7 layout + a 16-bit down-counter IRQ
 * that stops at 0. CHR 1KB. */
static void irem65_apply(void)
{
	u32 last = prg_banks8();
	if (last == 0) last = 1;
	if (g.cart.p4_layout & 0x80) {        /* $8000 reg -> $C000 window */
		set_prg8(0, last - 2);
		set_prg8(2, g.cart.p4_prg[0]);
	} else {
		set_prg8(0, g.cart.p4_prg[0]);
		set_prg8(2, last - 2);
	}
	set_prg8(1, g.cart.p4_prg[1]);
	set_prg8(3, last - 1);
}

static void irem65_write(u16 a, u8 v)
{
	switch (a & 0xF007) {
	case 0x8000: g.cart.p4_prg[0] = v; irem65_apply(); break;
	case 0x9000: g.cart.p4_layout = v; irem65_apply(); break;   /* bit7 layout */
	case 0x9001:                                                /* mirror b7-6 */
		switch ((v >> 6) & 3) {
		case 0: g.cart.mirror = MIR_VERT;    break;
		case 2: g.cart.mirror = MIR_HORZ;    break;
		default: g.cart.mirror = MIR_SINGLE0; break;            /* 1,3 -> 1scA */
		}
		break;
	case 0x9003: g.cart.p4_irq_enable = (v >> 7) & 1; g.cpu.irq_line &= ~IRQ_MAPPER; break;
	case 0x9004: g.cart.p4_irq_counter = g.cart.p4_irq_latch; g.cpu.irq_line &= ~IRQ_MAPPER; break;
	case 0x9005: g.cart.p4_irq_latch = (u16)((g.cart.p4_irq_latch & 0x00FF) | (v << 8)); break;
	case 0x9006: g.cart.p4_irq_latch = (u16)((g.cart.p4_irq_latch & 0xFF00) | v); break;
	case 0xA000: g.cart.p4_prg[1] = v; irem65_apply(); break;
	case 0xB000: case 0xB001: case 0xB002: case 0xB003:
	case 0xB004: case 0xB005: case 0xB006: case 0xB007:
		set_chr1(a & 7, v); break;
	}
}

/* ---- Jaleco SS88006 (18): all banks written as low/high nibble pairs.
 * 16-bit down-counter IRQ with a selectable effective width (4/8/12/16). */
static void jaleco18_apply_prg(void)
{
	u32 last = prg_banks8();
	if (last == 0) last = 1;
	set_prg8(0, g.cart.p4_prg[0]);
	set_prg8(1, g.cart.p4_prg[1]);
	set_prg8(2, g.cart.p4_prg[2]);
	set_prg8(3, last - 1);
}

static void jaleco18_write(u16 a, u8 v)
{
	int sub = a & 3;
	v &= 0x0F;
	switch (a & 0xF000) {
	case 0x8000:
		{
			int idx = sub >> 1;                    /* $8000/1=prg0, $8002/3=prg1 */
			if (sub & 1) g.cart.p4_prg[idx] = (u8)((g.cart.p4_prg[idx] & 0x0F) | (v << 4));
			else         g.cart.p4_prg[idx] = (u8)((g.cart.p4_prg[idx] & 0xF0) | v);
			jaleco18_apply_prg();
		}
		break;
	case 0x9000:
		if (sub == 0)      g.cart.p4_prg[2] = (u8)((g.cart.p4_prg[2] & 0xF0) | v);
		else if (sub == 1) g.cart.p4_prg[2] = (u8)((g.cart.p4_prg[2] & 0x0F) | (v << 4));
		jaleco18_apply_prg();                      /* sub==2: PRG-RAM ctrl (ignored) */
		break;
	case 0xA000: case 0xB000: case 0xC000: case 0xD000:
		{
			int slot = (((a >> 12) & 0xF) - 0xA) * 2 + ((a >> 1) & 1);   /* 0..7 */
			if (a & 1) g.cart.p4_chr[slot] = (u8)((g.cart.p4_chr[slot] & 0x0F) | (v << 4));
			else       g.cart.p4_chr[slot] = (u8)((g.cart.p4_chr[slot] & 0xF0) | v);
			set_chr1(slot, g.cart.p4_chr[slot]);
		}
		break;
	case 0xE000:                                   /* IRQ reload latch nibbles */
		g.cart.p4_irq_latch = (u16)((g.cart.p4_irq_latch & ~(0x000Fu << (sub * 4)))
		                            | ((u16)v << (sub * 4)));
		break;
	case 0xF000:
		if (sub == 0) {                            /* reload + ack */
			g.cart.p4_irq_counter = g.cart.p4_irq_latch;
			g.cpu.irq_line &= ~IRQ_MAPPER;
		} else if (sub == 1) {                     /* width/enable + ack */
			g.cart.p4_irq_width = v & 0x0F;
			g.cpu.irq_line &= ~IRQ_MAPPER;
		} else if (sub == 2) {                     /* mirroring (0=H,1=V) */
			switch (v & 3) {
			case 0: g.cart.mirror = MIR_HORZ;    break;
			case 1: g.cart.mirror = MIR_VERT;    break;
			case 2: g.cart.mirror = MIR_SINGLE0; break;
			case 3: g.cart.mirror = MIR_SINGLE1; break;
			}
		}
		break;
	}
}

/* ---- Sunsoft-3 (67): 2KB CHR banks at odd $x800 addresses, 16-bit down IRQ
 * (write-twice counter, hi then lo; ack via $8000), pauses on fire. */
static void sunsoft3_write(u16 a, u8 v)
{
	switch (a & 0xF800) {
	case 0x8000: g.cpu.irq_line &= ~IRQ_MAPPER; break;             /* IRQ ack */
	case 0x8800: set_chr1(0, v * 2); set_chr1(1, v * 2 + 1); break;
	case 0x9800: set_chr1(2, v * 2); set_chr1(3, v * 2 + 1); break;
	case 0xA800: set_chr1(4, v * 2); set_chr1(5, v * 2 + 1); break;
	case 0xB800: set_chr1(6, v * 2); set_chr1(7, v * 2 + 1); break;
	case 0xC800:                                                   /* counter hi/lo */
		if (g.cart.p4_irq_toggle)
			g.cart.p4_irq_counter = (u16)((g.cart.p4_irq_counter & 0x00FF) | (v << 8));
		else
			g.cart.p4_irq_counter = (u16)((g.cart.p4_irq_counter & 0xFF00) | v);
		g.cart.p4_irq_toggle ^= 1;
		break;
	case 0xD800:                                                   /* bit4 enable */
		g.cart.p4_irq_enable = (v >> 4) & 1;
		g.cart.p4_irq_toggle = 1;                                 /* next write = hi */
		break;
	case 0xE800:
		switch (v & 3) {
		case 0: g.cart.mirror = MIR_VERT;    break;
		case 1: g.cart.mirror = MIR_HORZ;    break;
		case 2: g.cart.mirror = MIR_SINGLE0; break;
		case 3: g.cart.mirror = MIR_SINGLE1; break;
		}
		break;
	case 0xF800: set_prg16(0, v & 0x0F); break;                   /* 16KB @ $8000 */
	}
}

/* ---- Sunsoft-4 (68): 2KB CHR banks + CHR-ROM nametables. No IRQ. */
static void sunsoft4_nt_apply(void)
{
	int arr     = g.cart.p4_misc & 3;
	int use_rom = (g.cart.p4_misc >> 2) & 1;
	int high[4], i;
	switch (arr) {
	case 0:  high[0]=0; high[1]=1; high[2]=0; high[3]=1; break;   /* V: L H L H */
	case 1:  high[0]=0; high[1]=0; high[2]=1; high[3]=1; break;   /* H: L L H H */
	case 2:  high[0]=high[1]=high[2]=high[3]=0; break;            /* 1scA */
	default: high[0]=high[1]=high[2]=high[3]=1; break;            /* 1scB */
	}
	g.cart.nt_rom = 1;            /* keep the engine on so nt_page[]/nt_bank[] are
	                              * honored for BOTH the CIRAM and CHR-ROM cases  */
	for (i = 0; i < 4; i++) {
		if (use_rom) {
			g.cart.nt_is_rom[i] = 1;
			g.cart.nt_bank[i]   = high[i] ? g.cart.p4_nt[1] : g.cart.p4_nt[0];
		} else {
			g.cart.nt_is_rom[i] = 0;
			g.cart.nt_page[i]   = (u8)high[i];   /* CIRAM page per arrangement */
		}
	}
}

static void sunsoft4_write(u16 a, u8 v)
{
	switch (a & 0xF000) {
	case 0x8000: set_chr1(0, v * 2); set_chr1(1, v * 2 + 1); break;
	case 0x9000: set_chr1(2, v * 2); set_chr1(3, v * 2 + 1); break;
	case 0xA000: set_chr1(4, v * 2); set_chr1(5, v * 2 + 1); break;
	case 0xB000: set_chr1(6, v * 2); set_chr1(7, v * 2 + 1); break;
	case 0xC000: g.cart.p4_nt[0] = (u8)((v & 0x7F) | 0x80); sunsoft4_nt_apply(); break;
	case 0xD000: g.cart.p4_nt[1] = (u8)((v & 0x7F) | 0x80); sunsoft4_nt_apply(); break;
	case 0xE000:                                  /* arrangement + CIRAM/ROM sel */
		g.cart.p4_misc = (u8)((v & 3) | (((v >> 4) & 1) << 2));
		sunsoft4_nt_apply();
		break;
	case 0xF000: set_prg16(0, v & 0x0F); break;   /* 16KB @ $8000; $C000 fixed */
	}
}

/* ---- Namco 163 (19): 8x1KB CHR, programmable nametables (CHR-ROM or CIRAM),
 * 3x8KB PRG, 15-bit up-counter IRQ at $5000/$5800. Expansion audio = Phase 6.
 * (CIRAM-as-pattern, value >=$E0 on a CHR window, is treated as ROM here.) */
static void n163_nt_apply(void)
{
	int i;
	g.cart.nt_rom = 1;
	for (i = 0; i < 4; i++) {
		u8 V = g.cart.p4_nt[i];
		if (V >= 0xE0) {                          /* CIRAM */
			g.cart.nt_is_rom[i] = 0;
			g.cart.nt_page[i]   = V & 1;
		} else {                                  /* CHR-ROM as nametable */
			g.cart.nt_is_rom[i] = 1;
			g.cart.nt_bank[i]   = V;
		}
	}
}

static void n163_write(u16 a, u8 v)
{
	if (a >= 0x4800 && a < 0x5000) {              /* sound RAM data port */
		n163_audio_data_write(v);
		return;
	}
	if (a >= 0x5000 && a < 0x5800) {              /* IRQ counter low 8 bits + ack */
		g.cart.p4_irq_counter = (u16)((g.cart.p4_irq_counter & 0x7F00) | v);
		g.cpu.irq_line &= ~IRQ_MAPPER;
		return;
	}
	if (a >= 0x5800 && a < 0x6000) {              /* IRQ high 7 bits + enable + ack */
		g.cart.p4_irq_counter = (u16)((g.cart.p4_irq_counter & 0x00FF) | ((v & 0x7F) << 8));
		g.cart.p4_irq_enable = (v >> 7) & 1;
		g.cpu.irq_line &= ~IRQ_MAPPER;
		return;
	}
	if (a < 0x8000)
		return;
	if (a < 0xC000) {                             /* $8000-$B800: 8 CHR windows */
		set_chr1((a - 0x8000) >> 11, v);
		return;
	}
	if (a < 0xE000) {                             /* $C000-$D800: 4 NT regs */
		g.cart.p4_nt[(a - 0xC000) >> 11] = v;
		n163_nt_apply();
		return;
	}
	if (a < 0xE800)      { g.cart.p4_prg[0] = v & 0x3F; set_prg8(0, g.cart.p4_prg[0]); }
	else if (a < 0xF000) { g.cart.p4_prg[1] = v & 0x3F; g.cart.p4_misc = v & 0xC0;
	                       set_prg8(1, g.cart.p4_prg[1]); }
	else if (a < 0xF800) { g.cart.p4_prg[2] = v & 0x3F; set_prg8(2, g.cart.p4_prg[2]); }
	else                 { n163_audio_addr(v); }   /* $F800 = sound RAM address */
}

/* ============================ MMC5 (mapper 5) ============================ *
 * The most flexible NES mapper. Implemented: PRG modes 0-3 (ROM banking; PRG
 * RAM at $6000 only), CHR modes 0-3 with the dual sprite/BG bank sets used in
 * 8x16 sprite mode, $5105 nametable mapping (CIRAM/ExRAM/fill), fill mode,
 * extended-attribute mode, the 8x8 hardware multiplier, and the scanline IRQ.
 * Deferred: vertical split ($5200-$5202, ~2 games), PRG-RAM banked into
 * $8000-$DFFF (rare), expansion audio (Phase 6). */

static void mmc5_audio_write(u16 a, u8 v);   /* defined in the Phase 6 audio section */

static u32 m5_chrsz(void) { u32 n = g.cart.chr_size; return n ? n : 1024; }

static void mmc5_prg_apply(void)
{
	u8 *R = g.cart.m5_prg_reg;             /* $5114,$5115,$5116,$5117 (ROM only) */
	switch (g.cart.m5_prg_mode & 3) {
	case 0:  set_prg32((R[3] & 0x7F) >> 2); break;                    /* 32K */
	case 1:  set_prg16(0, (R[1] & 0x7F) >> 1);
	         set_prg16(1, (R[3] & 0x7F) >> 1); break;                 /* 2x16K */
	case 2:  set_prg16(0, (R[1] & 0x7F) >> 1);
	         set_prg8(2, R[2] & 0x7F); set_prg8(3, R[3] & 0x7F); break;/* 16K+8K+8K */
	default: set_prg8(0, R[0] & 0x7F); set_prg8(1, R[1] & 0x7F);
	         set_prg8(2, R[2] & 0x7F); set_prg8(3, R[3] & 0x7F); break;/* 4x8K */
	}
}

/* Resolve CHR set A ($5120-$5127, sprite/8x8) into 8 x 1KB byte offsets. */
static void mmc5_set_a(u32 *out)
{
	int mode = g.cart.m5_chr_mode & 3, i;
	u16 up = (u16)((g.cart.m5_chr_upper & 3) << 8);
	u32 sz = m5_chrsz();
	for (i = 0; i < 8; i++) {
		u32 bank, page, off;
		switch (mode) {
		case 0:  bank = g.cart.m5_chr_a[7] | up; page = 0x2000; off = (u32)i * 0x400; break;
		case 1:  bank = g.cart.m5_chr_a[i < 4 ? 3 : 7] | up; page = 0x1000; off = (u32)(i & 3) * 0x400; break;
		case 2:  bank = g.cart.m5_chr_a[(i >> 1) * 2 + 1] | up; page = 0x800; off = (u32)(i & 1) * 0x400; break;
		default: bank = g.cart.m5_chr_a[i] | up; page = 0x400; off = 0; break;
		}
		out[i] = (bank * page + off) % sz;
	}
}

/* Resolve CHR set B ($5128-$512B, background), mirrored into both halves. */
static void mmc5_set_b(u32 *out)
{
	int mode = g.cart.m5_chr_mode & 3, i;
	u16 up = (u16)((g.cart.m5_chr_upper & 3) << 8);
	u32 sz = m5_chrsz();
	for (i = 0; i < 8; i++) {
		u32 bank, page, off;
		switch (mode) {
		case 0:  bank = g.cart.m5_chr_b[3] | up; page = 0x2000; off = (u32)i * 0x400; break;
		case 1:  bank = g.cart.m5_chr_b[3] | up; page = 0x1000; off = (u32)(i & 3) * 0x400; break;
		case 2:  bank = g.cart.m5_chr_b[(i & 2) ? 3 : 1] | up; page = 0x800; off = (u32)(i & 1) * 0x400; break;
		default: bank = g.cart.m5_chr_b[i & 3] | up; page = 0x400; off = 0; break;
		}
		out[i] = (bank * page + off) % sz;
	}
}

/* Recompute CHR offsets for the current scanline: sprites (and 8x8 BG) use set
 * A in g.cart.chr_off; BG uses set B in m5_chr_off_bg when 8x16 sprites + on. */
void mmc5_chr_resolve(void)
{
	int size8x16 = (g.ppu.ctrl & 0x20) != 0;
	int render_on = (g.ppu.mask & 0x18) != 0;
	mmc5_set_a(g.cart.chr_off);
	if (size8x16 && render_on) mmc5_set_b(g.cart.m5_chr_off_bg);
	else                       mmc5_set_a(g.cart.m5_chr_off_bg);
}

static void mmc5_fill_apply(void)
{
	int i;
	for (i = 0; i < 0x3C0; i++) g.cart.m5_fillbuf[i] = g.cart.m5_fill_tile;
	for (; i < 0x400; i++)      g.cart.m5_fillbuf[i] = (u8)(g.cart.m5_fill_attr * 0x55);
}

/* Drive the scanline IRQ; called once per visible scanline while rendering. */
void mmc5_scanline_irq(void)
{
	if (!g.cart.m5_in_frame) {
		g.cart.m5_in_frame = 1;
		g.cart.m5_irq_pending = 0;
		g.cart.m5_scanline = 0;
	} else {
		g.cart.m5_scanline++;
	}
	if (g.cart.m5_irq_latch != 0 && g.cart.m5_scanline == g.cart.m5_irq_latch)
		g.cart.m5_irq_pending = 1;
	if (g.cart.m5_irq_enable && g.cart.m5_irq_pending) g.cpu.irq_line |= IRQ_MAPPER;
	else                                               g.cpu.irq_line &= ~IRQ_MAPPER;
}

void mmc5_end_frame(void)            /* called at vblank: leave the active region */
{
	g.cart.m5_in_frame = 0;
}

static void mmc5_irq_update(void)
{
	if (g.cart.m5_irq_enable && g.cart.m5_irq_pending) g.cpu.irq_line |= IRQ_MAPPER;
	else                                               g.cpu.irq_line &= ~IRQ_MAPPER;
}

static void mmc5_write(u16 a, u8 v)
{
	if (a <= 0x5015) { mmc5_audio_write(a, v); return; }   /* $5000-$5015 audio */
	switch (a) {
	case 0x5100: g.cart.m5_prg_mode = v & 3; mmc5_prg_apply(); break;
	case 0x5101: g.cart.m5_chr_mode = v & 3; break;
	case 0x5102: case 0x5103: break;             /* PRG-RAM write protect (not enforced) */
	case 0x5104: g.cart.m5_exram_mode = v & 3; break;
	case 0x5105: g.cart.m5_nt_map = v; break;
	case 0x5106: g.cart.m5_fill_tile = v; mmc5_fill_apply(); break;
	case 0x5107: g.cart.m5_fill_attr = v & 3; mmc5_fill_apply(); break;
	case 0x5113: break;                          /* PRG-RAM bank @ $6000 (8K only) */
	case 0x5114: case 0x5115: case 0x5116:
		g.cart.m5_prg_reg[a - 0x5114] = v; mmc5_prg_apply(); break;
	case 0x5117: g.cart.m5_prg_reg[3] = (u8)(v | 0x80); mmc5_prg_apply(); break;
	case 0x5120: case 0x5121: case 0x5122: case 0x5123:
	case 0x5124: case 0x5125: case 0x5126: case 0x5127:
		g.cart.m5_chr_a[a - 0x5120] = v; break;
	case 0x5128: case 0x5129: case 0x512A: case 0x512B:
		g.cart.m5_chr_b[a - 0x5128] = v; break;
	case 0x5130: g.cart.m5_chr_upper = v & 3; break;
	case 0x5203: g.cart.m5_irq_latch = v; break;
	case 0x5204: g.cart.m5_irq_enable = (v >> 7) & 1; mmc5_irq_update(); break;
	case 0x5205: g.cart.m5_mul_a = v; break;
	case 0x5206: g.cart.m5_mul_b = v; break;
	default:
		if (a >= 0x5C00 && a <= 0x5FFF) {        /* ExRAM */
			if (g.cart.m5_exram_mode == 2 ||
			    (g.cart.m5_exram_mode <= 1 && g.cart.m5_in_frame))
				g.cart.m5_exram[a - 0x5C00] = v; /* mode3 read-only; 0/1 during render */
		}
		break;
	}
}

static u8 mmc5_read(u16 a)
{
	if (a == 0x5204) {
		u8 r = (u8)((g.cart.m5_irq_pending << 7) | (g.cart.m5_in_frame << 6));
		g.cart.m5_irq_pending = 0;
		mmc5_irq_update();
		return r;
	}
	if (a == 0x5205) return (u8)((g.cart.m5_mul_a * g.cart.m5_mul_b) & 0xFF);
	if (a == 0x5206) return (u8)(((g.cart.m5_mul_a * g.cart.m5_mul_b) >> 8) & 0xFF);
	if (a >= 0x5C00 && a <= 0x5FFF) {
		if (g.cart.m5_exram_mode >= 2) return g.cart.m5_exram[a - 0x5C00];
		return 0;                                /* modes 0/1: open bus */
	}
	return 0;
}

/* PPU nametable write under MMC5: route to CIRAM, ExRAM, or drop (fill). */
void mmc5_nt_write(u16 a, u8 v)
{
	int sel = (g.cart.m5_nt_map >> (((a >> 10) & 3) * 2)) & 3;
	switch (sel) {
	case 0: g.ppu.vram[0x000 + (a & 0x3FF)] = v; break;
	case 1: g.ppu.vram[0x400 + (a & 0x3FF)] = v; break;
	case 2: g.cart.m5_exram[a & 0x3FF] = v; break;
	default: break;                              /* fill: not writable */
	}
}

/* ====================== Phase 6: expansion audio ======================== *
 * Each chip advances its phase/timer state once per CPU cycle via
 * cart_audio_tick(); cart_audio_out() returns the current summed output scaled
 * to blend with the native APU mix (s = (p1+p2)*226 + ...). Only active when
 * g.cart.has_exp_audio is set (VRC6/Sunsoft5B/Namco163/MMC5/VRC7). */

/* ---- VRC6 audio (24/26): 2 pulse + sawtooth, linear DAC. ---- */
static void vrc6_audio_write(u16 a, u8 v)
{
	u16 base = a & 0xF000;
	int off = a & 3;
	if (g.cart.mapper == 26) off = ((off & 1) << 1) | ((off >> 1) & 1); /* VRC6b swap */
	if (base == 0x9000 && off == 3) {              /* $9003 frequency control */
		g.cart.v6_halt  = v & 1;
		g.cart.v6_shift = (v & 4) ? 8 : (v & 2) ? 4 : 0;
		return;
	}
	if (base == 0x9000 || base == 0xA000) {        /* pulse 1 / pulse 2 */
		int i = (base == 0xA000);
		if (off == 0) { g.cart.v6_vol[i] = v & 0x0F; g.cart.v6_duty[i] = (v >> 4) & 7; g.cart.v6_mode[i] = (v >> 7) & 1; }
		else if (off == 1) g.cart.v6_period[i] = (u16)((g.cart.v6_period[i] & 0xF00) | v);
		else if (off == 2) { g.cart.v6_period[i] = (u16)((g.cart.v6_period[i] & 0x0FF) | ((v & 0x0F) << 8));
		                     g.cart.v6_en[i] = (v >> 7) & 1; if (!g.cart.v6_en[i]) g.cart.v6_step[i] = 15; }
	} else if (base == 0xB000) {                   /* sawtooth */
		if (off == 0) g.cart.v6_saw_rate = v & 0x3F;
		else if (off == 1) g.cart.v6_period[2] = (u16)((g.cart.v6_period[2] & 0xF00) | v);
		else if (off == 2) { g.cart.v6_period[2] = (u16)((g.cart.v6_period[2] & 0x0FF) | ((v & 0x0F) << 8));
		                     g.cart.v6_en[2] = (v >> 7) & 1; }
	}
}

static void vrc6_audio_tick(void)
{
	int i;
	if (g.cart.v6_halt) return;
	for (i = 0; i < 2; i++) {                       /* pulses */
		if (!g.cart.v6_en[i]) continue;
		if (g.cart.v6_div[i] == 0) {
			g.cart.v6_div[i] = (u16)(g.cart.v6_period[i] >> g.cart.v6_shift);
			g.cart.v6_step[i] = (u8)(g.cart.v6_step[i] == 0 ? 15 : g.cart.v6_step[i] - 1);
		} else g.cart.v6_div[i]--;
	}
	if (g.cart.v6_en[2]) {                           /* sawtooth */
		if (g.cart.v6_div[2] == 0) {
			g.cart.v6_div[2] = (u16)(g.cart.v6_period[2] >> g.cart.v6_shift);
			g.cart.v6_saw_rt ^= 1;
			if (g.cart.v6_saw_rt) {                  /* reacts every 2 clocks */
				if (g.cart.v6_saw_phase == 6) { g.cart.v6_saw_acc = 0; g.cart.v6_saw_phase = 0; }
				else { g.cart.v6_saw_acc = (u8)(g.cart.v6_saw_acc + g.cart.v6_saw_rate);
				       g.cart.v6_saw_phase++; }
			}
		} else g.cart.v6_div[2]--;
	}
}

static int vrc6_audio_out(void)
{
	int s = 0, i;
	for (i = 0; i < 2; i++) {
		if (!g.cart.v6_en[i]) continue;
		if (g.cart.v6_mode[i] || g.cart.v6_step[i] <= g.cart.v6_duty[i]) s += g.cart.v6_vol[i];
	}
	if (g.cart.v6_en[2]) s += g.cart.v6_saw_acc >> 3;   /* 0..31 */
	return s * 150;                                  /* ~per-unit ≈ a 2A03 pulse step (tunable) */
}

/* ---- Sunsoft 5B audio (69): 3 square channels, logarithmic DAC. ---- */
static const int ay_vol_tab[16] = {     /* AY/YM2149 log DAC, +1.5dB/env-step, scaled */
	0, 27, 38, 53, 75, 106, 150, 212, 300, 424, 598, 845, 1194, 1687, 2383, 3366
};

static void ay_audio_write(u16 a, u8 v)
{
	if (a >= 0xC000 && a < 0xE000) { g.cart.ay_sel = v & 0x0F; return; }
	if (a >= 0xE000) {
		switch (g.cart.ay_sel) {
		case 0: g.cart.ay_period[0] = (u16)((g.cart.ay_period[0] & 0xF00) | v); break;
		case 1: g.cart.ay_period[0] = (u16)((g.cart.ay_period[0] & 0x0FF) | ((v & 0x0F) << 8)); break;
		case 2: g.cart.ay_period[1] = (u16)((g.cart.ay_period[1] & 0xF00) | v); break;
		case 3: g.cart.ay_period[1] = (u16)((g.cart.ay_period[1] & 0x0FF) | ((v & 0x0F) << 8)); break;
		case 4: g.cart.ay_period[2] = (u16)((g.cart.ay_period[2] & 0xF00) | v); break;
		case 5: g.cart.ay_period[2] = (u16)((g.cart.ay_period[2] & 0x0FF) | ((v & 0x0F) << 8)); break;
		case 7: g.cart.ay_mixer = v; break;
		case 8:  g.cart.ay_vol[0] = v & 0x0F; break;
		case 9:  g.cart.ay_vol[1] = v & 0x0F; break;
		case 10: g.cart.ay_vol[2] = v & 0x0F; break;
		default: break;                              /* noise/envelope: ignored */
		}
	}
}

static void ay_audio_tick(void)
{
	int i;
	for (i = 0; i < 3; i++) {
		if (g.cart.ay_div[i] == 0) {
			u16 p = g.cart.ay_period[i]; if (p == 0) p = 1;
			g.cart.ay_div[i] = (u16)(16 * p);        /* toggle every 16*P cycles */
			g.cart.ay_high[i] ^= 1;
		} else g.cart.ay_div[i]--;
	}
}

static int ay_audio_out(void)
{
	int s = 0, i;
	for (i = 0; i < 3; i++) {
		int tone_on = ((g.cart.ay_mixer >> i) & 1) == 0;   /* active-low */
		if (!tone_on || g.cart.ay_high[i]) s += ay_vol_tab[g.cart.ay_vol[i]];
	}
	return s;                                        /* 5B is "very loud"; table pre-scaled */
}

/* ---- Namco 163 audio (19): up to 8 time-multiplexed wavetable channels. ---- */
static int n163_num_ch(void) { return ((g.cart.n163_ram[0x7F] >> 4) & 7) + 1; }

void n163_audio_addr(u8 v) { g.cart.n163_addr = v & 0x7F; g.cart.n163_autoinc = (v >> 7) & 1; }

void n163_audio_data_write(u8 v)
{
	g.cart.n163_ram[g.cart.n163_addr] = v;
	if (g.cart.n163_autoinc && g.cart.n163_addr < 0x7F) g.cart.n163_addr++;
}

u8 n163_audio_data_read(void)
{
	u8 v = g.cart.n163_ram[g.cart.n163_addr];
	if (g.cart.n163_autoinc && g.cart.n163_addr < 0x7F) g.cart.n163_addr++;
	return v;
}

static void n163_audio_tick(void)
{
	u8 *R = g.cart.n163_ram;
	int N, i, base, length, wave, vol, idx, samp;
	u32 freq, phase, mod;
	if (++g.cart.n163_cyc < 15) return;            /* one channel per 15 CPU cycles */
	g.cart.n163_cyc = 0;
	N = n163_num_ch();
	i = g.cart.n163_slot % N;
	base = 0x40 + (8 - i - 1) * 8;                 /* ch8=$78, ch7=$70, ... */
	freq   = R[base+0] | (R[base+2] << 8) | ((u32)(R[base+4] & 3) << 16);
	phase  = R[base+1] | (R[base+3] << 8) | ((u32)R[base+5] << 16);
	length = 256 - (R[base+4] & 0xFC);
	wave   = R[base+6];
	vol    = R[base+7] & 0x0F;
	mod    = (u32)length << 16;
	if (mod) phase = (phase + freq) % mod;
	R[base+1] = (u8)(phase & 0xFF);
	R[base+3] = (u8)((phase >> 8) & 0xFF);
	R[base+5] = (u8)((phase >> 16) & 0xFF);
	idx  = (int)(((phase >> 16) + wave) & 0xFF);
	samp = (R[(idx >> 1) & 0x7F] >> ((idx & 1) * 4)) & 0x0F;
	g.cart.n163_out[i] = (samp - 8) * vol;         /* -120..+105 */
	g.cart.n163_slot = (u8)((g.cart.n163_slot + 1) % N);
}

static int n163_audio_out(void)
{
	int N = n163_num_ch(), i;
	long acc = 0;
	for (i = 0; i < N; i++) acc += g.cart.n163_out[i];
	return (int)(acc / N) * 80;                    /* loud chip; tunable */
}

/* ---- MMC5 audio (5): 2 APU-style pulses (no sweep, no t<8 mute) + 8-bit PCM. */
static const u8 m5a_duty_seq[4][8] = {
	{0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1}
};
static const u8 m5a_len_tab[32] = {
	10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
	12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};

static void mmc5_audio_write(u16 a, u8 v)
{
	int i = (a >> 2) & 1;
	switch (a) {
	case 0x5000: case 0x5004:
		g.cart.m5a_duty[i] = (v >> 6) & 3; g.cart.m5a_halt[i] = (v >> 5) & 1;
		g.cart.m5a_cv[i] = (v >> 4) & 1; g.cart.m5a_vol[i] = v & 0x0F; break;
	case 0x5002: case 0x5006:
		g.cart.m5a_period[i] = (u16)((g.cart.m5a_period[i] & 0x700) | v); break;
	case 0x5003: case 0x5007:
		g.cart.m5a_period[i] = (u16)((g.cart.m5a_period[i] & 0x0FF) | ((v & 7) << 8));
		g.cart.m5a_len[i] = m5a_len_tab[(v >> 3) & 0x1F];
		g.cart.m5a_seq[i] = 0; break;
	case 0x5010: g.cart.m5a_pcm_read = v & 1; break;
	case 0x5011: if (!g.cart.m5a_pcm_read && v != 0) g.cart.m5a_pcm = v; break;
	default: break;                                /* $5001/$5005 (sweep) ignored */
	}
}

static void mmc5_audio_tick(void)
{
	int i;
	g.cart.m5a_half ^= 1;
	if (g.cart.m5a_half) {                          /* pulse timers clock at CPU/2 */
		for (i = 0; i < 2; i++) {
			if (g.cart.m5a_timer[i] == 0) {
				g.cart.m5a_timer[i] = g.cart.m5a_period[i];
				g.cart.m5a_seq[i] = (u8)((g.cart.m5a_seq[i] + 1) & 7);
			} else g.cart.m5a_timer[i]--;
		}
	}
	if (++g.cart.m5a_240 >= 7457) {                 /* ~240Hz length clock */
		g.cart.m5a_240 = 0;
		for (i = 0; i < 2; i++)
			if (!g.cart.m5a_halt[i] && g.cart.m5a_len[i] > 0) g.cart.m5a_len[i]--;
	}
}

static int mmc5_audio_out(void)
{
	int s = 0, i;
	for (i = 0; i < 2; i++) {
		if (g.cart.m5a_len[i] == 0) continue;
		if (m5a_duty_seq[g.cart.m5a_duty[i]][g.cart.m5a_seq[i]])
			s += g.cart.m5a_vol[i];                 /* constant-volume (0..15) */
	}
	s *= 226;                                       /* pulses == APU pulse loudness */
	s += (g.cart.m5a_pcm - 128) * 100;              /* 8-bit PCM ~ APU DAC level */
	return s;
}

/* ---- VRC7 audio = Yamaha YM2413 (OPLL) FM synth, 6 channels x 2 operators.
 * Tables and formulas of the chip. Implemented: phase gen,
 * full ADSR envelope (with key-scale-rate), 2-operator FM + feedback, the 15
 * built-in instrument patches + custom, TL/volume, half-sine waveform. Omitted
 * (documented): KSL key-scale-level and the AM/PM LFO (tremolo/vibrato). */
#include "opll_tables.h"        /* opll_exp_tab[256], opll_logsin_q[256] */

static const u8  opll_ml[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
static const u8  opll_egstep[4][8] = {
	{0,1,0,1,0,1,0,1},{0,1,0,1,1,1,0,1},{0,1,1,1,0,1,1,1},{0,1,1,1,1,1,1,1}
};
static const u8  opll_inst[15][8] = {
	{0x03,0x21,0x05,0x06,0xE8,0x81,0x42,0x27},{0x13,0x41,0x14,0x0D,0xD8,0xF6,0x23,0x12},
	{0x11,0x11,0x08,0x08,0xFA,0xB2,0x20,0x12},{0x31,0x61,0x0C,0x07,0xA8,0x64,0x61,0x27},
	{0x32,0x21,0x1E,0x06,0xE1,0x76,0x01,0x28},{0x02,0x01,0x06,0x00,0xA3,0xE2,0xF4,0xF4},
	{0x21,0x61,0x1D,0x07,0x82,0x81,0x11,0x07},{0x23,0x21,0x22,0x17,0xA2,0x72,0x01,0x17},
	{0x35,0x11,0x25,0x00,0x40,0x73,0x72,0x01},{0xB5,0x01,0x0F,0x0F,0xA8,0xA5,0x51,0x02},
	{0x17,0xC1,0x24,0x07,0xF8,0xF8,0x22,0x12},{0x71,0x23,0x11,0x06,0x65,0x74,0x18,0x16},
	{0x01,0x02,0xD3,0x05,0xC9,0x95,0x03,0x02},{0x61,0x63,0x0C,0x00,0x94,0xC0,0x33,0xF6},
	{0x21,0x72,0x0D,0x00,0xC1,0xD5,0x56,0x06}
};
static u16 opll_full[1024], opll_half[1024];
static int opll_ready = 0;

static void opll_init_tables(void)
{
	int x;
	for (x = 0; x < 256; x++) opll_full[x] = opll_logsin_q[x];
	for (x = 0; x < 256; x++) opll_full[256+x] = opll_logsin_q[255-x];
	for (x = 0; x < 512; x++) opll_full[512+x] = (u16)(0x8000 | opll_full[x]);
	for (x = 0; x < 512; x++) opll_half[x] = opll_full[x];
	for (x = 512; x < 1024; x++) opll_half[x] = 0xFFF;
	opll_ready = 1;
}

static int opll_exp(int i)               /* log-domain -> linear, -4084..4084 */
{
	int t = opll_exp_tab[(i & 0xFF) ^ 0xFF] + 1024;
	int res = t >> ((i & 0x7F00) >> 8);
	return ((i & 0x8000) ? ~res : res) << 1;
}

static u8 opll_pb(int ch, int idx)       /* patch byte (custom regs or ROM) */
{
	int inst = (g.cart.v7_reg[0x30 + ch] >> 4) & 0x0F;
	return inst ? opll_inst[inst-1][idx] : g.cart.v7_reg[idx];
}

void opll_write(u8 reg, u8 data)
{
	if (reg >= 0x40) return;
	if (reg >= 0x19 && reg <= 0x1F) reg -= 9;     /* register mirrors */
	else if (reg >= 0x29 && reg <= 0x2F) reg -= 9;
	else if (reg >= 0x39 && reg <= 0x3F) reg -= 9;
	g.cart.v7_reg[reg] = data;
}

static void opll_sample(void)
{
	int ch, s;
	if (!opll_ready) opll_init_tables();
	g.cart.v7_egcnt++;

	for (ch = 0; ch < 6; ch++) {                   /* key on/off edges */
		int key = (g.cart.v7_reg[0x20+ch] >> 4) & 1;
		if (key && !g.cart.v7_keyprev[ch]) {
			g.cart.v7_egstate[ch*2] = 1; g.cart.v7_egstate[ch*2+1] = 1;  /* DAMP */
		} else if (!key && g.cart.v7_keyprev[ch]) {
			if (g.cart.v7_egstate[ch*2+1]) g.cart.v7_egstate[ch*2+1] = 5;/* carrier RELEASE; modulator holds */
		}
		g.cart.v7_keyprev[ch] = (u8)key;
	}

	for (s = 0; s < 12; s++) {                      /* envelope + phase per slot */
		int ch2 = s >> 1, isMod = !(s & 1);
		u8  r2 = g.cart.v7_reg[0x20 + ch2];
		int fnum = g.cart.v7_reg[0x10 + ch2] | ((r2 & 1) << 8);
		int block = (r2 >> 1) & 7, sustain = (r2 >> 5) & 1, keyon = (r2 >> 4) & 1;
		u8  b0  = opll_pb(ch2, isMod ? 0 : 1);
		u8  b45 = opll_pb(ch2, isMod ? 4 : 5);
		u8  b67 = opll_pb(ch2, isMod ? 6 : 7);
		int KSR = (b0 >> 4) & 1, ML = b0 & 15, EGt = (b0 >> 5) & 1;
		int AR = b45 >> 4, DR = b45 & 15, SL = b67 >> 4, RR = b67 & 15;
		int st = g.cart.v7_egstate[s];
		int rks = KSR ? ((block << 1) | (fnum >> 8)) : (block >> 1);
		int prate, rate_h, rate_l, shift; u32 mask;
		int eg = g.cart.v7_egout[s];
		u32 pm, incr;

		switch (st) {
		case 2: prate = AR; break;                  /* ATTACK */
		case 3: prate = DR; break;                  /* DECAY */
		case 4: prate = EGt ? 0 : RR; break;        /* SUSTAIN */
		case 5: prate = sustain ? 5 : (EGt ? RR : 7); break; /* RELEASE */
		case 1: prate = 12; break;                  /* DAMP */
		default: prate = 0; break;                  /* IDLE */
		}
		if (isMod && !keyon) prate = 0;             /* modulator frozen while key off */
		if (prate == 0) { rate_h = 0; rate_l = 0; }
		else { rate_h = prate + (rks >> 2); if (rate_h > 15) rate_h = 15; rate_l = rks & 3; }
		shift = (st == 2) ? ((rate_h > 0 && rate_h < 12) ? 13 - rate_h : 0)
		                  : ((rate_h < 13) ? 13 - rate_h : 0);
		mask = (1u << shift) - 1;

		if (st == 2) {                              /* attack: exponential toward 0 */
			if (eg > 0 && rate_h > 0 && (g.cart.v7_egcnt & mask & ~3u) == 0) {
				int step, c = (int)g.cart.v7_egcnt, l = rate_l;
				if (rate_h == 12) step = 4 - opll_egstep[l][(c&0xC)>>1];
				else if (rate_h == 13) step = 3 - opll_egstep[l][(c&0xC)>>1];
				else if (rate_h == 14) step = 2 - opll_egstep[l][(c&0xC)>>1];
				else if (rate_h == 15 || rate_h == 0) step = 0;
				else step = opll_egstep[l][(c>>shift)&7] ? 4 : 0;
				if (step > 0) { eg = eg - (eg >> step) - 1; if (eg < 0) eg = 0; }
			}
			if (eg == 0) g.cart.v7_egstate[s] = 3; /* -> DECAY */
		} else if (st != 0) {                       /* decay/sustain/release/damp: toward mute */
			if (rate_h > 0 && (g.cart.v7_egcnt & mask) == 0) {
				int step, c = (int)g.cart.v7_egcnt, l = rate_l;
				if (rate_h == 0) step = 0;
				else if (rate_h == 13) step = opll_egstep[l][((c&0xC)>>1)|(c&1)];
				else if (rate_h == 14) step = opll_egstep[l][(c&0xC)>>1] + 1;
				else if (rate_h == 15) step = 2;
				else step = opll_egstep[l][(c>>shift)&7];
				eg += step; if (eg > 127) eg = 127;
			}
			if (st == 1 && eg >= 123) {            /* DAMP done -> start attack */
				int arh = AR + (rks >> 2); if (arh > 15) arh = 15;
				g.cart.v7_pgphase[s] = 0;          /* reset phase on key-on */
				if (arh == 15) { g.cart.v7_egstate[s] = 3; eg = 0; }
				else g.cart.v7_egstate[s] = 2;
			} else if (st == 3 && (eg >> 3) == SL) {
				g.cart.v7_egstate[s] = 4;          /* DECAY -> SUSTAIN */
			}
		}
		g.cart.v7_egout[s] = eg;

		/* phase generator */
		pm = 0;                                     /* PM/vibrato LFO omitted */
		incr = ((((u32)(fnum & 0x1FF) * 2 + pm) * opll_ml[ML]) << block) >> 2;
		g.cart.v7_pgphase[s] = (g.cart.v7_pgphase[s] + incr) & ((1u << 19) - 1);
		g.cart.v7_pgout[s] = (u16)((g.cart.v7_pgphase[s] >> 9) & 1023);
	}

	for (ch = 0; ch < 6; ch++) {                    /* operator outputs per channel */
		int ms = ch*2, cs = ch*2+1;
		u8  b3 = opll_pb(ch, 3);
		int FB = b3 & 7, modWS = (b3 >> 3) & 1, carWS = (b3 >> 4) & 1;
		int modTL = opll_pb(ch, 2) & 63;
		int carVol = (g.cart.v7_reg[0x30 + ch] & 15) << 2;
		int fm, h, att, mout, cout;
		const u16 *mw = modWS ? opll_half : opll_full;
		const u16 *cw = carWS ? opll_half : opll_full;

		/* modulator (operator 0) with self-feedback */
		fm = FB ? (g.cart.v7_modhist[ch][0] + g.cart.v7_modhist[ch][1]) >> (9 - FB) : 0;
		if (g.cart.v7_egout[ms] > 123) mout = 0;
		else {
			att = g.cart.v7_egout[ms] + (modTL << 1); if (att > 127) att = 127;
			h = mw[(g.cart.v7_pgout[ms] + fm) & 1023];
			mout = opll_exp(h + (att << 4));
		}
		g.cart.v7_modhist[ch][1] = g.cart.v7_modhist[ch][0];
		g.cart.v7_modhist[ch][0] = mout;

		/* carrier (operator 1), phase-modulated by the modulator output */
		if (g.cart.v7_egout[cs] > 123) cout = 0;
		else {
			att = g.cart.v7_egout[cs] + (carVol << 1); if (att > 127) att = 127;
			h = cw[(g.cart.v7_pgout[cs] + 2 * (mout >> 1)) & 1023];
			cout = opll_exp(h + (att << 4));
		}
		g.cart.v7_chout[ch] = -(cout) >> 1;
	}
}

static void vrc7_audio_tick(void)
{
	if (++g.cart.v7_clk >= 36) {                    /* 1789773/49716 = 36 CPU cycles */
		g.cart.v7_clk = 0;
		opll_sample();
	}
}

static int vrc7_audio_out(void)
{
	int s = 0, ch;
	for (ch = 0; ch < 6; ch++) s += g.cart.v7_chout[ch];
	return s * 2;                                   /* sum of 6 channels (tunable) */
}

void cart_audio_tick(void)
{
	switch (g.cart.mapper) {
	case 24: case 26: vrc6_audio_tick(); break;
	case 69:          ay_audio_tick();   break;
	case 19:          n163_audio_tick(); break;
	case 5:           mmc5_audio_tick(); break;
	case 85:          vrc7_audio_tick(); break;
	default: break;
	}
}

int cart_audio_out(void)
{
	switch (g.cart.mapper) {
	case 24: case 26: return vrc6_audio_out();
	case 69:          return ay_audio_out();
	case 19:          return n163_audio_out();
	case 5:           return mmc5_audio_out();
	case 85:          return vrc7_audio_out();
	default: return 0;
	}
}

/* ==================== Additional discrete / unlicensed mappers =========== */

static void set_chr2(int win, u32 bank)        /* 2KB CHR window (win 0..3) */
{
	set_chr1(win * 2,     bank * 2);
	set_chr1(win * 2 + 1, bank * 2 + 1);
}

/* ---- Namco 109 / NAMCOT-3446 (76): DxROM subset, all-2KB CHR ---- */
static void namco76_write(u16 a, u8 v)
{
	/* registers decode ONLY in $8000-$9FFF: even=$8000 select, odd=$8001 data
	 * (the (a & 0xE001) mask; writes to $A000-$FFFF are ignored by the chip). */
	if ((a & 0xE001) == 0x8000) { g.cart.m4_bank_select = v & 7; return; }
	if ((a & 0xE001) != 0x8001) return;
	switch (g.cart.m4_bank_select) {               /* odd: data (6-bit) */
	case 2: set_chr2(0, v & 0x3F); break;
	case 3: set_chr2(1, v & 0x3F); break;
	case 4: set_chr2(2, v & 0x3F); break;
	case 5: set_chr2(3, v & 0x3F); break;
	case 6: set_prg8(0, v & 0x3F); break;
	case 7: set_prg8(1, v & 0x3F); break;
	default: break;
	}
}

/* ---- Taito X1-005 (80) and its per-half CIRAM variant (207) ----
 * Registers live at $7EF0-$7EFF (inside the PRG-RAM page). 80 uses $7EF6 bit0
 * for H/V; 207 ignores $7EF6 and drives per-half CIRAM from bit7 of the 2KB regs. */
static void taito80_write(u16 a, u8 v)
{
	switch (a & 0x000F) {
	case 0x0: set_chr2(0, v); break;
	case 0x1: set_chr2(1, v); break;
	case 0x2: set_chr1(4, v); break;
	case 0x3: set_chr1(5, v); break;
	case 0x4: set_chr1(6, v); break;
	case 0x5: set_chr1(7, v); break;
	case 0x6: g.cart.mirror = (v & 1) ? MIR_VERT : MIR_HORZ; break;
	case 0xA: case 0xB: set_prg8(0, v); break;
	case 0xC: case 0xD: set_prg8(1, v); break;
	case 0xE: case 0xF: set_prg8(2, v); break;
	default: break;                                /* $7EF8/$7EF9 RAM enable */
	}
}
static void taito207_write(u16 a, u8 v)
{
	switch (a & 0x000F) {
	case 0x0: set_chr2(0, v & 0x7F); g.cart.nt_custom = 1;
	          g.cart.nt_map[0] = g.cart.nt_map[1] = (v >> 7) & 1; break;
	case 0x1: set_chr2(1, v & 0x7F); g.cart.nt_custom = 1;
	          g.cart.nt_map[2] = g.cart.nt_map[3] = (v >> 7) & 1; break;
	case 0x2: set_chr1(4, v); break;
	case 0x3: set_chr1(5, v); break;
	case 0x4: set_chr1(6, v); break;
	case 0x5: set_chr1(7, v); break;
	case 0xA: case 0xB: set_prg8(0, v); break;
	case 0xC: case 0xD: set_prg8(1, v); break;
	case 0xE: case 0xF: set_prg8(2, v); break;
	default: break;                                /* $7EF6 ignored, $7EF8/9 RAM en */
	}
}

/* ---- Taito X1-017 (82): CHR A12-invert + PRG value >>2 ---- */
static void taito82_apply(void)
{
	if (g.cart.p4_layout & 2) {                    /* C=1: 2KB @ $1000, 1KB @ $0000 */
		set_chr2(2, g.cart.p4_chr[0]); set_chr2(3, g.cart.p4_chr[1]);
		set_chr1(0, g.cart.p4_chr[2]); set_chr1(1, g.cart.p4_chr[3]);
		set_chr1(2, g.cart.p4_chr[4]); set_chr1(3, g.cart.p4_chr[5]);
	} else {                                       /* C=0: 2KB @ $0000, 1KB @ $1000 */
		set_chr2(0, g.cart.p4_chr[0]); set_chr2(1, g.cart.p4_chr[1]);
		set_chr1(4, g.cart.p4_chr[2]); set_chr1(5, g.cart.p4_chr[3]);
		set_chr1(6, g.cart.p4_chr[4]); set_chr1(7, g.cart.p4_chr[5]);
	}
}
static void taito82_write(u16 a, u8 v)
{
	switch (a & 0x000F) {
	case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5:
		g.cart.p4_chr[a & 7] = v; taito82_apply(); break;
	case 0x6: g.cart.mirror = (v & 1) ? MIR_VERT : MIR_HORZ;
	          g.cart.p4_layout = v; taito82_apply(); break;
	case 0xA: set_prg8(0, v >> 2); break;          /* PRG value >>2 quirk */
	case 0xB: set_prg8(1, v >> 2); break;
	case 0xC: set_prg8(2, v >> 2); break;
	default: break;   /* $7EF7/8/9 are the 3 WRAM-enable magic latches ($CA/$69/
	                   * $84): we leave the $6000-$73FF battery RAM always enabled
	                   * (real games write the latch then use RAM; no title relies
	                   * on the disabled-region open-bus behavior). */
	}
}

/* ---- Namco 175 (sub1) / 340 (sub2) (210): like Namco163 banking, no IRQ/audio --
 * p4_layout: 0 = sub1 (fixed header mirror), 1 = sub2 (programmable mirror).
 * Note: the Namco 175 $C000-$C7FF PRG-RAM enable latch is not enforced — the
 * $6000-$7FFF work RAM is left always enabled (real games enable before use). */
static void namco210_write(u16 a, u8 v)
{
	if (a < 0xC000) {                              /* $8000-$B800: 8x1KB CHR */
		set_chr1((a - 0x8000) >> 11, v);
	} else if (a >= 0xE000 && a < 0xE800) {        /* PRG0 + (340) mirror */
		set_prg8(0, v & 0x3F);
		if (g.cart.p4_layout) {
			static const u8 m[4] = { MIR_SINGLE0, MIR_VERT, MIR_SINGLE1, MIR_HORZ };
			g.cart.mirror = m[(v >> 6) & 3];
		}
	} else if (a >= 0xE800 && a < 0xF000) set_prg8(1, v & 0x3F);
	else if (a >= 0xF000 && a < 0xF800)   set_prg8(2, v & 0x3F);
}

/* ---- Irem TAM-S1 (97): reversed UxROM (switch @ $C000, fixed @ $8000) ---- */
static void irem97_write(u16 a, u8 v)
{
	(void)a;
	set_prg16(1, v & 0x3F);
	g.cart.mirror = (v & 0x80) ? MIR_VERT : MIR_HORZ;
}

/* ---- Camerica/Codemasters BF9096 (232) ---- */
static void bf9096_apply(void)
{
	set_prg16(0, g.cart.p4_prg[0] * 4 + g.cart.p4_prg[1]);   /* block*4 + page */
	set_prg16(1, g.cart.p4_prg[0] * 4 + 3);                  /* block*4 + 3 (fixed) */
}
static void bf9096_write(u16 a, u8 v)
{
	if (a < 0xC000) g.cart.p4_prg[0] = (v >> 3) & 3;          /* outer block */
	else            g.cart.p4_prg[1] = v & 3;                 /* inner page */
	bf9096_apply();
}

/* ---- K-1029 multicart (15) ---- */
static void multi15_write(u16 a, u8 v)
{
	int ss = a & 3;
	u32 B = v & 0x3F;
	int p = (v >> 7) & 1;
	g.cart.mirror = (v & 0x40) ? MIR_HORZ : MIR_VERT;
	switch (ss) {
	case 0: set_prg16(0, B & 0xFE); set_prg16(1, B | 0x01); break;   /* 32K */
	case 1: set_prg16(0, B);        set_prg16(1, B | 0x07); break;   /* UNROM */
	case 2: { u32 b = ((u32)B << 1) | (u32)p;                        /* 8K mirror */
	          set_prg8(0, b); set_prg8(1, b); set_prg8(2, b); set_prg8(3, b); } break;
	default: set_prg16(0, B); set_prg16(1, B); break;               /* 16K mirror */
	}
}

/* ---- NINA-03/06 (79) / Sachen (146): $41xx PRG32 + CHR8 ---- */
static void nina79_write(u16 a, u8 v)
{
	(void)a;
	set_prg32((v >> 3) & 1);
	set_chr8(v & 0x07);
}
/* ---- HES NINA variant (113): + mirror + split CHR bank ---- */
static void nina113_write(u16 a, u8 v)
{
	(void)a;
	set_prg32((v >> 3) & 0x07);
	set_chr8(((v & 0x40) >> 3) | (v & 0x07));
	g.cart.mirror = (v & 0x80) ? MIR_VERT : MIR_HORZ;
}
/* ---- TXC 01-22000 (36): $4200 CHR, $8000 PRG (minimal, no protection) ---- */
static void txc36_write(u16 a, u8 v)
{
	if ((a & 0xE200) == 0x4200) set_chr8(v & 0x0F);
	else if (a >= 0x8000)       set_prg32((v >> 4) & 3);
}

/* ---- UNROM-512 (30): 16KB PRG switch + fixed last, up to 32KB CHR-RAM ----
 * Single register $8000-$FFFF, byte = [M CC PPPPP]: bits0-4 PRG 16KB bank,
 * bits5-6 CHR-RAM 8KB bank, bit7 1-screen select (only when p4_layout=1, i.e.
 * the board is in mapper-controlled 1-screen mode per the header). PRG/CHR flash
 * self-programming is not emulated (CHR-RAM stays plain RAM). */
static void unrom512_write(u16 a, u8 v)
{
	(void)a;
	set_prg16(0, v & 0x1F);
	set_chr8((v >> 5) & 0x03);
	if (g.cart.p4_layout)
		g.cart.mirror = (v & 0x80) ? MIR_SINGLE1 : MIR_SINGLE0;
}

/* --------------------------------------------- MMC2 (9) / MMC4 (10) ------ */

static void mmc2_apply(void)
{
	u32 last = prg_banks8();
	if (last == 0) last = 1;
	if (g.cart.mapper == 10) {            /* MMC4: 16KB switch at $8000, last fixed */
		set_prg16(0, g.cart.m9_prg & 0x0F);
		set_prg16(1, (last / 2) - 1);
	} else {                              /* MMC2: 8KB switch at $8000, last 3 fixed */
		set_prg8(0, g.cart.m9_prg & 0x1F);
		set_prg8(1, last - 3);
		set_prg8(2, last - 2);
		set_prg8(3, last - 1);
	}
	set_chr4(0, g.cart.m9_chr[g.cart.m9_latch[0] ? 1 : 0]);   /* $0000 4KB */
	set_chr4(1, g.cart.m9_chr[g.cart.m9_latch[1] ? 3 : 2]);   /* $1000 4KB */
}

static void mmc2_write(u16 a, u8 v)
{
	switch (a & 0xF000) {
	case 0xA000: g.cart.m9_prg    = v & 0x1F; break;
	case 0xB000: g.cart.m9_chr[0] = v & 0x1F; break;   /* $0000 latch=FD */
	case 0xC000: g.cart.m9_chr[1] = v & 0x1F; break;   /* $0000 latch=FE */
	case 0xD000: g.cart.m9_chr[2] = v & 0x1F; break;   /* $1000 latch=FD */
	case 0xE000: g.cart.m9_chr[3] = v & 0x1F; break;   /* $1000 latch=FE */
	case 0xF000: g.cart.mirror = (v & 1) ? MIR_HORZ : MIR_VERT; break;
	default: break;                                    /* $8000-$9FFF: no reg */
	}
	mmc2_apply();
}

/* Called from the renderer after fetching a BG tile: a $FD/$FE tile in a pattern
 * table flips that table's 4KB CHR latch (MMC2/4). patbase = 0x0000 or 0x1000.
 * The flip affects subsequent fetches (the canonical scanline-renderer approx). */
void cart_chr_latch(u16 patbase, u8 tile)
{
	int half = (patbase & 0x1000) ? 1 : 0;
	int want = (tile == 0xFE) ? 1 : 0;       /* $FD -> 0, $FE -> 1 */
	if (g.cart.m9_latch[half] != want) {
		g.cart.m9_latch[half] = (u8)want;
		mmc2_apply();
	}
}

/* ------------------------------------------------- Sunsoft FME-7 (69) ---- */

static void fme7_write(u16 a, u8 v)
{
	if (a < 0xA000) {                        /* $8000-$9FFF: command select */
		g.cart.m69_cmd = v & 0x0F;
		return;
	}
	switch (g.cart.m69_cmd) {                /* $A000-$BFFF: parameter */
	case 0: case 1: case 2: case 3:
	case 4: case 5: case 6: case 7:
		set_chr1(g.cart.m69_cmd, v);         /* 8 x 1KB CHR banks */
		break;
	case 9:  set_prg8(0, v & 0x3F); break;   /* $8000 (bit6 ROM/RAM ignored) */
	case 10: set_prg8(1, v & 0x3F); break;   /* $A000 */
	case 11: set_prg8(2, v & 0x3F); break;   /* $C000 */
	case 12:                                 /* mirroring */
		switch (v & 3) {
		case 0: g.cart.mirror = MIR_VERT;    break;
		case 1: g.cart.mirror = MIR_HORZ;    break;
		case 2: g.cart.mirror = MIR_SINGLE0; break;
		case 3: g.cart.mirror = MIR_SINGLE1; break;
		}
		break;
	case 13:                                 /* IRQ control: bit7 counter-en, bit0 IRQ-en */
		g.cart.m69_irq_ctrl = v;
		g.cpu.irq_line &= ~IRQ_MAPPER;       /* writing it acknowledges a pending IRQ */
		break;
	case 14: g.cart.m69_irq_ctr = (u16)((g.cart.m69_irq_ctr & 0xFF00) | v); break;
	case 15: g.cart.m69_irq_ctr = (u16)((g.cart.m69_irq_ctr & 0x00FF) | (v << 8)); break;
	default: break;                          /* cmd 8 ($6000 bank): use prg_ram */
	}
}

/* Clock a per-CPU-cycle mapper IRQ counter by `cyc` CPU cycles. Called once per
 * instruction from nes_run_frame for mappers that need cycle-accurate counting:
 *   69 (FME-7)        16-bit down-counter, IRQ on underflow.
 *   64 (RAMBO-1)      in CPU-cycle mode only: an MMC3-style counter clocked at
 *                     one tick per 4 CPU cycles (the scanline mode is driven
 *                     from cart_ppu_scanline instead). */
void cart_irq_clock(int cyc)
{
	switch (g.cart.mapper) {
	case 69: {                               /* FME-7: 16-bit down-counter */
		int newv;
		if (!(g.cart.m69_irq_ctrl & 0x80))   /* bit7 = counter enable */
			return;
		newv = (int)g.cart.m69_irq_ctr - cyc;
		if (newv < 0 && (g.cart.m69_irq_ctrl & 0x01))  /* bit0 = IRQ enable */
			g.cpu.irq_line |= IRQ_MAPPER;
		g.cart.m69_irq_ctr = (u16)(newv & 0xFFFF);
		break;
	}
	case 64:                                 /* RAMBO-1: CPU-cycle mode (/4) */
		if (g.cart.m64_irq_mode == 1) {
			g.cart.m64_irq_pre = (u16)(g.cart.m64_irq_pre + cyc);
			while (g.cart.m64_irq_pre >= 4) {
				g.cart.m64_irq_pre = (u16)(g.cart.m64_irq_pre - 4);
				mmc3_irq_tick();
			}
		}
		break;
	case 73:                                 /* VRC3: 16-bit per-cycle counter */
		vrc3_irq_clock(cyc);
		break;
	case 21: case 23: case 24: case 25: case 26: case 85:  /* VRC4/6/7 IRQ */
		vrc_irq_clock(cyc);
		break;
	case 16: case 159: {                     /* Bandai: 16-bit down, fire at 0 */
		int newv;
		if (!g.cart.p4_irq_enable) break;
		newv = (int)g.cart.p4_irq_counter - cyc;
		if (newv < 0) g.cpu.irq_line |= IRQ_MAPPER;
		g.cart.p4_irq_counter = (u16)(newv & 0xFFFF);
		break;
	}
	case 65: {                               /* Irem H3001: down, stops at 0 */
		int newv;
		if (!g.cart.p4_irq_enable) break;
		newv = (int)g.cart.p4_irq_counter - cyc;
		if (newv <= 0) { newv = 0; g.cpu.irq_line |= IRQ_MAPPER; }
		g.cart.p4_irq_counter = (u16)newv;
		break;
	}
	case 67: {                               /* Sunsoft-3: down, underflow, pauses */
		int newv;
		if (!g.cart.p4_irq_enable) break;
		newv = (int)g.cart.p4_irq_counter - cyc;
		if (newv < 0) { g.cpu.irq_line |= IRQ_MAPPER; g.cart.p4_irq_enable = 0; }
		g.cart.p4_irq_counter = (u16)(newv & 0xFFFF);
		break;
	}
	case 18: {                               /* Jaleco SS88006: width-masked down */
		int i;
		u16 mask;
		if (!(g.cart.p4_irq_width & 1)) break;      /* C = counting enable */
		mask = 0xFFFF;
		if (g.cart.p4_irq_width & 8) mask = 0x0FFF;
		else if (g.cart.p4_irq_width & 4) mask = 0x00FF;
		else if (g.cart.p4_irq_width & 2) mask = 0x000F;
		for (i = 0; i < cyc; i++) {
			u16 low = g.cart.p4_irq_counter & mask;
			if (low == 0) {
				g.cpu.irq_line |= IRQ_MAPPER;
				g.cart.p4_irq_counter = (u16)((g.cart.p4_irq_counter & ~mask) | mask);
			} else {
				g.cart.p4_irq_counter = (u16)((g.cart.p4_irq_counter & ~mask) | ((low - 1) & mask));
			}
		}
		break;
	}
	case 19: {                               /* Namco163: 15-bit up, fire at $7FFF */
		int i;
		if (!g.cart.p4_irq_enable) break;
		for (i = 0; i < cyc; i++) {
			if (g.cart.p4_irq_counter < 0x7FFF) {
				g.cart.p4_irq_counter++;
				if (g.cart.p4_irq_counter == 0x7FFF)
					g.cpu.irq_line |= IRQ_MAPPER;
			}
		}
		break;
	}
	default:
		break;
	}
}

/* ------------------------------------------------------- PRG read/write -- */

u8 cart_cpu_read(u16 a)
{
	if (a >= 0x8000) {                /* hot: PRG-ROM fetch first */
		int slot = (a >> 13) & 3;     /* 0:$8000 1:$A000 2:$C000 3:$E000 */
		return g.cart.prg[g.cart.prg_off[slot] + (a & 0x1FFF)];
	}
	if (a >= 0x6000)                  /* $6000-$7FFF PRG-RAM */
		return g.cart.prg_ram[a - 0x6000];
	if (g.cart.mapper == 5 && a >= 0x5000)   /* MMC5 multiplier/ExRAM/IRQ status */
		return mmc5_read(a);
	if (g.cart.mapper == 19 && a >= 0x4800 && a < 0x5000)  /* Namco163 sound data */
		return n163_audio_data_read();
	return 0;
}

#ifdef CART_DBG_HOOK
void (*cart_dbg_write_hook)(u16, u8) = 0;   /* host debug: observe cart writes */
int g_m8_mir   = -1;  /* host debug: -1=header; else force MIR_* (0=H 1=V 2=S0 3=S1 4=four) */
#endif
void cart_cpu_write(u16 a, u8 v)
{
#ifdef CART_DBG_HOOK
	if (cart_dbg_write_hook) cart_dbg_write_hook(a, v);
#endif
	if (g.cart.mapper == 19 && a >= 0x4800 && a < 0x6000) {  /* Namco163 IRQ/sound */
		n163_write(a, v);
		return;
	}
	if (g.cart.mapper == 5 && a >= 0x5000 && a < 0x6000) {   /* MMC5 registers */
		mmc5_write(a, v);
		return;
	}
	if (a >= 0x4020 && a < 0x6000) {     /* low-address register mappers */
		switch (g.cart.mapper) {
		case 79: case 146: if (a & 0x0100) nina79_write(a, v); break;  /* NINA-03/06 */
		case 113:          if (a & 0x0100) nina113_write(a, v); break; /* HES NINA   */
		case 36:           txc36_write(a, v); break;                   /* TXC $4200  */
		default: break;
		}
		return;
	}
	if (a >= 0x6000 && a < 0x8000) {
		g.cart.prg_ram[a - 0x6000] = v;
		switch (g.cart.mapper) {         /* mappers whose bank reg lives at $6000-$7FFF */
		case 87:  set_chr8((u32)(((v & 1) << 1) | ((v >> 1) & 1))); break;  /* Jaleco */
		case 38:  set_prg32(v & 0x03); set_chr8((v >> 2) & 0x03); break;    /* PCI556 */
		case 140: set_prg32((v >> 4) & 0x0F); set_chr8(v & 0x0F); break;    /* Jaleco JF-11/14 */
		case 184: set_chr4(0, v & 0x07); set_chr4(1, (v >> 4) & 0x07); break; /* Sunsoft */
		case 16: case 159: bandai16_write(a, v); break;   /* Bandai FCG @ $6000   */
		case 80:  if ((a & 0xFFF0) == 0x7EF0) taito80_write(a, v);  break;  /* Taito X1-005 */
		case 207: if ((a & 0xFFF0) == 0x7EF0) taito207_write(a, v); break;  /* X1-005 variant */
		case 82:  if ((a & 0xFFF0) == 0x7EF0) taito82_write(a, v);  break;  /* Taito X1-017 */
		default:  break;
		}
		return;
	}
	if (a < 0x8000)
		return;

	switch (g.cart.mapper) {
	case 0:                          /* NROM: no registers */
		break;
	case 1:
		mmc1_write(a, v);
		break;
	case 2:                          /* UNROM: 16KB switch at $8000 */
		g.cart.bank_latch = v;       /* set_prg16's % wraps to the bank count */
		set_prg16(0, g.cart.bank_latch);
		break;
	case 3:                          /* CNROM: 8KB CHR switch */
		g.cart.bank_latch = v;       /* set_chr8's % wraps to the bank count */
		set_chr8(g.cart.bank_latch);
		break;
	case 4:
	case 118:                        /* TxSROM/TLSROM (MMC3 + per-bank nametable)  */
		mmc3_write(a, v);
		break;
	case 33:                         /* Taito TC0190 (no IRQ)                      */
	case 48:                         /* Taito TC0690 (TC0190 + scanline IRQ)       */
		taito_write(a, v);
		break;
	case 64:                         /* Tengen RAMBO-1 (MMC3-like + dual IRQ)      */
		rambo1_write(a, v);
		break;
	case 75:                         /* Konami VRC1                               */
		vrc1_write(a, v);
		break;
	case 21:                         /* Konami VRC4a/c                            */
	case 22:                         /* Konami VRC2a                              */
	case 23:                         /* Konami VRC2b / VRC4e/f                     */
	case 25:                         /* Konami VRC2c / VRC4b/d                     */
		vrc24_write(a, v);
		break;
	case 24:                         /* Konami VRC6a (banking + audio)            */
	case 26:                         /* Konami VRC6b                              */
		vrc6_write(a, v);            /* $8000/$B003/$C000-$F002 banking + IRQ     */
		vrc6_audio_write(a, v);      /* $9000-$9003/$A000-2/$B000-2 audio         */
		break;
	case 85:                         /* Konami VRC7 (banking + OPLL FM audio)     */
		vrc7_write(a, v);
		if ((a & 0xF010) == 0x9010) { /* $9010 = OPLL addr, $9030 = OPLL data    */
			if (a & 0x20) opll_write(g.cart.v7_adr, v);
			else          g.cart.v7_adr = v;
		}
		break;
	case 73:                         /* Konami VRC3 (Salamander)                  */
		vrc3_write(a, v);
		break;
	case 16:                         /* Bandai FCG / LZ93D50                      */
	case 159:                        /* Bandai LZ93D50 (X24C01 EEPROM)            */
		bandai16_write(a, v);
		break;
	case 18:  jaleco18_write(a, v);  break;  /* Jaleco SS88006                    */
	case 32:  irem32_write(a, v);    break;  /* Irem G-101                        */
	case 65:  irem65_write(a, v);    break;  /* Irem H3001                        */
	case 67:  sunsoft3_write(a, v);  break;  /* Sunsoft-3                         */
	case 68:  sunsoft4_write(a, v);  break;  /* Sunsoft-4 (CHR-ROM nametables)    */
	case 19:  n163_write(a, v);      break;  /* Namco 163 (audio = Phase 6)       */
	case 206:                        /* DxROM / Namcot 108 (MMC3 subset, no IRQ)  */
	case 88:                         /* Namcot 118 (1KB CHR from upper half)      */
	case 95:                         /* Namcot 118 + CHR-driven nametable          */
	case 154:                        /* Namcot 118 + 1-screen mirror               */
		namcot_write(a, v);
		break;
	case 8:                          /* FFE F3xxx (= mapper 6 submapper 4)     */
		g.cart.bank_latch = v;       /* register byte = [PPPP PCCC]           */
		/* 32KB-aligned PRG pair: $8000 = 16KB bank (v>>3), $C000 = its odd
		 * sibling (v>>3)|1. Carts write only EVEN (v>>3), so this is the
		 * proper 32KB window; the odd half at $C000 is where the reset/NMI
		 * vectors AND the level data live (they differ per bank -- odd banks
		 * 1/3/5/7 share only the vectors). Fixing $C000 to bank 1 fed the
		 * level decoder the WRONG 16KB of data -> garbage terrain / crash. */
		set_prg16(0, v >> 3);
		set_prg16(1, (v >> 3) | 1);
		set_chr8(v & 0x07);          /* 8KB CHR bank (low 3 bits)             */
		break;
	case 7:                          /* AxROM: 32KB PRG switch + 1-screen mirror */
		g.cart.bank_latch = v;
		set_prg32(v & 0x07);
		g.cart.mirror = (v & 0x10) ? MIR_SINGLE1 : MIR_SINGLE0;
		break;
	case 11:                         /* Color Dreams: PRG(lo 2) + CHR(hi 4)      */
		g.cart.bank_latch = v;
		set_prg32(v & 0x03);
		set_chr8((v >> 4) & 0x0F);
		break;
	case 66:                         /* GxROM/MxROM: PRG(bits4-5) + CHR(bits0-1)  */
		g.cart.bank_latch = v;
		set_prg32((v >> 4) & 0x03);
		set_chr8(v & 0x03);
		break;
	case 71:                         /* Camerica/Codemasters BF909x: UxROM-like   */
		g.cart.bank_latch = v;
		set_prg16(0, v & 0x0F);      /* 16KB switch at $8000; last 16KB fixed     */
		break;
	case 9:                          /* MMC2 */
	case 10:                         /* MMC4 */
		mmc2_write(a, v);
		break;
	case 69:                         /* Sunsoft FME-7 / 5B                        */
		if (a >= 0xC000) ay_audio_write(a, v);   /* 5B audio: $C000 sel/$E000 data */
		else             fme7_write(a, v);        /* $8000/$A000 banking + IRQ      */
		break;
	case 34:                         /* BNROM: 32KB PRG (CHR-RAM)                 */
		g.cart.bank_latch = v;
		set_prg32(v);
		break;
	case 70:                         /* Bandai: PRG(bits4-7) + CHR(bits0-3)       */
		g.cart.bank_latch = v;
		set_prg16(0, (v >> 4) & 0x0F);
		set_chr8(v & 0x0F);
		break;
	case 152:                        /* Bandai: 1-screen mirror + PRG(4-6)+CHR(0-3)*/
		g.cart.bank_latch = v;
		set_prg16(0, (v >> 4) & 0x07);
		set_chr8(v & 0x0F);
		g.cart.mirror = (v & 0x80) ? MIR_SINGLE1 : MIR_SINGLE0;
		break;
	case 93:                         /* Sunsoft-3R: PRG(bits4-6), CHR-RAM         */
		g.cart.bank_latch = v;
		set_prg16(0, (v >> 4) & 0x07);
		break;
	case 87:                         /* Jaleco: CHR bank is at $6000-$7FFF (above)*/
	case 38:                         /* PCI556 / JF-11/14 / Sunsoft 184: at $6000   */
	case 140:
	case 184:
		break;
	case 78:                         /* sub3 default: bit3 0=H, 1=V               */
		g.cart.bank_latch = v;
		set_prg16(0, v & 0x07);
		set_chr8((v >> 4) & 0x0F);
		g.cart.mirror = (v & 0x08) ? MIR_VERT : MIR_HORZ;
		break;
	case 89:                         /* Sunsoft-2: CPPP MCCC (CHR hi=bit7, mir=bit3)*/
		g.cart.bank_latch = v;
		set_prg16(0, (v >> 4) & 0x07);
		set_chr8(((v >> 4) & 0x08) | (v & 0x07));
		g.cart.mirror = (v & 0x08) ? MIR_SINGLE1 : MIR_SINGLE0;
		break;
	case 94:                         /* UN1ROM: PRG bits 2-4                      */
		g.cart.bank_latch = v;
		set_prg16(0, (v >> 2) & 0x07);
		break;
	case 180:                        /* reversed UxROM: $8000 fixed, $C000 switch  */
		g.cart.bank_latch = v;
		set_prg16(1, v & 0x07);
		break;
	case 185:                        /* CNROM copy-protect: ignore CHR writes      */
		break;
	case 76:  namco76_write(a, v);   break;  /* Namco 109 / NAMCOT-3446           */
	case 210: namco210_write(a, v);  break;  /* Namco 175 / 340                   */
	case 97:  irem97_write(a, v);    break;  /* Irem TAM-S1 (reversed UxROM)      */
	case 232: bf9096_write(a, v); break; /* Camerica/Codemasters BF9096       */
	case 15:  multi15_write(a, v);   break;  /* K-1029 multicart                  */
	case 36:  txc36_write(a, v);     break;  /* TXC 01-22000 ($8000 PRG copy)     */
	case 30:  unrom512_write(a, v);  break;  /* UNROM-512 (homebrew)              */
	}
}

/* ------------------------------------------------------- CHR read/write -- */

u8 cart_chr_read(u16 a)
{
	/* callers pass a < 0x2000 (a &= 0x1FFF was dead); hot reads use the inline
	 * cart_chr_read_fast in nesint.h. */
	return g.cart.chr[g.cart.chr_off[(a >> 10) & 7] + (a & 0x3FF)];
}

void cart_chr_write(u16 a, u8 v)
{
	a &= 0x1FFF;
	if (g.cart.chr_is_ram)
		g.cart.chr[g.cart.chr_off[(a >> 10) & 7] + (a & 0x3FF)] = v;
}

/* ------------------------------------------------ nametable mirroring ---- */

/* physical CIRAM page (0..3) for a logical nametable, honoring the per-table
 * custom maps (TxSROM/95/154) and the CIRAM side of CHR-ROM-nametable mappers. */
static int cart_nt_page(int table)
{
	if (g.cart.nt_custom) return g.cart.nt_map[table];     /* TxSROM/95/154   */
	if (g.cart.nt_rom)    return g.cart.nt_page[table];     /* Sunsoft-4/N163  */
	switch (g.cart.mirror) {
	case MIR_HORZ:    return (table >> 1);              /* 0,0,1,1 */
	case MIR_VERT:    return (table & 1);               /* 0,1,0,1 */
	case MIR_SINGLE0: return 0;
	case MIR_SINGLE1: return 1;
	case MIR_FOUR:    return table;                     /* 0,1,2,3 */
	default:          return 0;
	}
}

/* Base pointer for the 1KB nametable region containing `a`: the byte is
 * cart_nt_base(a)[a & 0x3FF]. Returns CHR-ROM for Sunsoft-4 / Namco163 ROM
 * nametables, else CIRAM. The render path and ppu_mem_read use this. */
const u8 *cart_nt_base(u16 a)
{
	int table = (a >> 10) & 3;
	if (g.cart.mapper == 5) {            /* MMC5 $5105: CIRAM/ExRAM/fill per NT */
		switch ((g.cart.m5_nt_map >> (table * 2)) & 3) {
		case 0:  return g.ppu.vram + 0;
		case 1:  return g.ppu.vram + 0x400;
		case 2:  return g.cart.m5_exram;
		default: return g.cart.m5_fillbuf;
		}
	}
	if (g.cart.nt_rom && g.cart.nt_is_rom[table]) {
		u32 nb = chr_banks1();
		if (nb == 0) nb = 1;
		return g.cart.chr + (g.cart.nt_bank[table] % nb) * CHR1K;
	}
	return g.ppu.vram + cart_nt_page(table) * 0x400;
}

u16 cart_nt_index(u16 a)            /* CIRAM byte index (used by writes) */
{
	return (u16)(cart_nt_page((a >> 10) & 3) * 0x400 + (a & 0x3FF));
}

/* ----------------------------------------------------------- iNES load --- */

int cart_load(const u8 *rom, long len, const char **err)
{
	static u8 chr_ram_dummy;
	u32 prg16, chr8;
	long need;
	const u8 *p;
	int trainer;
	(void)chr_ram_dummy;

	if (len < 16 || rom[0] != 'N' || rom[1] != 'E' || rom[2] != 'S' || rom[3] != 0x1A) {
		if (err) *err = "iNES header not found (expected 'NES\\x1a')";
		return -1;
	}

	prg16 = rom[4];
	chr8  = rom[5];
	if (prg16 == 0) {
		if (err) *err = "PRG size is 0";
		return -1;
	}
	trainer = (rom[6] & 0x04) ? 512 : 0;

	need = 16 + trainer + (long)prg16 * 16384 + (long)chr8 * 8192;
	if (len < need) {
		if (err) *err = "ROM image is truncated (smaller than header declares)";
		return -1;
	}

	memset(&g.cart, 0, sizeof(g.cart));

	/* Mapper number. Many old dumps carry a corrupt "archaic" header where bytes
	 * 7..15 hold ASCII garbage (e.g. "DiskDude!"), which poisons the mapper high
	 * nibble in byte 7. Only trust byte 7 for NES 2.0 headers or when bytes 12-15
	 * are clean; otherwise use just the low nibble (byte 6 >> 4). */
	g.cart.mapper = rom[6] >> 4;
	if ((rom[7] & 0x0C) == 0x08)
		g.cart.mapper |= (rom[7] & 0xF0);                 /* NES 2.0 */
	else if (rom[12] == 0 && rom[13] == 0 && rom[14] == 0 && rom[15] == 0)
		g.cart.mapper |= (rom[7] & 0xF0);                 /* clean iNES 1.0 */
	/* else: archaic/garbage header -> keep only the low nibble */

	if (rom[6] & 0x08)
		g.cart.mirror = MIR_FOUR;
	else
		g.cart.mirror = (rom[6] & 0x01) ? MIR_VERT : MIR_HORZ;

	p = rom + 16 + trainer;
	g.cart.prg = (u8 *)p;
	g.cart.prg_size = prg16 * 16384u;
	p += g.cart.prg_size;

	if (chr8 == 0) {
		g.cart.chr = g.cart.chr_ram;
		/* UNROM-512 (30) uses up to 32KB CHR-RAM (4 banks); all others use 8KB. */
		g.cart.chr_size = (g.cart.mapper == 30) ? 32768 : 8192;
		g.cart.chr_is_ram = 1;
	} else {
		g.cart.chr = (u8 *)p;
		g.cart.chr_size = chr8 * 8192u;
		g.cart.chr_is_ram = 0;
	}

	/* iNES trainer (flag6 bit2): 512 bytes that load at $7000-$71FF (prg_ram offset
	 * 0x1000). Images carrying one JSR/JMP into it during boot to apply patches;
	 * without it they execute empty RAM and hang. */
	if (trainer)
		memcpy(g.cart.prg_ram + 0x1000, rom + 16, 512);

	/* power-on bank layout per mapper */
	switch (g.cart.mapper) {
	case 0: /* NROM */
		if (prg16 == 1) { set_prg16(0, 0); set_prg16(1, 0); }
		else            { set_prg32(0); }
		set_chr8(0);
		break;
	case 1: /* MMC1 */
		g.cart.m1_ctrl = 0x0C;       /* PRG mode 3 (fix last at $C000) */
		g.cart.m1_shift = 0x10;
		g.cart.m1_count = 0;
		g.cart.m1_chr0 = g.cart.m1_chr1 = g.cart.m1_prg = 0;
		mmc1_apply();
		break;
	case 2: /* UNROM */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);     /* fixed last 16KB */
		set_chr8(0);
		break;
	case 3: /* CNROM */
		if (prg16 == 1) { set_prg16(0, 0); set_prg16(1, 0); }
		else            { set_prg32(0); }
		set_chr8(0);
		break;
	case 4: /* MMC3 */
		g.cart.m4_regs[0] = 0; g.cart.m4_regs[1] = 2;
		g.cart.m4_regs[2] = 4; g.cart.m4_regs[3] = 5;
		g.cart.m4_regs[4] = 6; g.cart.m4_regs[5] = 7;
		g.cart.m4_regs[6] = 0; g.cart.m4_regs[7] = 1;
		g.cart.m4_prg_mode = g.cart.m4_chr_mode = 0;
		mmc3_apply();
		break;
	case 8: /* FFE F3xxx. Single $8000-$FFFF latch [PPPP PCCC]: PRG = a 32KB-
		 * aligned pair of 16KB banks -- $8000 = bank (v>>3), $C000 = its odd
		 * sibling (v>>3)|1 -- and CHR = 8KB bank (v&7). Carts only ever write
		 * even (v>>3), so $C000 is the odd half of the 32KB window; the odd
		 * banks (1/3/5/7) each hold their OWN 16KB of data (they share only the
		 * reset/NMI vectors), so fixing $C000 to bank 1 feeds the wrong 16KB.
		 * Reset state = the v=0 pair (bank 0 at $8000, bank 1 at $C000). */
		set_prg16(0, 0);
		set_prg16(1, 1);
		set_chr8(0);
		/* Mirroring is 2-screen from header bit0 -- IGNORE the four-screen bit
		 * (bit3): FFE carts have no extra VRAM. Titles that scroll vertically by
		 * rewriting rows within NT0 (which mirrors to NT2 under VERT) would leave
		 * the mirrored table unwritten under four-screen, blanking the background
		 * whenever the scroll crosses into it. The header bit is spurious. */
#ifdef CART_DBG_HOOK
		if (g_m8_mir >= 0) g.cart.mirror = g_m8_mir;   /* host: force a mirror mode */
		else
#endif
		g.cart.mirror = (rom[6] & 0x01) ? MIR_VERT : MIR_HORZ;
		break;
	case 7: /* AxROM: 32KB PRG, CHR-RAM, single-screen mirroring (set by writes) */
		set_prg32(0);
		set_chr8(0);
		g.cart.mirror = MIR_SINGLE0;
		break;
	case 11: /* Color Dreams: 32KB PRG + 8KB CHR */
		set_prg32(0);
		set_chr8(0);
		break;
	case 66: /* GxROM/MxROM: 32KB PRG + 8KB CHR */
		set_prg32(0);
		set_chr8(0);
		break;
	case 71: /* Camerica/Codemasters: UxROM-like (16KB switch + fixed last) */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		break;
	case 9:  /* MMC2 */
	case 10: /* MMC4 */
		g.cart.chr_latch = 1;
		g.cart.m9_prg = 0;
		g.cart.m9_chr[0] = g.cart.m9_chr[1] = g.cart.m9_chr[2] = g.cart.m9_chr[3] = 0;
		g.cart.m9_latch[0] = g.cart.m9_latch[1] = 1;   /* power-on latch = FE */
		mmc2_apply();
		break;
	case 69: /* Sunsoft FME-7 / 5B */
		set_prg8(0, 0);
		set_prg8(1, 1);
		set_prg8(2, 2);
		set_prg8(3, (prg16 * 2) - 1);    /* $E000 fixed to the last 8KB bank */
		set_chr8(0);
		g.cart.irq_cpu_clocked = 1;
		g.cart.has_exp_audio = 1;        /* Sunsoft 5B: 3 square channels */
		break;
	case 34: /* BNROM (32KB PRG, CHR-RAM) */
		set_prg32(0);
		set_chr8(0);
		break;
	case 70:  /* Bandai (16KB switch + 8KB CHR, fixed last 16KB) */
	case 93:  /* Sunsoft-3R (16KB switch, CHR-RAM, fixed last 16KB) */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		break;
	case 152: /* Bandai 1-screen variant */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		g.cart.mirror = MIR_SINGLE0;
		break;
	case 87:  /* Jaleco CHR-bank ($6000-$7FFF); PRG fixed like NROM */
	case 184: /* Sunsoft CHR-bank ($6000-$7FFF); PRG fixed */
	case 185: /* CNROM copy-protect; PRG fixed, CHR fixed */
		if (prg16 == 1) { set_prg16(0, 0); set_prg16(1, 0); }
		else            { set_prg32(0); }
		set_chr8(0);
		break;
	case 38:  /* PCI556 (32KB PRG + 8KB CHR at $7000) */
	case 140: /* Jaleco JF-11/14 (32KB PRG + 8KB CHR at $6000) */
		set_prg32(0);
		set_chr8(0);
		break;
	case 78:  /* Irem 74HC161/32 (16KB switch + 8KB CHR + fixed last)          */
	case 94:  /* UN1ROM (16KB switch, CHR-RAM, fixed last) */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		break;
	case 89:  /* Sunsoft-2 (16KB switch + 8KB CHR + 1-screen, fixed last) */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		g.cart.mirror = MIR_SINGLE0;
		break;
	case 180: /* reversed UxROM ($8000 fixed bank 0, $C000 switchable) */
		set_prg16(0, 0);
		set_prg16(1, 0);
		set_chr8(0);
		break;
	case 206: /* DxROM / Namcot 108 (MMC3 subset, fixed mirror from header) */
	case 88:  /* Namcot 118 */
	case 95:  /* Namcot 118 + CHR-driven nametable */
	case 154: /* Namcot 118 + 1-screen mirror */
		g.cart.m4_regs[0] = 0; g.cart.m4_regs[1] = 2;
		g.cart.m4_regs[2] = 4; g.cart.m4_regs[3] = 5;
		g.cart.m4_regs[4] = 6; g.cart.m4_regs[5] = 7;
		g.cart.m4_regs[6] = 0; g.cart.m4_regs[7] = 1;
		if (g.cart.mapper == 154) g.cart.mirror = MIR_SINGLE0;
		namcot_apply();
		break;
	case 118: /* TxSROM/TLSROM (MMC3 core + per-bank nametable mirroring) */
		g.cart.m4_regs[0] = 0; g.cart.m4_regs[1] = 2;
		g.cart.m4_regs[2] = 4; g.cart.m4_regs[3] = 5;
		g.cart.m4_regs[4] = 6; g.cart.m4_regs[5] = 7;
		g.cart.m4_regs[6] = 0; g.cart.m4_regs[7] = 1;
		g.cart.m4_prg_mode = g.cart.m4_chr_mode = 0;
		mmc3_apply();
		break;
	case 33:  /* Taito TC0190 */
	case 48:  /* Taito TC0690 */
		{
			u32 last = prg_banks8();
			if (last == 0) last = 1;
			set_prg8(0, 0);
			set_prg8(1, 1);
			set_prg8(2, last - 2);       /* $C000 fixed */
			set_prg8(3, last - 1);       /* $E000 fixed */
			set_chr8(0);
		}
		break;
	case 64:  /* Tengen RAMBO-1 */
		g.cart.m64_regs[0] = 0; g.cart.m64_regs[1] = 1;
		g.cart.m64_regs[2] = 2; g.cart.m64_regs[3] = 3;
		g.cart.m64_regs[4] = 4; g.cart.m64_regs[5] = 5;
		g.cart.m64_regs[6] = 0; g.cart.m64_regs[7] = 1;
		g.cart.m64_regs[8] = 6; g.cart.m64_regs[9] = 7;
		g.cart.m64_regs[15] = 0;
		g.cart.m4_bank_select = 0;
		g.cart.irq_cpu_clocked = 1;
		rambo1_apply();
		break;
	case 75:  /* Konami VRC1 (no IRQ, no audio) */
		g.cart.vrc_prg[0] = 0; g.cart.vrc_prg[1] = 1; g.cart.vrc_prg[2] = 2;
		g.cart.vrc_chr[0] = 0; g.cart.vrc_chr[1] = 1;
		vrc1_apply();
		break;
	case 21:  /* Konami VRC4a/c */
	case 22:  /* Konami VRC2a */
	case 23:  /* Konami VRC2b / VRC4e/f */
	case 25:  /* Konami VRC2c / VRC4b/d */
		/* register-select de-scramble: OR every submapper variant's lines so
		 * a plain iNES dump (submapper 0) works whichever board it really is */
		switch (g.cart.mapper) {
		case 22: g.cart.vrc_a0 = 1u << 1;            g.cart.vrc_a1 = 1u << 0;            break;
		case 21: g.cart.vrc_a0 = (1u<<1)|(1u<<6);    g.cart.vrc_a1 = (1u<<2)|(1u<<7);    break;
		case 25: g.cart.vrc_a0 = (1u<<1)|(1u<<3);    g.cart.vrc_a1 = (1u<<0)|(1u<<2);    break;
		case 23: g.cart.vrc_a0 = (1u<<0)|(1u<<2);    g.cart.vrc_a1 = (1u<<1)|(1u<<3);    break;
		}
		g.cart.vrc_prg[0] = 0;
		g.cart.vrc_prg[1] = 1;
		g.cart.irq_cpu_clocked = (g.cart.mapper != 22);  /* VRC2a has no IRQ */
		vrc24_apply();
		break;
	case 24:  /* Konami VRC6a */
	case 26:  /* Konami VRC6b */
		g.cart.vrc_prg[0] = 0;
		g.cart.vrc_prg[1] = 0;
		g.cart.vrc6_ctrl  = 0;
		g.cart.irq_cpu_clocked = 1;
		g.cart.has_exp_audio = 1;        /* 2 pulse + sawtooth */
		vrc6_apply();
		break;
	case 85:  /* Konami VRC7 */
		g.cart.vrc_prg[0] = 0;
		g.cart.vrc_prg[1] = 1;
		g.cart.vrc_prg[2] = 2;
		g.cart.irq_cpu_clocked = 1;
		g.cart.has_exp_audio = 1;        /* OPLL FM (6 channels) */
		{   int i;                       /* OPLL reset: all slots silent (RELEASE) */
		    for (i = 0; i < 12; i++) { g.cart.v7_egout[i] = 127; g.cart.v7_egstate[i] = 5; }
		}
		vrc7_apply();
		break;
	case 73:  /* Konami VRC3 (Salamander) */
		g.cart.vrc_prg[0] = 0;
		g.cart.irq_cpu_clocked = 1;
		set_chr8(0);
		vrc3_apply();
		break;
	case 16:  /* Bandai FCG / LZ93D50 */
	case 159:
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		g.cart.irq_cpu_clocked = 1;
		break;
	case 18:  /* Jaleco SS88006 */
		g.cart.p4_prg[0] = 0; g.cart.p4_prg[1] = 1; g.cart.p4_prg[2] = 2;
		jaleco18_apply_prg();
		set_chr8(0);
		g.cart.irq_cpu_clocked = 1;
		break;
	case 32:  /* Irem G-101 */
		g.cart.p4_prg[0] = 0; g.cart.p4_prg[1] = 1;
		irem32_apply();
		set_chr8(0);
		break;
	case 65:  /* Irem H3001 */
		g.cart.p4_prg[0] = 0; g.cart.p4_prg[1] = 1;
		irem65_apply();
		set_chr8(0);
		g.cart.irq_cpu_clocked = 1;
		break;
	case 67:  /* Sunsoft-3 */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		g.cart.p4_irq_toggle = 1;
		g.cart.irq_cpu_clocked = 1;
		break;
	case 68:  /* Sunsoft-4 (CHR-ROM nametables) */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		break;
	case 19:  /* Namco 163 */
		g.cart.p4_prg[0] = 0; g.cart.p4_prg[1] = 1; g.cart.p4_prg[2] = 2;
		set_prg8(0, 0); set_prg8(1, 1); set_prg8(2, 2);
		set_prg8(3, (prg16 * 2) - 1);
		set_chr8(0);
		g.cart.irq_cpu_clocked = 1;
		g.cart.has_exp_audio = 1;        /* wavetable (up to 8 channels) */
		break;
	case 5:   /* MMC5 */
		g.cart.m5_prg_mode = 3;          /* power-on: 8K mode, $5117=last */
		g.cart.m5_chr_mode = 3;
		g.cart.m5_exram_mode = 0;
		g.cart.m5_prg_reg[0] = (u8)((prg16 * 2) - 4);
		g.cart.m5_prg_reg[1] = (u8)((prg16 * 2) - 3);
		g.cart.m5_prg_reg[2] = (u8)((prg16 * 2) - 2);
		g.cart.m5_prg_reg[3] = (u8)((prg16 * 2) - 1) | 0x80;
		g.cart.m5_nt_map = 0;            /* all CIRAM0 until set */
		mmc5_prg_apply();
		set_chr8(0);
		mmc5_set_a(g.cart.m5_chr_off_bg);
		mmc5_fill_apply();
		g.cart.has_exp_audio = 1;        /* 2 pulse + 8-bit PCM */
		g.cart.m5a_pcm = 128;            /* PCM DAC neutral (no DC offset when unused) */
		break;
	case 76:  /* Namco 109 / NAMCOT-3446: 2 switchable + 2 fixed 8KB PRG, all-2KB CHR */
		set_prg8(0, 0); set_prg8(1, 1);
		set_prg8(2, (prg16 * 2) - 2);
		set_prg8(3, (prg16 * 2) - 1);
		set_chr8(0);
		break;
	case 80:  /* Taito X1-005 */
	case 207: /* Taito X1-005 per-half CIRAM variant */
	case 82:  /* Taito X1-017 */
		set_prg8(0, 0); set_prg8(1, 1); set_prg8(2, 2);
		set_prg8(3, (prg16 * 2) - 1);    /* $E000 fixed last */
		set_chr8(0);
		break;
	case 210: /* Namco 175 (sub1) / 340 (sub2) */
		set_chr8(0);
		set_prg8(0, 0); set_prg8(1, 1); set_prg8(2, 2);
		set_prg8(3, (prg16 * 2) - 1);    /* $E000 fixed last */
		g.cart.p4_layout = (((rom[7] & 0x0C) == 0x08) && (rom[8] >> 4) == 2) ? 1 : 0;
		break;
	case 97: /* Irem TAM-S1: reversed UxROM ($8000 fixed last, $C000 switch) */
		set_prg16(0, prg16 - 1);
		set_prg16(1, 0);
		set_chr8(0);
		break;
	case 232: /* Camerica/Codemasters BF9096 */
		g.cart.p4_prg[0] = 0; g.cart.p4_prg[1] = 0;
		bf9096_apply();
		set_chr8(0);
		break;
	case 15: /* K-1029 multicart */
		set_prg16(0, 0); set_prg16(1, 1);
		set_chr8(0);
		break;
	case 79:  /* NINA-03/06 (AVE) */
	case 146: /* Sachen 3015 (identical to 79) */
	case 113: /* HES NINA variant (+ mirror) */
	case 36:  /* TXC 01-22000 */
		set_prg32(0);
		set_chr8(0);
		break;
	case 30:  /* UNROM-512: 16KB switch + fixed last, 32KB CHR-RAM */
		set_prg16(0, 0);
		set_prg16(1, prg16 - 1);
		set_chr8(0);
		if (g.cart.mirror == MIR_FOUR) {   /* header four-screen -> mapper 1-screen */
			g.cart.p4_layout = 1;
			g.cart.mirror = MIR_SINGLE0;
		}
		break;
	default:
		if (err) *err = "unsupported mapper (see MAPPER_PLAN.md / cart.c switch)";
		return EMU_ERR_NOCHIP;
	}

	if (err) *err = "ok";
	return 0;
}
