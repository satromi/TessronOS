/*
 * nesint.h - Internal shared state and cross-module contract for the NES core.
 *
 * Design: a single global machine instance `g` (struct NES). Every core module
 * (cpu/ppu/apu/cart/nes) reads and writes `g.<field>` directly, which keeps the
 * hot paths free of context-pointer plumbing. There is exactly one emulated
 * machine per process, which matches the BTRON app (one window, one ROM).
 *
 * Timing model (NTSC, scanline/dot PPU driven by per-instruction CPU catch-up):
 *   nes_run_frame() repeatedly executes one CPU instruction, then ticks the PPU
 *   3x that many dots and the APU once per CPU cycle, until the PPU signals the
 *   end of a frame. The PPU renders a whole visible scanline in one shot (at a
 *   fixed dot) using the loopy v/t/x scroll registers, which is accurate enough
 *   for the targeted mappers (0-4) and games while staying compact.
 */
#ifndef FCEMU_NESINT_H
#define FCEMU_NESINT_H

#include <stdint.h>
#include "nes.h"

typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;

/* ------------------------------------------------------------------ CPU --- */

/* 6502 status flags */
#define FLAG_C 0x01
#define FLAG_Z 0x02
#define FLAG_I 0x04
#define FLAG_D 0x08
#define FLAG_B 0x10
#define FLAG_U 0x20  /* unused, always 1 when pushed */
#define FLAG_V 0x40
#define FLAG_N 0x80

typedef struct {
	u8  a, x, y, s;     /* accumulator, index regs, stack pointer */
	u8  p;              /* processor status */
	u16 pc;             /* program counter */
	long cycles;        /* total CPU cycles since reset */
	int  stall;         /* extra stall cycles (OAM DMA) to charge */
	int  nmi_pending;   /* edge-triggered NMI request from the PPU */
	int  nmi_delay;     /* instructions to wait before servicing NMI (hw latency:
	                     * lets a $2002 vblank poll read the flag before the NMI
	                     * handler clears it) */
	int  irq_line;      /* level-sensitive IRQ line (mapper | apu frame | dmc) */
} CPU;

/* IRQ source bits OR'd into cpu.irq_line so multiple sources coexist */
#define IRQ_APU_FRAME 0x01
#define IRQ_APU_DMC   0x02
#define IRQ_MAPPER    0x04

/* ------------------------------------------------------------------ CART -- */

/* Nametable mirroring modes */
enum { MIR_HORZ = 0, MIR_VERT, MIR_SINGLE0, MIR_SINGLE1, MIR_FOUR };

typedef struct {
	/* ROM images (PRG always ROM; CHR is ROM, or RAM when chr_size==0 in file) */
	u8  *prg;
	u32  prg_size;       /* bytes (multiple of 16KB) */
	u8  *chr;            /* points at chr_rom or chr_ram */
	u32  chr_size;       /* bytes (multiple of 8KB); CHR-RAM == 8192, or 32768 (m30) */
	int  chr_is_ram;
	u8   chr_ram[32768]; /* up to 32KB CHR-RAM for UNROM-512 (30); 8KB for others */

	u8   prg_ram[8192];  /* $6000-$7FFF work/save RAM */

	int  mapper;
	int  mirror;         /* current MIR_* */

	/* generic bank offsets resolved by mapper writes (byte offsets into prg/chr).
	 * prg_off[0]=$8000 prg_off[1]=$A000 prg_off[2]=$C000 prg_off[3]=$E000 (8KB).
	 * chr_off[0..7] = 8x 1KB windows of pattern memory. */
	u32  prg_off[4];
	u32  chr_off[8];

	/* --- MMC1 (mapper 1) --- */
	u8   m1_shift, m1_count, m1_ctrl, m1_chr0, m1_chr1, m1_prg;

	/* --- UNROM (2) / CNROM (3) latch --- */
	u8   bank_latch;

	/* --- MMC3 (mapper 4) --- */
	u8   m4_bank_select;     /* $8000 even: command/banking mode */
	u8   m4_regs[8];         /* R0..R7 */
	u8   m4_irq_latch, m4_irq_counter, m4_irq_reload, m4_irq_enable;
	int  m4_prg_mode, m4_chr_mode;

	/* --- MMC2 (9) / MMC4 (10) --- */
	int  chr_latch;          /* 1 if this mapper uses the $FD/$FE CHR latch */
	u8   m9_prg;             /* switchable PRG bank ($8000)                  */
	u8   m9_chr[4];          /* CHR banks: 0=$0000/FD 1=$0000/FE 2=$1000/FD 3=$1000/FE */
	u8   m9_latch[2];        /* current latch for $0000 / $1000 (0=FD, 1=FE) */

	/* --- Sunsoft FME-7 (69) --- */
	u8   m69_cmd;            /* selected command register (0..0xF)           */
	u8   m69_irq_ctrl;       /* bit7 = counter enable, bit0 = IRQ enable     */
	u16  m69_irq_ctr;        /* 16-bit down-counter, clocked per CPU cycle   */

	/* --- MMC3 derivatives & Taito (33,48,64,88,95,118,119,154,206) --- */
	int  nt_custom;          /* 1: consult nt_map[] for per-1KB nametable    */
	u8   nt_map[4];          /* physical nametable (0/1) per logical table    */
	u8   t_regs[8];          /* Taito TC0190/TC0690 bank registers            */

	/* --- Tengen RAMBO-1 (64) --- */
	u8   m64_regs[16];       /* bank registers R0..R15                        */
	u8   m64_irq_mode;       /* 0 = scanline (A12), 1 = CPU-cycle             */
	u16  m64_irq_pre;        /* CPU-cycle prescaler accumulator (divide-by-4) */

	/* --- Konami VRC family (21,22,23,24,25,26,73,85) --- */
	int  irq_cpu_clocked;    /* 1: cart_irq_clock() must run each instruction */
	u16  vrc_a0, vrc_a1;     /* VRC2/4 register-select de-scramble bit masks  */
	u8   vrc_prg[3];         /* switchable PRG bank registers (per-chip use)  */
	u8   vrc_prg_mode;       /* VRC4 PRG swap mode (M)                        */
	u8   vrc6_ctrl;          /* VRC6 $B003: W.PN MMDD (mode/mirror)           */
	u16  vrc_chr[8];         /* CHR bank regs (9-bit VRC2/4, 8-bit VRC6/7)    */
	u8   vrc_irq_latch;      /* 8-bit reload value (VRC4/6/7)                 */
	u8   vrc_irq_counter;    /* 8-bit up-counter (VRC4/6/7)                   */
	u8   vrc_irq_enable;     /* E (enable)                                    */
	u8   vrc_irq_aenable;    /* A (enable-after-acknowledge)                  */
	u8   vrc_irq_mode;       /* M: VRC4/6/7 0=scanline/1=cycle; VRC3 0=16/1=8 */
	int  vrc_irq_pre;        /* scanline-mode prescaler accumulator (341,-3)  */
	u16  vrc3_latch;         /* VRC3 16-bit reload value                      */
	u16  vrc3_counter;       /* VRC3 16-bit up-counter                        */

	/* --- Phase 4: Bandai 16/159, Jaleco 18, Irem 32/65, Sunsoft 67/68, Namco 19 */
	u8   p4_prg[3];          /* PRG bank regs (recomputed on mode change)     */
	u8   p4_chr[8];          /* CHR bank regs (Jaleco-18 assembled / Namco163)*/
	u8   p4_nt[4];           /* Namco163 NT-select / Sunsoft-4 NT banks       */
	u8   p4_layout;          /* Irem PRG mode bit / misc layout               */
	u8   p4_misc;            /* Sunsoft-4 $E000 / Namco163 CHR-RAM-disable    */
	u16  p4_irq_latch;       /* 16-bit reload latch (16/18/65)                */
	u16  p4_irq_counter;     /* 16-/15-bit IRQ counter                        */
	u8   p4_irq_enable;
	u8   p4_irq_width;       /* Jaleco-18 counter-width bits (C/T/E/F)         */
	u8   p4_irq_toggle;      /* Sunsoft-3 write-twice flip-flop               */

	/* --- CHR-ROM nametable engine (Sunsoft-4, Namco163) --- */
	int  nt_rom;             /* 1: some nametables read from CHR-ROM          */
	u8   nt_is_rom[4];       /* per logical NT: 1=CHR-ROM bank, 0=CIRAM page  */
	u8   nt_page[4];         /* CIRAM physical page (0/1) when nt_is_rom==0   */
	u16  nt_bank[4];         /* CHR-ROM 1KB bank when nt_is_rom==1            */

	/* --- MMC5 (5) --- */
	u8   m5_prg_mode, m5_chr_mode, m5_exram_mode;
	u8   m5_nt_map;          /* $5105 DDCCBBAA                                 */
	u8   m5_fill_tile, m5_fill_attr;
	u8   m5_prg_reg[4];      /* $5114-$5117                                    */
	u16  m5_chr_a[8];        /* $5120-$5127 sprite set (10-bit via $5130)      */
	u16  m5_chr_b[4];        /* $5128-$512B background set                     */
	u8   m5_chr_upper;       /* $5130 (CHR bank upper 2 bits)                  */
	u8   m5_mul_a, m5_mul_b;
	u8   m5_irq_latch, m5_irq_enable, m5_irq_pending, m5_in_frame;
	u8   m5_scanline;        /* internal scanline counter                     */
	u32  m5_chr_off_bg[8];   /* resolved BG CHR offsets (set B in 8x16)        */
	u8   m5_exram[1024];     /* on-chip ExRAM                                  */
	u8   m5_fillbuf[1024];   /* synthesized fill-mode nametable                */

	/* --- Phase 6: expansion audio (VRC6/Sunsoft5B/Namco163/MMC5/VRC7) --- */
	int  has_exp_audio;      /* 1: cart_audio_tick()/out() are active          */
	/* VRC6 (24/26): 2 pulse [0,1] + saw [2] */
	u8   v6_vol[2], v6_duty[2], v6_mode[2], v6_en[3], v6_step[2];
	u16  v6_period[3], v6_div[3];
	u8   v6_saw_rate, v6_saw_acc, v6_saw_phase, v6_saw_rt, v6_halt, v6_shift;
	/* Sunsoft 5B (69): 3 square channels A/B/C */
	u8   ay_sel, ay_mixer, ay_vol[3], ay_high[3];
	u16  ay_period[3], ay_div[3];
	/* Namco163 audio (19): up to 8 wavetable channels in 128-byte RAM */
	u8   n163_ram[128];
	u8   n163_addr, n163_autoinc, n163_cyc, n163_slot;
	int  n163_out[8];
	/* MMC5 audio (5): 2 pulse + 8-bit PCM */
	u8   m5a_duty[2], m5a_halt[2], m5a_cv[2], m5a_vol[2], m5a_seq[2], m5a_len[2];
	u16  m5a_period[2], m5a_timer[2];
	u8   m5a_half, m5a_pcm, m5a_pcm_read;
	int  m5a_240;
	/* VRC7 OPLL FM (85): 6 channels x 2 operators (12 slots) */
	u8   v7_reg[64], v7_adr;
	u32  v7_pgphase[12];
	u16  v7_pgout[12];
	int  v7_egout[12];       /* 0(loud)..127(mute) */
	u8   v7_egstate[12];     /* 0 idle,1 damp,2 attack,3 decay,4 sustain,5 release */
	int  v7_modhist[6][2];   /* modulator output history (feedback)             */
	u8   v7_keyprev[6];
	u32  v7_egcnt;
	int  v7_clk;             /* 36-CPU-cycle -> 1 OPLL internal sample          */
	int  v7_chout[6];
} CART;

/* ------------------------------------------------------------------ PPU -- */

typedef struct {
	u8  ctrl;      /* $2000 */
	u8  mask;      /* $2001 */
	u8  status;    /* $2002 (low 5 bits open bus / latch) */
	u8  oamaddr;   /* $2003 */

	u16 v, t;      /* loopy current / temp VRAM address */
	u8  x;         /* fine X scroll (3 bits) */
	u8  w;         /* write toggle */
	u8  readbuf;   /* $2007 buffered read latch */

	u8  oam[256];
	u8  palette[32];
	u8  vram[4096];        /* 4 nametables worth (4-screen); 2-screen uses low 2KB */

	int scanline;          /* -1(pre)/0..239 visible /240 post /241..260 vblank */
	int dot;               /* 0..340 */
	long frame;            /* frame counter */
	int  frame_ready;      /* set true at end of a frame */
	int  odd;              /* odd/even frame for cycle skip */

	int  s0_hit_dot;       /* dot at which to raise the sprite-0 hit flag (-1=none)*/

	u32  fb[NES_W * NES_H];
} PPU;

/* ------------------------------------------------------------------ APU -- */

typedef struct {
	u8   reg[0x18];        /* $4000-$4017 raw (for read-back where relevant) */
	/* length counters for the 4 channels (pulse1,pulse2,triangle,noise) */
	u8   len[4];
	int  enabled[5];       /* p1,p2,tri,noise,dmc */
	/* frame sequencer */
	int  frame_mode;       /* 0 = 4-step, 1 = 5-step */
	int  frame_irq_inhibit;
	int  frame_irq;        /* pending frame IRQ flag */
	long frame_cycle;      /* CPU cycle accumulator for the sequencer */
	int  seq_step;
	long frame_ctr_down;   /* cycles until next frame-seq boundary (audio-off fast path) */
	/* DMC (minimal: just IRQ + length so reads of $4015 behave) */
	int  dmc_irq;
	int  dmc_bytes;        /* remaining sample bytes (counts down, no real DMA) */
	int  dmc_loop;
	int  dmc_sample_len;   /* bytes loaded on (re)start */
	int  dmc_timer;        /* CPU-cycle countdown to next byte fetch */

	/* ---- audio synthesis (added) ---- */
	int      apu_cyc;          /* toggles each CPU cycle (APU clock = /2)        */
	u16      p_timer[2];       /* pulse period counters                          */
	u8       p_duty[2];        /* pulse duty-sequencer position (0..7)           */
	u8       p_env_start[2];   /* pulse envelope restart flags                   */
	u8       p_env_div[2];     /* pulse envelope dividers                        */
	u8       p_env_vol[2];     /* pulse envelope decay volumes (0..15)           */
	u8       p_sweep_div[2];   /* pulse sweep dividers                           */
	u8       p_sweep_reload[2];/* pulse sweep reload flags                       */
	u16      t_timer;          /* triangle period counter                        */
	u8       t_pos;            /* triangle sequencer position (0..31)            */
	u8       lin_ctr;          /* triangle linear counter                        */
	u8       lin_reload;       /* triangle linear-counter reload flag            */
	u16      n_timer;          /* noise period counter                           */
	u16      n_lfsr;           /* noise 15-bit LFSR                              */
	u8       n_env_start, n_env_div, n_env_vol;  /* noise envelope               */
	u8       dmc_dac;          /* DMC 7-bit output level (0..127)                */
	u16      dmc_caddr;        /* DMC current sample byte offset from $C000      */
	u8       dmc_shift;        /* DMC sample shift register                      */
	u8       dmc_sbits;        /* DMC bits remaining in the shift register       */
	int      dmc_silence;      /* DMC output-unit silence flag                   */
	long     smp_acc;          /* CPU->48k downsample accumulator                */
	long     hp_in, hp_out;    /* one-pole DC-removal high-pass state            */
	int      smp_w, smp_r;     /* sample ring buffer write/read indices          */
	int16_t  smp_buf[8192];    /* 48kHz mono 16-bit output ring (~0.17s)         */
} APU;

/* ------------------------------------------------------------- Machine --- */

struct NES {
	CPU  cpu;
	PPU  ppu;
	APU  apu;
	CART cart;

	u8   ram[2048];        /* $0000-$07FF internal RAM */

	/* controllers */
	u8   pad[2];           /* live button state (NES_BTN_*) */
	u8   pad_shift[2];     /* shift register snapshot while reading */
	int  pad_strobe;       /* $4016 strobe latch */
};

extern struct NES g;

/* ----- bus (nes.c) ----- */
u8   cpu_read(u16 a);
void cpu_write(u16 a, u8 v);

/* ----- cpu (cpu.c) ----- */
void cpu_power(void);
void cpu_reset(void);
void cpu_trigger_nmi(void);
int  cpu_step(void);          /* run one instruction; returns CPU cycles used */

/* ----- ppu (ppu.c) ----- */
void ppu_power(void);
void ppu_reset(void);
u8   ppu_reg_read(u16 a);     /* a in 0x2000..0x2007 */
void ppu_reg_write(u16 a, u8 v);
u8   ppu_mem_read(u16 a);     /* PPU bus $0000..$3FFF */
void ppu_mem_write(u16 a, u8 v);

/* ----- apu (apu.c) ----- */
void apu_power(void);
void apu_reset(void);
void apu_write(u16 a, u8 v);  /* $4000..$4013, $4015, $4017 */
u8   apu_read_status(void);   /* $4015 */
int  apu_read_samples(int16_t *out, int max);  /* drain 48kHz/16-bit mono */
void apu_set_audio(int on);   /* enable synthesis (off => zero cost) */

/* ----- cart (cart.c) ----- */
int  cart_load(const u8 *rom, long len, const char **err);
u8   cart_cpu_read(u16 a);    /* $4020..$FFFF (PRG ROM/RAM) */
void cart_cpu_write(u16 a, u8 v);
u8   cart_chr_read(u16 a);    /* PPU $0000..$1FFF */
void cart_chr_write(u16 a, u8 v);
u16  cart_nt_index(u16 a);    /* map $2000..$2FFF -> 0..0x7FF physical vram idx */
const u8 *cart_nt_base(u16 a);/* 1KB NT region base (CIRAM or CHR-ROM); byte = base[a&0x3FF] */
void mmc5_chr_resolve(void);  /* MMC5: recompute BG/sprite CHR offsets (per scanline) */
void mmc5_scanline_irq(void); /* MMC5: visible-scanline IRQ tick */
void mmc5_end_frame(void);    /* MMC5: clear in-frame at vblank */
void mmc5_nt_write(u16 a, u8 v); /* MMC5: PPU nametable write routing */
void cart_audio_tick(void);   /* advance expansion-audio chip by 1 CPU cycle */
int  cart_audio_out(void);    /* current expansion-audio sample, added to APU mix */
void n163_audio_addr(u8 v);        /* Namco163 sound RAM address port ($F800)  */
void n163_audio_data_write(u8 v);  /* Namco163 sound RAM data write ($4800)     */
u8   n163_audio_data_read(void);   /* Namco163 sound RAM data read ($4800)      */
void cart_ppu_scanline(void); /* per-scanline hook (MMC3 IRQ counter) */
void cart_chr_latch(u16 patbase, u8 tile); /* MMC2/4 $FD/$FE CHR latch (render) */
void cart_irq_clock(int cyc); /* FME-7 per-instruction IRQ counter (nes_run_frame) */

/* ------------------------------------------------- per-cycle hot path --- *
 * The CPU advances the rest of the machine one cycle at a time on every bus
 * access (and internal cycle). With this build compiling each .c as a separate
 * translation unit and NO -flto, an out-of-line cpu_cycle() that calls
 * apu_tick()+3x ppu_tick() cost ~149k cross-TU call/ret per frame — the single
 * dominant cost on the slow i386 target. These static-inline forms collapse all
 * of that into the caller; only genuinely "eventful" dots / sequencer
 * boundaries fall back to the cold out-of-line *_slow() bodies. The per-access
 * apu(1)+ppu(3) advance still happens IN PLACE before the access returns, so the
 * $2002-poll-vs-NMI ordering is preserved bit-for-bit. */
void ppu_tick_slow(void);     /* cold: a dot that may raise a flag / do loopy work */
void apu_tick_slow(void);     /* cold: a frame-sequencer boundary is due           */
void apu_synth_tick(void);    /* cold: audio synthesis (only when audio is on)     */
void apu_dmc_tick(void);      /* DMC sample clock (must run even with audio off)   */
extern int apu_audio_on;      /* defined in apu.c                                  */

/* one PPU dot. Fast path = just advance the dot; everything that can set a flag,
 * frame_ready, do loopy v/t updates or the odd-frame skip routes to the verbatim
 * ppu_tick_slow(). The gate is a strict SUPERSET of eventful dots, so behaviour
 * is identical to the old ppu_tick(). */
static __inline__ void ppu_tick_inline(void)
{
	PPU *p = &g.ppu;
	int dot = p->dot;
	/* Route to the cold path ONLY on dots that can have a side effect: render
	 * (0), the RENDER_ON loopy/MMC3 dots (256/257/260), the dot-wrap+odd-skip
	 * (>=340), the vblank line (241) and the pre-render line (261, many event
	 * dots). Every other dot just advances. The old gate sent all dot>=256 to
	 * ppu_tick_slow() — a cross-TU call on this no-LTO build — for ~80 inert
	 * dots/line (258,259,261..339); those now take the cheap inline path.
	 * Behaviour is identical: the routed-away dots have no side effects in the
	 * original ppu_tick body. */
	if (p->s0_hit_dot >= 0 ||
	    dot == 0 || dot == 256 || dot == 257 || dot == 260 || dot >= 340 ||
	    p->scanline == 241 || p->scanline >= 261) {
		ppu_tick_slow();
		return;
	}
	p->dot = dot + 1;         /* overwhelmingly common inert dot */
}

/* one APU cycle. Audio-off hot path = a single decrement to the next boundary. */
static __inline__ void apu_tick_inline(void)
{
	APU *a = &g.apu;
	if (--a->frame_ctr_down <= 0) apu_tick_slow();
	if (apu_audio_on) apu_synth_tick();          /* includes the DMC clock */
	else if (a->dmc_bytes > 0) apu_dmc_tick();   /* audio OFF: still count the sample
	                                              * down so $4015 bit4 clears and the
	                                              * DMC IRQ fires (games poll/wait on
	                                              * these -> would soft-lock). Costs
	                                              * one test when no sample plays. */
}

/* advance the whole machine by one CPU cycle (was the out-of-line cpu_cycle).
 * The 3 PPU dots per CPU cycle are the dominant per-frame work. The common case
 * is 3 inert mid-line dots (no event, no armed sprite-0 hit): collapse them to a
 * single dot+=3 with ONE test instead of three ppu_tick_inline gate checks. dot
 * 0 (render), the RENDER_ON dots (256/257/260), the wrap and lines 241/261 are
 * all excluded, so every event still fires exactly as the per-dot path would —
 * byte-identical. This preserves the per-CPU-cycle PPU advance the $2002/NMI
 * ordering depends on; only inert dots are coalesced. */
static __inline__ void nes_cycle(void)
{
	apu_tick_inline();
#ifndef NO_DOT_BATCH
	{
		PPU *p = &g.ppu;
		int dot = p->dot;
		if (p->s0_hit_dot < 0 && dot >= 1 && dot + 3 <= 256 &&
		    p->scanline != 241 && p->scanline < 261) {
			p->dot = dot + 3;         /* 3 inert dots -> one advance */
		} else {
			ppu_tick_inline();
			ppu_tick_inline();
			ppu_tick_inline();
		}
	}
#else
	ppu_tick_inline();                        /* reference per-dot path (A/B) */
	ppu_tick_inline();
	ppu_tick_inline();
#endif
	g.cpu.cycles++;
}
#define cpu_cycle nes_cycle   /* existing call sites now inline, no edits needed */

/* Inlinable mapper fast paths for the hot read ranges (no cross-TU call). The
 * index math is byte-identical to cart_chr_read / the a>=0x8000 branch of
 * cart_cpu_read; cold ranges keep the out-of-line functions. */
static __inline__ u8 cart_chr_read_fast(u16 a)
{
	return g.cart.chr[g.cart.chr_off[(a >> 10) & 7] + (a & 0x3FF)];
}
static __inline__ u8 cart_cpu_read_fast(u16 a)   /* caller guarantees a >= 0x8000 */
{
	return g.cart.prg[g.cart.prg_off[(a >> 13) & 3] + (a & 0x1FFF)];
}

#endif /* FCEMU_NESINT_H */
