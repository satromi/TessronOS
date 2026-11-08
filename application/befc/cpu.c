/*
 * cpu.c - MOS 6502 / Ricoh 2A03 core (no decimal mode, as on the NES).
 *
 * Implements all official opcodes plus the stable illegal opcodes commonly
 * relied upon by NES software (LAX, SAX, DCP, ISC, SLO, RLA, SRE, RRA, ANC,
 * ALR, ARR, AXS and the multi-byte NOPs). Cycle counts include page-cross and
 * branch penalties. Interrupts: edge-triggered NMI, level IRQ (gated by I).
 */
#include "nesint.h"
#if defined(VB_TRACE)
#include <stdio.h>
#endif

#ifdef CPU_TRACE
/* Host-only trace counters. ALL CPU_TRACE globals are defined here (one TU) so a
 * CPU_TRACE build links: the increments live in cpu.c/apu.c/ppu.c/nes.c as
 * `extern long ...; ...++;` and previously only dbg_nmi/dbg_irq had a definition,
 * so enabling CPU_TRACE failed at link on dbg_badop/dbg_w4017/dbg_w2007/dbg_oamdma. */
long dbg_nmi = 0, dbg_irq = 0;   /* interrupt counters                    */
long dbg_badop = 0;              /* illegal/undocumented opcodes hit       */
int  dbg_badop_code = 0;         /* last such opcode                       */
long dbg_w4017 = 0;              /* $4017 (APU frame counter) writes        */
long dbg_w2007 = 0;              /* $2007 (PPU data) writes                 */
long dbg_oamdma = 0;             /* $4014 OAM-DMA transfers                 */
#endif

static int cyc;     /* base cycles for the instruction in flight */
static int extra;   /* conditional page-cross / branch-taken penalty (added last
                     * so the per-opcode `cyc = N` base assignment cannot clobber
                     * it) */

/* ---- memory helpers ---- */
static u16 rd16(u16 a)
{
	return (u16)(cpu_read(a) | (cpu_read((u16)(a + 1)) << 8));
}
static u16 rd16pc(void)
{
	u16 v = (u16)(cpu_read(g.cpu.pc) | (cpu_read((u16)(g.cpu.pc + 1)) << 8));
	g.cpu.pc += 2;
	return v;
}
static void push(u8 v)   { cpu_write((u16)(0x100 + g.cpu.s), v); g.cpu.s--; }
static u8   pull(void)   { g.cpu.s++; return cpu_read((u16)(0x100 + g.cpu.s)); }
static void push16(u16 v){ push((u8)(v >> 8)); push((u8)(v & 0xFF)); }
static u16  pull16(void) { u16 lo = pull(); u16 hi = pull(); return (u16)(lo | (hi << 8)); }

static void set_zn(u8 v)
{
	g.cpu.p = (u8)((g.cpu.p & ~(FLAG_Z | FLAG_N)) | (v == 0 ? FLAG_Z : 0) | (v & 0x80));
}
static void set_flag(u8 f, int on)
{
	if (on) g.cpu.p |= f; else g.cpu.p &= (u8)~f;
}

/* ---- addressing modes (return effective address; bump cyc on page cross
 *      only when `pen` is set, i.e. for the read-group instructions) ---- */
static u16 a_zp (void)      { return cpu_read(g.cpu.pc++); }
static u16 a_zpx(void)      { return (u16)((cpu_read(g.cpu.pc++) + g.cpu.x) & 0xFF); }
static u16 a_zpy(void)      { return (u16)((cpu_read(g.cpu.pc++) + g.cpu.y) & 0xFF); }
static u16 a_abs(void)      { return rd16pc(); }
static u16 a_abx(int pen)   { u16 b = rd16pc(); u16 a = (u16)(b + g.cpu.x); if (pen && ((b ^ a) & 0x100)) extra++; return a; }
static u16 a_aby(int pen)   { u16 b = rd16pc(); u16 a = (u16)(b + g.cpu.y); if (pen && ((b ^ a) & 0x100)) extra++; return a; }
static u16 a_izx(void)
{
	u8 z = (u8)(cpu_read(g.cpu.pc++) + g.cpu.x);
	return (u16)(cpu_read(z) | (cpu_read((u8)(z + 1)) << 8));
}
static u16 a_izy(int pen)
{
	u8 z = cpu_read(g.cpu.pc++);
	u16 b = (u16)(cpu_read(z) | (cpu_read((u8)(z + 1)) << 8));
	u16 a = (u16)(b + g.cpu.y);
	if (pen && ((b ^ a) & 0x100)) extra++;
	return a;
}

/* ---- ALU helpers ---- */
static void adc(u8 m)
{
	unsigned a = g.cpu.a, c = (g.cpu.p & FLAG_C) ? 1u : 0u;
	unsigned r = a + m + c;
	set_flag(FLAG_C, r > 0xFF);
	set_flag(FLAG_V, (~(a ^ m) & (a ^ r) & 0x80) != 0);
	g.cpu.a = (u8)r;
	set_zn(g.cpu.a);
}
static void sbc(u8 m) { adc((u8)(m ^ 0xFF)); }
static void cmp_(u8 reg, u8 m)
{
	u16 t = (u16)(reg - m);
	set_flag(FLAG_C, reg >= m);
	set_zn((u8)t);
}
static u8 op_asl(u8 v) { set_flag(FLAG_C, v & 0x80); v = (u8)(v << 1); set_zn(v); return v; }
static u8 op_lsr(u8 v) { set_flag(FLAG_C, v & 0x01); v = (u8)(v >> 1); set_zn(v); return v; }
static u8 op_rol(u8 v) { u8 c = (g.cpu.p & FLAG_C) ? 1 : 0; set_flag(FLAG_C, v & 0x80); v = (u8)((v << 1) | c); set_zn(v); return v; }
static u8 op_ror(u8 v) { u8 c = (g.cpu.p & FLAG_C) ? 0x80 : 0; set_flag(FLAG_C, v & 0x01); v = (u8)((v >> 1) | c); set_zn(v); return v; }

static void branch(int cond)
{
	s8 off = (s8)cpu_read(g.cpu.pc++);
	if (cond) {
		u16 old = g.cpu.pc;
		g.cpu.pc = (u16)(old + off);
		extra++;
		if ((old ^ g.cpu.pc) & 0x100) extra++;
	}
}

/* ---- interrupts ---- */
static void interrupt(u16 vector, int brk)
{
	push16(g.cpu.pc);
	push((u8)((g.cpu.p & ~FLAG_B) | FLAG_U | (brk ? FLAG_B : 0)));
	g.cpu.p |= FLAG_I;
	g.cpu.pc = rd16(vector);
}

void cpu_trigger_nmi(void)
{
	if (!g.cpu.nmi_pending) {
		g.cpu.nmi_pending = 1;
		g.cpu.nmi_delay = 1;   /* service after one more instruction runs */
	}
}

void cpu_power(void)
{
	g.cpu.a = g.cpu.x = g.cpu.y = 0;
	g.cpu.s = 0xFD;
	g.cpu.p = FLAG_I | FLAG_U;
	g.cpu.cycles = 0;
	g.cpu.stall = 0;
	g.cpu.nmi_pending = 0;
	g.cpu.nmi_delay = 0;
	g.cpu.irq_line = 0;
	g.cpu.pc = rd16(0xFFFC);
}

void cpu_reset(void)
{
	g.cpu.s -= 3;
	g.cpu.p |= FLAG_I;
	g.cpu.pc = rd16(0xFFFC);
	g.cpu.nmi_pending = 0;
}

int cpu_step(void)
{
	u8 op, m;
	u16 ad;
	long start = g.cpu.cycles;     /* cpu_cycle() (via every bus access) bumps
	                                * g.cpu.cycles; we top up internal cycles at
	                                * the end so the PPU advances exactly `cyc`. */

	if (g.cpu.nmi_pending && g.cpu.nmi_delay > 0) {
		/* NMI is pending but its 1-instruction latency hasn't elapsed: let this
		 * instruction (e.g. the poll loop's `BIT $2002`) run first. */
		g.cpu.nmi_delay--;
	} else if (g.cpu.nmi_pending) {
		g.cpu.nmi_pending = 0;
#ifdef CPU_TRACE
		dbg_nmi++;
#endif
#ifdef VB_TRACE
		{ extern long vb_n; extern int vb_on;
		  if (vb_on && vb_n < 70) { fprintf(stderr, ">>> NMI taken (ppu line=%d dot=%d)\n", g.ppu.scanline, g.ppu.dot); vb_n++; } }
#endif
		interrupt(0xFFFA, 0);
		while (g.cpu.cycles - start < 7) cpu_cycle();
		return 7;
	}
	if (g.cpu.irq_line && !(g.cpu.p & FLAG_I)) {
#ifdef CPU_TRACE
		dbg_irq++;
#endif
		interrupt(0xFFFE, 0);
		while (g.cpu.cycles - start < 7) cpu_cycle();
		return 7;
	}

#ifdef CPU_PC_HOOK
	{ extern void cpu_pc_hook(u16, u8, u8, u8, u8, u8);
	  cpu_pc_hook(g.cpu.pc, g.cpu.a, g.cpu.x, g.cpu.y, g.cpu.p, g.cpu.s); }
#endif
	op = cpu_read(g.cpu.pc++);
	cyc = 0;
	extra = 0;

	switch (op) {
	/* ---- ORA ---- */
	case 0x09: g.cpu.a |= cpu_read(g.cpu.pc++); set_zn(g.cpu.a); cyc = 2; break;
	case 0x05: g.cpu.a |= cpu_read(a_zp());  set_zn(g.cpu.a); cyc = 3; break;
	case 0x15: g.cpu.a |= cpu_read(a_zpx()); set_zn(g.cpu.a); cyc = 4; break;
	case 0x0D: g.cpu.a |= cpu_read(a_abs()); set_zn(g.cpu.a); cyc = 4; break;
	case 0x1D: g.cpu.a |= cpu_read(a_abx(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0x19: g.cpu.a |= cpu_read(a_aby(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0x01: g.cpu.a |= cpu_read(a_izx()); set_zn(g.cpu.a); cyc = 6; break;
	case 0x11: g.cpu.a |= cpu_read(a_izy(1)); set_zn(g.cpu.a); cyc = 5; break;
	/* ---- AND ---- */
	case 0x29: g.cpu.a &= cpu_read(g.cpu.pc++); set_zn(g.cpu.a); cyc = 2; break;
	case 0x25: g.cpu.a &= cpu_read(a_zp());  set_zn(g.cpu.a); cyc = 3; break;
	case 0x35: g.cpu.a &= cpu_read(a_zpx()); set_zn(g.cpu.a); cyc = 4; break;
	case 0x2D: g.cpu.a &= cpu_read(a_abs()); set_zn(g.cpu.a); cyc = 4; break;
	case 0x3D: g.cpu.a &= cpu_read(a_abx(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0x39: g.cpu.a &= cpu_read(a_aby(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0x21: g.cpu.a &= cpu_read(a_izx()); set_zn(g.cpu.a); cyc = 6; break;
	case 0x31: g.cpu.a &= cpu_read(a_izy(1)); set_zn(g.cpu.a); cyc = 5; break;
	/* ---- EOR ---- */
	case 0x49: g.cpu.a ^= cpu_read(g.cpu.pc++); set_zn(g.cpu.a); cyc = 2; break;
	case 0x45: g.cpu.a ^= cpu_read(a_zp());  set_zn(g.cpu.a); cyc = 3; break;
	case 0x55: g.cpu.a ^= cpu_read(a_zpx()); set_zn(g.cpu.a); cyc = 4; break;
	case 0x4D: g.cpu.a ^= cpu_read(a_abs()); set_zn(g.cpu.a); cyc = 4; break;
	case 0x5D: g.cpu.a ^= cpu_read(a_abx(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0x59: g.cpu.a ^= cpu_read(a_aby(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0x41: g.cpu.a ^= cpu_read(a_izx()); set_zn(g.cpu.a); cyc = 6; break;
	case 0x51: g.cpu.a ^= cpu_read(a_izy(1)); set_zn(g.cpu.a); cyc = 5; break;
	/* ---- ADC ---- */
	case 0x69: adc(cpu_read(g.cpu.pc++)); cyc = 2; break;
	case 0x65: adc(cpu_read(a_zp()));  cyc = 3; break;
	case 0x75: adc(cpu_read(a_zpx())); cyc = 4; break;
	case 0x6D: adc(cpu_read(a_abs())); cyc = 4; break;
	case 0x7D: adc(cpu_read(a_abx(1))); cyc = 4; break;
	case 0x79: adc(cpu_read(a_aby(1))); cyc = 4; break;
	case 0x61: adc(cpu_read(a_izx())); cyc = 6; break;
	case 0x71: adc(cpu_read(a_izy(1))); cyc = 5; break;
	/* ---- SBC ---- */
	case 0xE9: case 0xEB: sbc(cpu_read(g.cpu.pc++)); cyc = 2; break;
	case 0xE5: sbc(cpu_read(a_zp()));  cyc = 3; break;
	case 0xF5: sbc(cpu_read(a_zpx())); cyc = 4; break;
	case 0xED: sbc(cpu_read(a_abs())); cyc = 4; break;
	case 0xFD: sbc(cpu_read(a_abx(1))); cyc = 4; break;
	case 0xF9: sbc(cpu_read(a_aby(1))); cyc = 4; break;
	case 0xE1: sbc(cpu_read(a_izx())); cyc = 6; break;
	case 0xF1: sbc(cpu_read(a_izy(1))); cyc = 5; break;
	/* ---- CMP ---- */
	case 0xC9: cmp_(g.cpu.a, cpu_read(g.cpu.pc++)); cyc = 2; break;
	case 0xC5: cmp_(g.cpu.a, cpu_read(a_zp()));  cyc = 3; break;
	case 0xD5: cmp_(g.cpu.a, cpu_read(a_zpx())); cyc = 4; break;
	case 0xCD: cmp_(g.cpu.a, cpu_read(a_abs())); cyc = 4; break;
	case 0xDD: cmp_(g.cpu.a, cpu_read(a_abx(1))); cyc = 4; break;
	case 0xD9: cmp_(g.cpu.a, cpu_read(a_aby(1))); cyc = 4; break;
	case 0xC1: cmp_(g.cpu.a, cpu_read(a_izx())); cyc = 6; break;
	case 0xD1: cmp_(g.cpu.a, cpu_read(a_izy(1))); cyc = 5; break;
	/* ---- CPX / CPY ---- */
	case 0xE0: cmp_(g.cpu.x, cpu_read(g.cpu.pc++)); cyc = 2; break;
	case 0xE4: cmp_(g.cpu.x, cpu_read(a_zp()));  cyc = 3; break;
	case 0xEC: cmp_(g.cpu.x, cpu_read(a_abs())); cyc = 4; break;
	case 0xC0: cmp_(g.cpu.y, cpu_read(g.cpu.pc++)); cyc = 2; break;
	case 0xC4: cmp_(g.cpu.y, cpu_read(a_zp()));  cyc = 3; break;
	case 0xCC: cmp_(g.cpu.y, cpu_read(a_abs())); cyc = 4; break;
	/* ---- LDA ---- */
	case 0xA9: g.cpu.a = cpu_read(g.cpu.pc++); set_zn(g.cpu.a); cyc = 2; break;
	case 0xA5: g.cpu.a = cpu_read(a_zp());  set_zn(g.cpu.a); cyc = 3; break;
	case 0xB5: g.cpu.a = cpu_read(a_zpx()); set_zn(g.cpu.a); cyc = 4; break;
	case 0xAD: g.cpu.a = cpu_read(a_abs()); set_zn(g.cpu.a); cyc = 4; break;
	case 0xBD: g.cpu.a = cpu_read(a_abx(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0xB9: g.cpu.a = cpu_read(a_aby(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0xA1: g.cpu.a = cpu_read(a_izx()); set_zn(g.cpu.a); cyc = 6; break;
	case 0xB1: g.cpu.a = cpu_read(a_izy(1)); set_zn(g.cpu.a); cyc = 5; break;
	/* ---- LDX ---- */
	case 0xA2: g.cpu.x = cpu_read(g.cpu.pc++); set_zn(g.cpu.x); cyc = 2; break;
	case 0xA6: g.cpu.x = cpu_read(a_zp());  set_zn(g.cpu.x); cyc = 3; break;
	case 0xB6: g.cpu.x = cpu_read(a_zpy()); set_zn(g.cpu.x); cyc = 4; break;
	case 0xAE: g.cpu.x = cpu_read(a_abs()); set_zn(g.cpu.x); cyc = 4; break;
	case 0xBE: g.cpu.x = cpu_read(a_aby(1)); set_zn(g.cpu.x); cyc = 4; break;
	/* ---- LDY ---- */
	case 0xA0: g.cpu.y = cpu_read(g.cpu.pc++); set_zn(g.cpu.y); cyc = 2; break;
	case 0xA4: g.cpu.y = cpu_read(a_zp());  set_zn(g.cpu.y); cyc = 3; break;
	case 0xB4: g.cpu.y = cpu_read(a_zpx()); set_zn(g.cpu.y); cyc = 4; break;
	case 0xAC: g.cpu.y = cpu_read(a_abs()); set_zn(g.cpu.y); cyc = 4; break;
	case 0xBC: g.cpu.y = cpu_read(a_abx(1)); set_zn(g.cpu.y); cyc = 4; break;
	/* ---- STA ---- */
	case 0x85: cpu_write(a_zp(),  g.cpu.a); cyc = 3; break;
	case 0x95: cpu_write(a_zpx(), g.cpu.a); cyc = 4; break;
	case 0x8D: cpu_write(a_abs(), g.cpu.a); cyc = 4; break;
	case 0x9D: cpu_write(a_abx(0), g.cpu.a); cyc = 5; break;
	case 0x99: cpu_write(a_aby(0), g.cpu.a); cyc = 5; break;
	case 0x81: cpu_write(a_izx(), g.cpu.a); cyc = 6; break;
	case 0x91: cpu_write(a_izy(0), g.cpu.a); cyc = 6; break;
	/* ---- STX / STY ---- */
	case 0x86: cpu_write(a_zp(),  g.cpu.x); cyc = 3; break;
	case 0x96: cpu_write(a_zpy(), g.cpu.x); cyc = 4; break;
	case 0x8E: cpu_write(a_abs(), g.cpu.x); cyc = 4; break;
	case 0x84: cpu_write(a_zp(),  g.cpu.y); cyc = 3; break;
	case 0x94: cpu_write(a_zpx(), g.cpu.y); cyc = 4; break;
	case 0x8C: cpu_write(a_abs(), g.cpu.y); cyc = 4; break;
	/* ---- transfers ---- */
	case 0xAA: g.cpu.x = g.cpu.a; set_zn(g.cpu.x); cyc = 2; break; /* TAX */
	case 0xA8: g.cpu.y = g.cpu.a; set_zn(g.cpu.y); cyc = 2; break; /* TAY */
	case 0xBA: g.cpu.x = g.cpu.s; set_zn(g.cpu.x); cyc = 2; break; /* TSX */
	case 0x8A: g.cpu.a = g.cpu.x; set_zn(g.cpu.a); cyc = 2; break; /* TXA */
	case 0x9A: g.cpu.s = g.cpu.x; cyc = 2; break;                  /* TXS */
	case 0x98: g.cpu.a = g.cpu.y; set_zn(g.cpu.a); cyc = 2; break; /* TYA */
	/* ---- stack ---- */
	case 0x48: push(g.cpu.a); cyc = 3; break;                       /* PHA */
	case 0x08: push((u8)(g.cpu.p | FLAG_B | FLAG_U)); cyc = 3; break;/* PHP */
	case 0x68: g.cpu.a = pull(); set_zn(g.cpu.a); cyc = 4; break;    /* PLA */
	case 0x28: g.cpu.p = (u8)((pull() & ~FLAG_B) | FLAG_U); cyc = 4; break; /* PLP */
	/* ---- INC / DEC memory ---- */
	case 0xE6: ad = a_zp();   m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); set_zn(m); cyc = 5; break;
	case 0xF6: ad = a_zpx();  m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); set_zn(m); cyc = 6; break;
	case 0xEE: ad = a_abs();  m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); set_zn(m); cyc = 6; break;
	case 0xFE: ad = a_abx(0); m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); set_zn(m); cyc = 7; break;
	case 0xC6: ad = a_zp();   m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); set_zn(m); cyc = 5; break;
	case 0xD6: ad = a_zpx();  m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); set_zn(m); cyc = 6; break;
	case 0xCE: ad = a_abs();  m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); set_zn(m); cyc = 6; break;
	case 0xDE: ad = a_abx(0); m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); set_zn(m); cyc = 7; break;
	case 0xE8: g.cpu.x++; set_zn(g.cpu.x); cyc = 2; break;          /* INX */
	case 0xC8: g.cpu.y++; set_zn(g.cpu.y); cyc = 2; break;          /* INY */
	case 0xCA: g.cpu.x--; set_zn(g.cpu.x); cyc = 2; break;          /* DEX */
	case 0x88: g.cpu.y--; set_zn(g.cpu.y); cyc = 2; break;          /* DEY */
	/* ---- shifts/rotates accumulator ---- */
	case 0x0A: g.cpu.a = op_asl(g.cpu.a); cyc = 2; break;
	case 0x4A: g.cpu.a = op_lsr(g.cpu.a); cyc = 2; break;
	case 0x2A: g.cpu.a = op_rol(g.cpu.a); cyc = 2; break;
	case 0x6A: g.cpu.a = op_ror(g.cpu.a); cyc = 2; break;
	/* ---- ASL memory ---- */
	case 0x06: ad = a_zp();   cpu_write(ad, op_asl(cpu_read(ad))); cyc = 5; break;
	case 0x16: ad = a_zpx();  cpu_write(ad, op_asl(cpu_read(ad))); cyc = 6; break;
	case 0x0E: ad = a_abs();  cpu_write(ad, op_asl(cpu_read(ad))); cyc = 6; break;
	case 0x1E: ad = a_abx(0); cpu_write(ad, op_asl(cpu_read(ad))); cyc = 7; break;
	/* ---- LSR memory ---- */
	case 0x46: ad = a_zp();   cpu_write(ad, op_lsr(cpu_read(ad))); cyc = 5; break;
	case 0x56: ad = a_zpx();  cpu_write(ad, op_lsr(cpu_read(ad))); cyc = 6; break;
	case 0x4E: ad = a_abs();  cpu_write(ad, op_lsr(cpu_read(ad))); cyc = 6; break;
	case 0x5E: ad = a_abx(0); cpu_write(ad, op_lsr(cpu_read(ad))); cyc = 7; break;
	/* ---- ROL memory ---- */
	case 0x26: ad = a_zp();   cpu_write(ad, op_rol(cpu_read(ad))); cyc = 5; break;
	case 0x36: ad = a_zpx();  cpu_write(ad, op_rol(cpu_read(ad))); cyc = 6; break;
	case 0x2E: ad = a_abs();  cpu_write(ad, op_rol(cpu_read(ad))); cyc = 6; break;
	case 0x3E: ad = a_abx(0); cpu_write(ad, op_rol(cpu_read(ad))); cyc = 7; break;
	/* ---- ROR memory ---- */
	case 0x66: ad = a_zp();   cpu_write(ad, op_ror(cpu_read(ad))); cyc = 5; break;
	case 0x76: ad = a_zpx();  cpu_write(ad, op_ror(cpu_read(ad))); cyc = 6; break;
	case 0x6E: ad = a_abs();  cpu_write(ad, op_ror(cpu_read(ad))); cyc = 6; break;
	case 0x7E: ad = a_abx(0); cpu_write(ad, op_ror(cpu_read(ad))); cyc = 7; break;
	/* ---- BIT ---- */
	case 0x24: m = cpu_read(a_zp());  set_flag(FLAG_Z, (g.cpu.a & m) == 0); g.cpu.p = (u8)((g.cpu.p & ~(FLAG_N | FLAG_V)) | (m & 0xC0)); cyc = 3; break;
	case 0x2C: m = cpu_read(a_abs()); set_flag(FLAG_Z, (g.cpu.a & m) == 0); g.cpu.p = (u8)((g.cpu.p & ~(FLAG_N | FLAG_V)) | (m & 0xC0)); cyc = 4; break;
	/* ---- branches ---- */
	case 0x10: branch(!(g.cpu.p & FLAG_N)); cyc = 2; break; /* BPL */
	case 0x30: branch( (g.cpu.p & FLAG_N)); cyc = 2; break; /* BMI */
	case 0x50: branch(!(g.cpu.p & FLAG_V)); cyc = 2; break; /* BVC */
	case 0x70: branch( (g.cpu.p & FLAG_V)); cyc = 2; break; /* BVS */
	case 0x90: branch(!(g.cpu.p & FLAG_C)); cyc = 2; break; /* BCC */
	case 0xB0: branch( (g.cpu.p & FLAG_C)); cyc = 2; break; /* BCS */
	case 0xD0: branch(!(g.cpu.p & FLAG_Z)); cyc = 2; break; /* BNE */
	case 0xF0: branch( (g.cpu.p & FLAG_Z)); cyc = 2; break; /* BEQ */
	/* ---- jumps / calls ---- */
	case 0x4C: g.cpu.pc = rd16pc(); cyc = 3; break;          /* JMP abs */
	case 0x6C: {                                             /* JMP (ind) bug */
		u16 ptr = rd16pc();
		u16 lo = cpu_read(ptr);
		u16 hi = cpu_read((u16)((ptr & 0xFF00) | ((ptr + 1) & 0xFF)));
		g.cpu.pc = (u16)(lo | (hi << 8));
		cyc = 5;
		break;
	}
	case 0x20: ad = rd16pc(); push16((u16)(g.cpu.pc - 1)); g.cpu.pc = ad; cyc = 6; break; /* JSR */
	case 0x60: g.cpu.pc = (u16)(pull16() + 1); cyc = 6; break; /* RTS */
	case 0x40: g.cpu.p = (u8)((pull() & ~FLAG_B) | FLAG_U); g.cpu.pc = pull16(); cyc = 6; break; /* RTI */
	case 0x00: /* BRK */
		push16((u16)(g.cpu.pc + 1));
		push((u8)(g.cpu.p | FLAG_B | FLAG_U));
		g.cpu.p |= FLAG_I;
		g.cpu.pc = rd16(0xFFFE);
		cyc = 7;
		break;
	/* ---- flags ---- */
	case 0x18: g.cpu.p &= (u8)~FLAG_C; cyc = 2; break; /* CLC */
	case 0x38: g.cpu.p |=  FLAG_C; cyc = 2; break;     /* SEC */
	case 0x58: g.cpu.p &= (u8)~FLAG_I; cyc = 2; break; /* CLI */
	case 0x78: g.cpu.p |=  FLAG_I; cyc = 2; break;     /* SEI */
	case 0xB8: g.cpu.p &= (u8)~FLAG_V; cyc = 2; break; /* CLV */
	case 0xD8: g.cpu.p &= (u8)~FLAG_D; cyc = 2; break; /* CLD */
	case 0xF8: g.cpu.p |=  FLAG_D; cyc = 2; break;     /* SED */
	/* ---- NOP (official) ---- */
	case 0xEA: cyc = 2; break;

	/* ============ illegal / unofficial ============ */
	/* multi-byte NOPs (consume operands; page-cross penalty for abs,X reads) */
	case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: cyc = 2; break;
	case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: g.cpu.pc++; cyc = 2; break; /* NOP #imm */
	case 0x04: case 0x44: case 0x64: a_zp();  cyc = 3; break;          /* NOP zp */
	case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: a_zpx(); cyc = 4; break; /* NOP zpx */
	case 0x0C: a_abs(); cyc = 4; break;                               /* NOP abs */
	case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: a_abx(1); cyc = 4; break; /* NOP abs,X */

	/* LAX (LDA+LDX) */
	case 0xA7: g.cpu.a = g.cpu.x = cpu_read(a_zp());  set_zn(g.cpu.a); cyc = 3; break;
	case 0xB7: g.cpu.a = g.cpu.x = cpu_read(a_zpy()); set_zn(g.cpu.a); cyc = 4; break;
	case 0xAF: g.cpu.a = g.cpu.x = cpu_read(a_abs()); set_zn(g.cpu.a); cyc = 4; break;
	case 0xBF: g.cpu.a = g.cpu.x = cpu_read(a_aby(1)); set_zn(g.cpu.a); cyc = 4; break;
	case 0xA3: g.cpu.a = g.cpu.x = cpu_read(a_izx()); set_zn(g.cpu.a); cyc = 6; break;
	case 0xB3: g.cpu.a = g.cpu.x = cpu_read(a_izy(1)); set_zn(g.cpu.a); cyc = 5; break;
	case 0xAB: g.cpu.a = g.cpu.x = cpu_read(g.cpu.pc++); set_zn(g.cpu.a); cyc = 2; break;
	/* SAX (store A&X) */
	case 0x87: cpu_write(a_zp(),  (u8)(g.cpu.a & g.cpu.x)); cyc = 3; break;
	case 0x97: cpu_write(a_zpy(), (u8)(g.cpu.a & g.cpu.x)); cyc = 4; break;
	case 0x8F: cpu_write(a_abs(), (u8)(g.cpu.a & g.cpu.x)); cyc = 4; break;
	case 0x83: cpu_write(a_izx(), (u8)(g.cpu.a & g.cpu.x)); cyc = 6; break;
	/* DCP (DEC + CMP) */
	case 0xC7: ad = a_zp();   m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); cmp_(g.cpu.a, m); cyc = 5; break;
	case 0xD7: ad = a_zpx();  m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); cmp_(g.cpu.a, m); cyc = 6; break;
	case 0xCF: ad = a_abs();  m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); cmp_(g.cpu.a, m); cyc = 6; break;
	case 0xDF: ad = a_abx(0); m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); cmp_(g.cpu.a, m); cyc = 7; break;
	case 0xDB: ad = a_aby(0); m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); cmp_(g.cpu.a, m); cyc = 7; break;
	case 0xC3: ad = a_izx();  m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); cmp_(g.cpu.a, m); cyc = 8; break;
	case 0xD3: ad = a_izy(0); m = (u8)(cpu_read(ad) - 1); cpu_write(ad, m); cmp_(g.cpu.a, m); cyc = 8; break;
	/* ISC (INC + SBC) */
	case 0xE7: ad = a_zp();   m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); sbc(m); cyc = 5; break;
	case 0xF7: ad = a_zpx();  m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); sbc(m); cyc = 6; break;
	case 0xEF: ad = a_abs();  m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); sbc(m); cyc = 6; break;
	case 0xFF: ad = a_abx(0); m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); sbc(m); cyc = 7; break;
	case 0xFB: ad = a_aby(0); m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); sbc(m); cyc = 7; break;
	case 0xE3: ad = a_izx();  m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); sbc(m); cyc = 8; break;
	case 0xF3: ad = a_izy(0); m = (u8)(cpu_read(ad) + 1); cpu_write(ad, m); sbc(m); cyc = 8; break;
	/* SLO (ASL + ORA) */
	case 0x07: ad = a_zp();   m = op_asl(cpu_read(ad)); cpu_write(ad, m); g.cpu.a |= m; set_zn(g.cpu.a); cyc = 5; break;
	case 0x17: ad = a_zpx();  m = op_asl(cpu_read(ad)); cpu_write(ad, m); g.cpu.a |= m; set_zn(g.cpu.a); cyc = 6; break;
	case 0x0F: ad = a_abs();  m = op_asl(cpu_read(ad)); cpu_write(ad, m); g.cpu.a |= m; set_zn(g.cpu.a); cyc = 6; break;
	case 0x1F: ad = a_abx(0); m = op_asl(cpu_read(ad)); cpu_write(ad, m); g.cpu.a |= m; set_zn(g.cpu.a); cyc = 7; break;
	case 0x1B: ad = a_aby(0); m = op_asl(cpu_read(ad)); cpu_write(ad, m); g.cpu.a |= m; set_zn(g.cpu.a); cyc = 7; break;
	case 0x03: ad = a_izx();  m = op_asl(cpu_read(ad)); cpu_write(ad, m); g.cpu.a |= m; set_zn(g.cpu.a); cyc = 8; break;
	case 0x13: ad = a_izy(0); m = op_asl(cpu_read(ad)); cpu_write(ad, m); g.cpu.a |= m; set_zn(g.cpu.a); cyc = 8; break;
	/* RLA (ROL + AND) */
	case 0x27: ad = a_zp();   m = op_rol(cpu_read(ad)); cpu_write(ad, m); g.cpu.a &= m; set_zn(g.cpu.a); cyc = 5; break;
	case 0x37: ad = a_zpx();  m = op_rol(cpu_read(ad)); cpu_write(ad, m); g.cpu.a &= m; set_zn(g.cpu.a); cyc = 6; break;
	case 0x2F: ad = a_abs();  m = op_rol(cpu_read(ad)); cpu_write(ad, m); g.cpu.a &= m; set_zn(g.cpu.a); cyc = 6; break;
	case 0x3F: ad = a_abx(0); m = op_rol(cpu_read(ad)); cpu_write(ad, m); g.cpu.a &= m; set_zn(g.cpu.a); cyc = 7; break;
	case 0x3B: ad = a_aby(0); m = op_rol(cpu_read(ad)); cpu_write(ad, m); g.cpu.a &= m; set_zn(g.cpu.a); cyc = 7; break;
	case 0x23: ad = a_izx();  m = op_rol(cpu_read(ad)); cpu_write(ad, m); g.cpu.a &= m; set_zn(g.cpu.a); cyc = 8; break;
	case 0x33: ad = a_izy(0); m = op_rol(cpu_read(ad)); cpu_write(ad, m); g.cpu.a &= m; set_zn(g.cpu.a); cyc = 8; break;
	/* SRE (LSR + EOR) */
	case 0x47: ad = a_zp();   m = op_lsr(cpu_read(ad)); cpu_write(ad, m); g.cpu.a ^= m; set_zn(g.cpu.a); cyc = 5; break;
	case 0x57: ad = a_zpx();  m = op_lsr(cpu_read(ad)); cpu_write(ad, m); g.cpu.a ^= m; set_zn(g.cpu.a); cyc = 6; break;
	case 0x4F: ad = a_abs();  m = op_lsr(cpu_read(ad)); cpu_write(ad, m); g.cpu.a ^= m; set_zn(g.cpu.a); cyc = 6; break;
	case 0x5F: ad = a_abx(0); m = op_lsr(cpu_read(ad)); cpu_write(ad, m); g.cpu.a ^= m; set_zn(g.cpu.a); cyc = 7; break;
	case 0x5B: ad = a_aby(0); m = op_lsr(cpu_read(ad)); cpu_write(ad, m); g.cpu.a ^= m; set_zn(g.cpu.a); cyc = 7; break;
	case 0x43: ad = a_izx();  m = op_lsr(cpu_read(ad)); cpu_write(ad, m); g.cpu.a ^= m; set_zn(g.cpu.a); cyc = 8; break;
	case 0x53: ad = a_izy(0); m = op_lsr(cpu_read(ad)); cpu_write(ad, m); g.cpu.a ^= m; set_zn(g.cpu.a); cyc = 8; break;
	/* RRA (ROR + ADC) */
	case 0x67: ad = a_zp();   m = op_ror(cpu_read(ad)); cpu_write(ad, m); adc(m); cyc = 5; break;
	case 0x77: ad = a_zpx();  m = op_ror(cpu_read(ad)); cpu_write(ad, m); adc(m); cyc = 6; break;
	case 0x6F: ad = a_abs();  m = op_ror(cpu_read(ad)); cpu_write(ad, m); adc(m); cyc = 6; break;
	case 0x7F: ad = a_abx(0); m = op_ror(cpu_read(ad)); cpu_write(ad, m); adc(m); cyc = 7; break;
	case 0x7B: ad = a_aby(0); m = op_ror(cpu_read(ad)); cpu_write(ad, m); adc(m); cyc = 7; break;
	case 0x63: ad = a_izx();  m = op_ror(cpu_read(ad)); cpu_write(ad, m); adc(m); cyc = 8; break;
	case 0x73: ad = a_izy(0); m = op_ror(cpu_read(ad)); cpu_write(ad, m); adc(m); cyc = 8; break;
	/* ANC / ALR / ARR / AXS */
	case 0x0B: case 0x2B: g.cpu.a &= cpu_read(g.cpu.pc++); set_zn(g.cpu.a); set_flag(FLAG_C, g.cpu.a & 0x80); cyc = 2; break; /* ANC */
	case 0x4B: g.cpu.a &= cpu_read(g.cpu.pc++); g.cpu.a = op_lsr(g.cpu.a); cyc = 2; break; /* ALR */
	case 0x6B: { /* ARR (approximate, sets C/V per documented behaviour) */
		u8 v = (u8)(g.cpu.a & cpu_read(g.cpu.pc++));
		g.cpu.a = (u8)((v >> 1) | ((g.cpu.p & FLAG_C) ? 0x80 : 0));
		set_zn(g.cpu.a);
		set_flag(FLAG_C, g.cpu.a & 0x40);
		set_flag(FLAG_V, ((g.cpu.a >> 6) ^ (g.cpu.a >> 5)) & 1);
		cyc = 2; break;
	}
	case 0xCB: { /* AXS: X = (A & X) - imm */
		u8 v = cpu_read(g.cpu.pc++);
		u16 t = (u16)((g.cpu.a & g.cpu.x) - v);
		set_flag(FLAG_C, (g.cpu.a & g.cpu.x) >= v);
		g.cpu.x = (u8)t;
		set_zn(g.cpu.x);
		cyc = 2; break;
	}

	/* KIL/JAM and unstable opcodes: treat as NOP so we never wedge */
	default:
#ifdef CPU_TRACE
		{ extern long dbg_badop; extern int dbg_badop_code; dbg_badop++; dbg_badop_code = op; }
#endif
		cyc = 2;
		break;
	}

	cyc += extra;                 /* add page-cross / branch-taken penalties */
	/* Bus accesses already advanced the PPU/APU; top up any remaining internal
	 * cycles (register ops, RMW dummy cycle, taken-branch idle, etc.). */
	while (g.cpu.cycles - start < cyc) cpu_cycle();
	return cyc;
}
