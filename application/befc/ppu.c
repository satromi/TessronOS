/*
 * ppu.c - NES PPU (2C02), scanline renderer with loopy v/t/x scroll registers.
 *
 * The whole visible scanline is rasterised in one shot at dot 0 using the
 * current scroll address (walking a local copy across the line for coarse-X),
 * which is accurate enough for mappers 0-4. Per-dot bookkeeping still drives
 * vblank/NMI timing, the loopy address increments, sprite-0-hit timing and the
 * MMC3 scanline IRQ. Output is written as 0x00RRGGBB into ppu.fb.
 */
#include <string.h>
#include "nesint.h"
#include "nespal.h"

#if defined(PPU_TRACE) || defined(VB_TRACE)   /* host-only trace (never on BTRON) */
#include <stdio.h>
#endif

#define RENDER_ON ((g.ppu.mask & 0x18) != 0)

/* 1 = hardware-accurate 8-sprites-per-line limit (+ overflow flag); 0 = draw all
 * sprites on a line (removes flicker). Default on = byte-identical to hardware. */
int ppu_sprite_limit = 1;

/* ------------------------------------------------------------ PPU bus --- */

u8 ppu_mem_read(u16 a)
{
	a &= 0x3FFF;
	if (a < 0x2000)
		return cart_chr_read_fast(a);
	if (a < 0x3F00) {
		if (a >= 0x3000) a -= 0x1000;
		return cart_nt_base(a)[a & 0x3FF];
	}
	{
		u8 idx = a & 0x1F;
		if ((idx & 0x13) == 0x10) idx &= ~0x10;  /* $3F10/14/18/1C mirror */
		return g.ppu.palette[idx];
	}
}

void ppu_mem_write(u16 a, u8 v)
{
	a &= 0x3FFF;
	if (a < 0x2000) {
		cart_chr_write(a, v);
		return;
	}
	if (a < 0x3F00) {
		if (a >= 0x3000) a -= 0x1000;
		if (g.cart.nt_rom && g.cart.nt_is_rom[(a >> 10) & 3])
			return;                       /* CHR-ROM nametable: ignore writes */
		if (g.cart.mapper == 5) { mmc5_nt_write(a, v); return; }
		g.ppu.vram[cart_nt_index(a)] = v;
		return;
	}
	{
		u8 idx = a & 0x1F;
		if ((idx & 0x13) == 0x10) idx &= ~0x10;
		g.ppu.palette[idx] = v & 0x3F;
	}
}

/* --------------------------------------------------------- registers --- */

u8 ppu_reg_read(u16 a)
{
	PPU *p = &g.ppu;
	switch (a & 7) {
	case 2: {
		u8 r = (u8)(p->status & 0xE0);
#ifdef VB_TRACE
		{ extern long vb_n; extern int vb_on;
		  if (vb_on && vb_n < 70) { fprintf(stderr, "  rd $2002 line=%3d dot=%3d ret=%02X\n", p->scanline, p->dot, (unsigned)r); vb_n++; } }
#endif
		p->status &= (u8)~0x80;   /* reading clears vblank */
		p->w = 0;
		return r;
	}
	case 4:
		return p->oam[p->oamaddr];
	case 7: {
		u16 addr = p->v & 0x3FFF;
		u8 r;
		if (addr >= 0x3F00) {
			r = ppu_mem_read(addr);
			p->readbuf = ppu_mem_read((u16)(addr - 0x1000));
		} else {
			r = p->readbuf;
			p->readbuf = ppu_mem_read(addr);
		}
		p->v += (p->ctrl & 0x04) ? 32 : 1;
		return r;
	}
	default:
		return 0;
	}
}

void ppu_reg_write(u16 a, u8 v)
{
	PPU *p = &g.ppu;
#ifdef PPU_TRACE
	{ extern int g_pputrace_on;
	  if (g_pputrace_on && ((a & 7) == 0 || (a & 7) == 1 || (a & 7) == 5 || (a & 7) == 6))
		printf("  $20%02X=%02X (line=%d dot=%d)\n", (a & 7), v, p->scanline, p->dot); }
#endif
	switch (a & 7) {
	case 0: {
		u8 old = p->ctrl;
		p->ctrl = v;
		p->t = (u16)((p->t & ~0x0C00) | ((v & 3) << 10));
		/* enabling NMI during vblank with the flag still set fires immediately */
		if ((v & 0x80) && !(old & 0x80) && (p->status & 0x80))
			cpu_trigger_nmi();
		break;
	}
	case 1:
		p->mask = v;
		break;
	case 3:
		p->oamaddr = v;
		break;
	case 4:
		p->oam[p->oamaddr++] = v;
		break;
	case 5:
		if (p->w == 0) {
			p->t = (u16)((p->t & ~0x001F) | (v >> 3));
			p->x = v & 7;
			p->w = 1;
		} else {
			p->t = (u16)((p->t & ~0x73E0) | ((v & 7) << 12) | ((v & 0xF8) << 2));
			p->w = 0;
		}
		break;
	case 6:
		if (p->w == 0) {
			p->t = (u16)((p->t & ~0xFF00) | ((v & 0x3F) << 8));
			p->w = 1;
		} else {
			p->t = (u16)((p->t & ~0x00FF) | v);
			p->v = p->t;
			p->w = 0;
#ifdef PPU_TRACE
			{ extern void ppu_w6_log(u16); ppu_w6_log(p->v); }
#endif
		}
		break;
	case 7:
#ifdef CPU_TRACE
		{ extern long dbg_w2007; dbg_w2007++; }
#endif
#ifdef PPU_TRACE
		{ extern void ppu_w7_log(u16, u8); ppu_w7_log(p->v, v); }   /* always count */
#endif
		ppu_mem_write(p->v, v);
		p->v += (p->ctrl & 0x04) ? 32 : 1;
		break;
	}
}

/* ---------------------------------------------------- loopy helpers --- */

static void inc_y(void)
{
	PPU *p = &g.ppu;
	if ((p->v & 0x7000) != 0x7000) {
		p->v += 0x1000;                       /* fine Y++ */
	} else {
		p->v &= ~0x7000;                      /* fine Y = 0 */
		{
			int y = (p->v & 0x03E0) >> 5;
			if (y == 29)      { y = 0;  p->v ^= 0x0800; }  /* next NT */
			else if (y == 31) { y = 0; }
			else              { y++; }
			p->v = (u16)((p->v & ~0x03E0) | (y << 5));
		}
	}
}

/* ---------------------------------------------------- scanline draw --- */

static void render_scanline(int line)
{
	PPU *p = &g.ppu;
	u32 *row = p->fb + (size_t)line * NES_W;
	u8  bg_op[NES_W];
	int i;
	u8  backdrop = (u8)(p->palette[0] & 0x3F);

	int show_bg   = (p->mask & 0x08) != 0;
	int show_sp   = (p->mask & 0x10) != 0;
	int left_bg   = (p->mask & 0x02) != 0;
	int left_sp   = (p->mask & 0x04) != 0;

	if (g.cart.mapper == 5) {             /* MMC5: per-scanline CHR sets + IRQ */
		mmc5_chr_resolve();
		if (p->mask & 0x18) mmc5_scanline_irq();
	}

	/* ---- background ----
	 * Decode 33 tiles into per-fine-x scratch, then resolve colour in a second
	 * pass (so fine-X scroll across the 33rd tile is handled). BOTH the nametable/
	 * attribute reads and the CHR pattern reads bypass the per-byte
	 * ppu_mem_read -> cart_* call nesting: cart_nt_index is linear within a
	 * nametable (phys*0x400 + (a&0x3FF)), so the nametable row and its attribute
	 * row are resolved to direct g.ppu.vram pointers ONCE per scanline and
	 * coarse-X just indexes them; the 0x400 coarse-X wrap recomputes the two
	 * pointers (equivalent to the old addr^=0x0400). The two CHR bytes lie in the
	 * same 1KB window so they read straight from g.cart.chr. Byte-identical to the
	 * old path; removes ~66 ppu_mem_read + ~66 cart_nt_index calls per scanline. */
	if (show_bg) {
		const u32 *npal = nes_palette;
		const u8  *cpal = p->palette;
		const u8  *chr  = g.cart.chr;
		const u32 *bgoff = (g.cart.mapper == 5) ? g.cart.m5_chr_off_bg : g.cart.chr_off;
		int   exattr = (g.cart.mapper == 5 && g.cart.m5_exram_mode == 1 && (p->mask & 0x18));
		u32   chrsz  = g.cart.chr_size ? g.cart.chr_size : 1024;
		u8  line_pix[264], line_hi2[264];
		u16 v0 = p->v;
		int fineY = (v0 >> 12) & 7;
		u16 patbase = (p->ctrl & 0x10) ? 0x1000 : 0x0000;
		int fineX = p->x;
		int col = 0, t;
		int cx    = v0 & 0x001F;                       /* coarse X within the NT */
		u16 ntsel = v0 & 0x0C00;                       /* nametable select bits  */
		u16 ntrow = v0 & 0x03E0;                       /* coarse-Y << 5 (fixed)  */
		int at_hi = (ntrow >> 4) & 4;                  /* coarse-Y attribute bit */
		const u8 *nt_row = cart_nt_base((u16)(0x2000 | ntsel | ntrow)) + ntrow;
		const u8 *at_row = cart_nt_base((u16)(0x23C0 | ntsel)) + 0x3C0 + ((ntrow >> 4) & 0x38);
		for (t = 0; t < 33; t++) {
			u8  tile = nt_row[cx];
			u8  ab   = at_row[cx >> 2];
			int shift = at_hi | (cx & 2);
			u8  hi2  = (u8)((ab >> shift) & 3);
			u16 pa   = (u16)(patbase + tile * 16 + fineY);
			u8  lo, hi;
			if (exattr) {                        /* MMC5 extended attributes  */
				u8  ex = g.cart.m5_exram[(ntrow | cx) & 0x3FF];
				u32 b4 = (((u32)(g.cart.m5_chr_upper & 3) << 6) | (ex & 0x3F)) << 12;
				u32 pl = ((u32)(tile * 16 + fineY)) & 0x0FFF;
				lo  = chr[(b4 + pl)     % chrsz];
				hi  = chr[(b4 + pl + 8) % chrsz];
				hi2 = (u8)((ex >> 6) & 3);        /* per-tile palette */
			} else {
				const u8 *cp = chr + bgoff[(pa >> 10) & 7];
				lo = cp[pa & 0x3FF];
				hi = cp[(pa + 8) & 0x3FF];
			}
			if (g.cart.chr_latch && (tile == 0xFD || tile == 0xFE))
				cart_chr_latch(patbase, tile);   /* MMC2/4 BG CHR latch */
			line_pix[col+0]=(u8)(((lo>>7)&1)|(((hi>>7)&1)<<1)); line_hi2[col+0]=hi2;
			line_pix[col+1]=(u8)(((lo>>6)&1)|(((hi>>6)&1)<<1)); line_hi2[col+1]=hi2;
			line_pix[col+2]=(u8)(((lo>>5)&1)|(((hi>>5)&1)<<1)); line_hi2[col+2]=hi2;
			line_pix[col+3]=(u8)(((lo>>4)&1)|(((hi>>4)&1)<<1)); line_hi2[col+3]=hi2;
			line_pix[col+4]=(u8)(((lo>>3)&1)|(((hi>>3)&1)<<1)); line_hi2[col+4]=hi2;
			line_pix[col+5]=(u8)(((lo>>2)&1)|(((hi>>2)&1)<<1)); line_hi2[col+5]=hi2;
			line_pix[col+6]=(u8)(((lo>>1)&1)|(((hi>>1)&1)<<1)); line_hi2[col+6]=hi2;
			line_pix[col+7]=(u8)(( lo    &1)|(( hi    &1)<<1)); line_hi2[col+7]=hi2;
			col += 8;
			if (cx == 31) {                            /* wrap to adjacent NT */
				cx = 0; ntsel ^= 0x0400;
				nt_row = cart_nt_base((u16)(0x2000 | ntsel | ntrow)) + ntrow;
				at_row = cart_nt_base((u16)(0x23C0 | ntsel)) + 0x3C0 + ((ntrow >> 4) & 0x38);
			} else {
				cx++;
			}
		}
		for (i = 0; i < NES_W; i++) {
			int k = i + fineX;
			u8  pv = line_pix[k];
			if (pv == 0 || (i < 8 && !left_bg)) {
				row[i] = npal[backdrop];
				bg_op[i] = 0;
			} else {
				row[i] = npal[cpal[line_hi2[k] * 4 + pv] & 0x3F];
				bg_op[i] = 1;
			}
		}
	} else {
		const u32 bd = nes_palette[backdrop];
		for (i = 0; i < NES_W; i++) { row[i] = bd; bg_op[i] = 0; }
	}

	/* ---- sprites ----
	 * Eval still scans all 64 OAM entries (overflow flag semantics) and caps 8
	 * per line; draw is back-to-front. Pattern bytes are read straight from
	 * g.cart.chr (same 1KB-window argument as the BG path) instead of via
	 * ppu_mem_read; oam/palette pointers are hoisted. Sprite-0-hit logic is
	 * byte-identical. Per-frame cost is FIXED w.r.t. on-screen object count. */
	if (show_sp) {
		const u32 *npal = nes_palette;
		const u8  *cpal = p->palette;
		const u8  *oam  = p->oam;
		int height = (p->ctrl & 0x20) ? 16 : 8;
		int visible[64];
		int nvis = 0;
		int s, c;
		for (s = 0; s < 64; s++) {
			int sy = oam[s * 4 + 0];
			int rr = line - sy - 1;   /* +1 hardware delay: sprite shows at Y+1 */
			if (rr < 0 || rr >= height) continue;
			if (ppu_sprite_limit) {            /* hardware: 8 sprites/line + overflow */
				if (nvis < 8) {
					visible[nvis++] = s;
				} else {
					p->status |= 0x20;     /* sprite overflow (simplified) */
					break;
				}
			} else if (nvis < 64) {            /* no-flicker: draw all on the line */
				visible[nvis++] = s;
			}
		}
		/* draw back-to-front so lower OAM index wins */
		for (c = nvis - 1; c >= 0; c--) {
			s = visible[c];
			{
				int sy   = oam[s * 4 + 0];
				u8  tile = oam[s * 4 + 1];
				u8  attr = oam[s * 4 + 2];
				int sx   = oam[s * 4 + 3];
				int rr   = line - sy - 1;   /* +1 hardware delay (see eval loop) */
				int hflip = attr & 0x40;
				int vflip = attr & 0x80;
				int behind = attr & 0x20;
				int palsel = attr & 3;
				u16 base;
				const u8 *cp;
				u8  lo, hi;
				int col;

				if (vflip) rr = height - 1 - rr;
				if (height == 16) {
					u16 pb = (tile & 1) ? 0x1000 : 0x0000;
					u8  ti = (u8)(tile & 0xFE);
					if (rr >= 8) { ti++; rr -= 8; }
					base = (u16)(pb + ti * 16 + rr);
				} else {
					u16 pb = (p->ctrl & 0x08) ? 0x1000 : 0x0000;
					base = (u16)(pb + tile * 16 + rr);
				}
				cp = g.cart.chr + g.cart.chr_off[(base >> 10) & 7];
				lo = cp[base & 0x3FF];
				hi = cp[(base + 8) & 0x3FF];

				for (col = 0; col < 8; col++) {
					int bit = hflip ? col : (7 - col);
					u8  pv = (u8)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
					int x = sx + col;
					if (pv == 0) continue;
					if (x >= NES_W) continue;
					if (x < 8 && !left_sp) continue;
					/* sprite-0 hit: opaque sprite0 over opaque bg, x!=255 */
					if (s == 0 && bg_op[x] && x != 255 && show_bg &&
					    !(p->status & 0x40) && p->s0_hit_dot < 0)
						p->s0_hit_dot = x + 1;
					if (behind && bg_op[x]) continue;
					row[x] = npal[cpal[0x10 + palsel * 4 + pv] & 0x3F];
				}
			}
		}
	}
}

/* -------------------------------------------------------- per-dot --- */

/* Cold slow path: reached (via ppu_tick_inline in nesint.h) only on dots that
 * may do real work — dot 0 (render), dot>=256 (loopy/MMC3/odd-skip), the vblank
 * line 241, the pre-render line 261, or when a sprite-0 hit is armed. The body
 * is the ORIGINAL ppu_tick() verbatim, so it stays correct even if entered on a
 * dot that turns out to have nothing to do. */
void ppu_tick_slow(void)
{
	PPU *p = &g.ppu;
	int line = p->scanline;
	int dot  = p->dot;

	/* deferred sprite-0 hit within a visible line */
	if (p->s0_hit_dot >= 0 && line >= 0 && line < 240 && dot == p->s0_hit_dot) {
		p->status |= 0x40;
		p->s0_hit_dot = -1;
	}

	if (line < 240) {                         /* visible scanlines */
		if (dot == 0) {
			p->s0_hit_dot = -1;
			render_scanline(line);
		}
		if (RENDER_ON) {
			if (dot == 256) inc_y();
			if (dot == 257) p->v = (u16)((p->v & ~0x041F) | (p->t & 0x041F));
			if (dot == 260) cart_ppu_scanline();
		}
	} else if (line == 241) {
		if (dot == 1) {
			p->status |= 0x80;                /* vblank set */
			if (p->ctrl & 0x80) cpu_trigger_nmi();
			p->frame_ready = 1;
			p->frame++;
			if (g.cart.mapper == 5) mmc5_end_frame();   /* leave active region */
		}
	} else if (line == 261) {                 /* pre-render */
		if (dot == 1)
			p->status &= (u8)~(0x80 | 0x40 | 0x20);
		if (RENDER_ON) {
			if (dot == 256) inc_y();
			if (dot == 257) p->v = (u16)((p->v & ~0x041F) | (p->t & 0x041F));
			if (dot == 260) cart_ppu_scanline();
			if (dot >= 280 && dot <= 304)
				p->v = (u16)((p->v & ~0x7BE0) | (p->t & 0x7BE0));
		}
	}

	/* advance dot/scanline with odd-frame cycle skip */
	p->dot++;
	if (line == 261 && p->dot == 340 && p->odd && RENDER_ON)
		p->dot++;                             /* skip last dot on odd frames */
	if (p->dot > 340) {
		p->dot = 0;
		p->scanline++;
		if (p->scanline > 261) {
			p->scanline = 0;
			p->odd ^= 1;
		}
	}
}

/* ----------------------------------------------------- power/reset --- */

void ppu_power(void)
{
	memset(&g.ppu.oam, 0, sizeof(g.ppu.oam));
	memset(&g.ppu.vram, 0, sizeof(g.ppu.vram));
	memset(&g.ppu.palette, 0, sizeof(g.ppu.palette));
	memset(&g.ppu.fb, 0, sizeof(g.ppu.fb));
	g.ppu.ctrl = g.ppu.mask = g.ppu.status = g.ppu.oamaddr = 0;
	g.ppu.v = g.ppu.t = 0;
	g.ppu.x = g.ppu.w = 0;
	g.ppu.readbuf = 0;
	g.ppu.scanline = 261;
	g.ppu.dot = 0;
	g.ppu.frame = 0;
	g.ppu.frame_ready = 0;
	g.ppu.odd = 0;
	g.ppu.s0_hit_dot = -1;
}

void ppu_reset(void)
{
	g.ppu.ctrl = g.ppu.mask = 0;
	g.ppu.w = 0;
	g.ppu.scanline = 261;
	g.ppu.dot = 0;
	g.ppu.frame_ready = 0;
	g.ppu.odd = 0;
	g.ppu.s0_hit_dot = -1;
}
