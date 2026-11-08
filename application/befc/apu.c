/*
 * apu.c - NES APU: register/timing model PLUS audio synthesis.
 *
 * Emulates the observable state (length counters, frame sequencer + IRQ, $4015,
 * DMC byte counter) AND now synthesises sound: the two pulse channels, triangle,
 * noise and DMC are clocked per CPU cycle, mixed (linear approximation of the
 * NES non-linear mixer) and resampled to a 48kHz / 16-bit mono ring buffer that
 * the frontend drains and writes to the "ｓｏｕｎｄ" device (see nes_audio_read).
 */
#include <string.h>
#include "nesint.h"

#define APU_OUT_HZ   48000
#define NES_CPU_HZ   1789773          /* NTSC CPU frequency                     */
#define SMP_N        8192             /* ring buffer length (must match struct) */

/* Audio synthesis runs only when an output consumer is active (a "ｓｏｕｎｄ"
 * device opened). Off by default so the per-cycle mixer adds zero cost until
 * sound is actually wired up. */
int apu_audio_on = 0;          /* read by apu_tick_inline (nesint.h) */
/* per-channel mute mask for debugging which channel is the noise: bit0=pulse1,
 * 1=pulse2, 2=triangle, 3=noise, 4=dmc. 0x1F = all on. */
int apu_chan_mask = 0x1F;
void apu_set_audio(int on)
{
	apu_audio_on = on;
	if (!on) { g.apu.smp_w = g.apu.smp_r = 0; }
}

static const u8 len_table[32] = {
	10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
	12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

/* NTSC frame-sequencer boundaries (CPU cycles) */
static const long seq4[4] = { 7457, 14913, 22371, 29829 };
static const long seq5[5] = { 7457, 14913, 22371, 29829, 37281 };

/* cycles from the just-fired step to the NEXT boundary (audio-off fast path).
 * Equals boundary[s] - boundary[s-1], so the countdown fires a sequencer step
 * on the identical CPU cycle the old absolute-compare apu_tick() did. */
static long apu_next_delta(void)
{
	const long *seq = g.apu.frame_mode ? seq5 : seq4;
	int nstep = g.apu.frame_mode ? 5 : 4;
	int s = g.apu.seq_step;
	long now  = (s == 0) ? 0 : seq[s - 1];
	long next = (s < nstep) ? seq[s] : seq[0];
	return next - now;
}

/* NTSC DMC rate table (CPU cycles per output-unit clock) */
static const int dmc_rate[16] = {
	428, 380, 340, 320, 286, 254, 226, 214,
	190, 160, 142, 128, 106,  84,  72,  54
};

static const u16 noise_period[16] = {
	4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

static const u8 duty_seq[4][8] = {
	{ 0,1,0,0,0,0,0,0 },        /* 12.5% */
	{ 0,1,1,0,0,0,0,0 },        /* 25%   */
	{ 0,1,1,1,1,0,0,0 },        /* 50%   */
	{ 1,0,0,1,1,1,1,1 }         /* 25% negated */
};

static const u8 tri_seq[32] = {
	15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
	0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
};

/* ------------------------------------------------------ frame sequencer ---- */

static void clock_lengths(void)
{
	APU *a = &g.apu;
	if (!(a->reg[0x00] & 0x20) && a->len[0]) a->len[0]--;
	if (!(a->reg[0x04] & 0x20) && a->len[1]) a->len[1]--;
	if (!(a->reg[0x08] & 0x80) && a->len[2]) a->len[2]--;
	if (!(a->reg[0x0C] & 0x20) && a->len[3]) a->len[3]--;
}

static void clock_one_env(u8 reg, u8 *start, u8 *div, u8 *vol)
{
	if (*start) {
		*start = 0;
		*vol = 15;
		*div = reg & 0x0F;
	} else if (*div == 0) {
		*div = reg & 0x0F;
		if (*vol > 0) (*vol)--;
		else if (reg & 0x20) *vol = 15;     /* loop */
	} else {
		(*div)--;
	}
}

/* quarter-frame: envelopes (pulse1/2, noise) + triangle linear counter */
static void clock_quarter(void)
{
	APU *a = &g.apu;
	clock_one_env(a->reg[0x00], &a->p_env_start[0], &a->p_env_div[0], &a->p_env_vol[0]);
	clock_one_env(a->reg[0x04], &a->p_env_start[1], &a->p_env_div[1], &a->p_env_vol[1]);
	clock_one_env(a->reg[0x0C], &a->n_env_start,    &a->n_env_div,    &a->n_env_vol);
	if (a->lin_reload) a->lin_ctr = a->reg[0x08] & 0x7F;
	else if (a->lin_ctr > 0) a->lin_ctr--;
	if (!(a->reg[0x08] & 0x80)) a->lin_reload = 0;
}

/* half-frame sweep for pulse channel i (0,1) */
static void clock_one_sweep(int i)
{
	APU *a = &g.apu;
	u8  reg = a->reg[0x01 + i * 4];
	int shift = reg & 0x07;
	int period = (a->reg[0x03 + i * 4] & 0x07) << 8 | a->reg[0x02 + i * 4];
	int delta = period >> shift;
	int target = (reg & 0x08) ? (period - delta - (i == 0 ? 1 : 0)) : (period + delta);

	if (a->p_sweep_div[i] == 0 && (reg & 0x80) && shift > 0 &&
	    period >= 8 && target <= 0x7FF && target >= 0) {
		a->reg[0x02 + i * 4] = (u8)(target & 0xFF);
		a->reg[0x03 + i * 4] = (u8)((a->reg[0x03 + i * 4] & 0xF8) | ((target >> 8) & 0x07));
	}
	if (a->p_sweep_div[i] == 0 || a->p_sweep_reload[i]) {
		a->p_sweep_div[i] = (reg >> 4) & 0x07;
		a->p_sweep_reload[i] = 0;
	} else {
		a->p_sweep_div[i]--;
	}
}

static void clock_half(void)
{
	clock_lengths();
	clock_one_sweep(0);
	clock_one_sweep(1);
}

/* ------------------------------------------------------------- channels ---- */

static int pulse_out(int i)
{
	APU *a = &g.apu;
	int period = (a->reg[0x03 + i * 4] & 0x07) << 8 | a->reg[0x02 + i * 4];
	if (a->len[i] == 0 || period < 8 || duty_seq[a->reg[0x00 + i * 4] >> 6][a->p_duty[i]] == 0)
		return 0;
	return (a->reg[0x00 + i * 4] & 0x10) ? (a->reg[0x00 + i * 4] & 0x0F) : a->p_env_vol[i];
}

static int noise_out(void)
{
	APU *a = &g.apu;
	if (a->len[3] == 0 || (a->n_lfsr & 1))
		return 0;
	return (a->reg[0x0C] & 0x10) ? (a->reg[0x0C] & 0x0F) : a->n_env_vol;
}

/* DMC output unit (CPU-cycle clocked at its own rate). Extracted from
 * apu_synth_tick because it must ALSO run while audio output is off: games poll
 * $4015 bit4 for sample completion or wait for the DMC IRQ, so gating the whole
 * unit behind apu_audio_on left dmc_bytes frozen and soft-locked them. */
void apu_dmc_tick(void)
{
	APU *a = &g.apu;
	if (a->dmc_timer > 0 && --a->dmc_timer == 0) {
		a->dmc_timer = dmc_rate[a->reg[0x10] & 0x0F];
		if (!a->dmc_silence) {
			if (a->dmc_shift & 1) { if (a->dmc_dac <= 125) a->dmc_dac += 2; }
			else                  { if (a->dmc_dac >= 2)   a->dmc_dac -= 2; }
		}
		a->dmc_shift >>= 1;
		if (a->dmc_sbits == 0 || --a->dmc_sbits == 0) {
			a->dmc_sbits = 8;
			if (a->dmc_bytes > 0) {
				/* DMC fetch address wraps $FFFF->$8000 (NESdev), not $FFFF->$0000:
				 * base $C000 + caddr, kept inside $8000-$FFFF. */
				a->dmc_shift = cart_cpu_read((u16)(0x8000 + ((0x4000 + a->dmc_caddr) & 0x7FFF)));
				a->dmc_caddr++;
				a->dmc_silence = 0;
				if (--a->dmc_bytes == 0) {
					if (a->dmc_loop) {
						a->dmc_caddr = (u16)(a->reg[0x12] << 6);
						a->dmc_bytes = (a->reg[0x13] << 4) + 1;
					} else if (a->reg[0x10] & 0x80) {
						a->dmc_irq = 1;
						g.cpu.irq_line |= IRQ_APU_DMC;
					}
				}
			} else {
				a->dmc_silence = 1;
			}
		}
	}
}

/* advance the channel timers by one APU/CPU cycle and emit resampled output.
 * Called from apu_tick_inline (nesint.h) only while audio output is on. */
void apu_synth_tick(void)
{
	APU *a = &g.apu;
	int i;

	if (g.cart.has_exp_audio)             /* expansion sound chip (Phase 6) */
		cart_audio_tick();

	/* triangle: clocked every CPU cycle. Freeze the sequencer when the timer period
	 * is < 2 (the value real hardware runs ultrasonically and a point-sampler aliases
	 * into loud noise). t_pos then holds a DC level
	 * the high-pass removes, instead of screaming. */
	{
		int t_period = (a->reg[0x0B] & 0x07) << 8 | a->reg[0x0A];
		if (a->t_timer == 0) {
			a->t_timer = (u16)t_period;
			if (a->len[2] > 0 && a->lin_ctr > 0 && t_period >= 2)
				a->t_pos = (u8)((a->t_pos + 1) & 31);
		} else {
			a->t_timer--;
		}
	}

	/* pulse + noise + DMC: clocked every other CPU cycle (APU clock) */
	a->apu_cyc ^= 1;
	if (a->apu_cyc == 0) {
		for (i = 0; i < 2; i++) {
			if (a->p_timer[i] == 0) {
				a->p_timer[i] = (u16)((a->reg[0x03 + i * 4] & 0x07) << 8 | a->reg[0x02 + i * 4]);
				a->p_duty[i] = (u8)((a->p_duty[i] + 1) & 7);
			} else {
				a->p_timer[i]--;
			}
		}
		if (a->n_timer == 0) {
			u16 fb;
			a->n_timer = noise_period[a->reg[0x0E] & 0x0F];
			fb = (a->n_lfsr ^ (a->reg[0x0E] & 0x80 ? (a->n_lfsr >> 6) : (a->n_lfsr >> 1))) & 1;
			a->n_lfsr = (u16)((a->n_lfsr >> 1) | (fb << 14));
		} else {
			a->n_timer--;
		}
	}

	apu_dmc_tick();                       /* DMC output unit (see above) */

	/* resample: emit one output sample every NES_CPU_HZ/APU_OUT_HZ CPU cycles */
	a->smp_acc += APU_OUT_HZ;
	if (a->smp_acc >= NES_CPU_HZ) {
		/* Linear approximation of the NES non-linear mixer, in the correct RATIOS
		 * (nesdev: pulse 0.00752/unit, tri 0.00851, noise 0.00494, dmc 0.00335),
		 * scaled to int16 with headroom. The OLD weights had the noise channel ~4x
		 * and the triangle ~3x too loud relative to the pulses, so any game using the
		 * noise channel (drums/SFX) drowned the music in broadband "noise". */
		int p1  = (apu_chan_mask & 0x01) ? pulse_out(0) : 0;
		int p2  = (apu_chan_mask & 0x02) ? pulse_out(1) : 0;
		int tri = (apu_chan_mask & 0x04) ? tri_seq[a->t_pos] : 0;
		int noi = (apu_chan_mask & 0x08) ? noise_out() : 0;
		int dmc = (apu_chan_mask & 0x10) ? a->dmc_dac : 0;
		int s = (p1 + p2) * 226 + tri * 255 + noi * 148 + dmc * 100;
		if (g.cart.has_exp_audio)         /* add expansion-chip output (Phase 6) */
			s += cart_audio_out();
		/* one-pole high-pass (~75Hz) removes the DC the idle triangle adds, as
		 * the real NES mixer + speaker do: y = x - x_prev + 0.988*y_prev. */
		long y = (long)s - a->hp_in + (a->hp_out * 253) / 256;
		a->hp_in = s;
		a->hp_out = y;
		a->smp_acc -= NES_CPU_HZ;
		if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
		{
			int nw = (a->smp_w + 1) % SMP_N;
			if (nw != a->smp_r) {           /* drop if the consumer is behind */
				a->smp_buf[a->smp_w] = (int16_t)y;
				a->smp_w = nw;
			}
		}
	}
}

/* drain up to `max` samples into `out`; returns the count written. */
int apu_read_samples(int16_t *out, int max)
{
	APU *a = &g.apu;
	int n = 0;
	while (n < max && a->smp_r != a->smp_w) {
		out[n++] = a->smp_buf[a->smp_r];
		a->smp_r = (a->smp_r + 1) % SMP_N;
	}
	return n;
}

/* ----------------------------------------------------------- registers ----- */

void apu_write(u16 addr, u8 v)
{
	APU *a = &g.apu;

	if (addr >= 0x4000 && addr <= 0x4013) {
		a->reg[addr - 0x4000] = v;
		switch (addr) {
		case 0x4003: if (a->enabled[0]) a->len[0] = len_table[v >> 3]; a->p_env_start[0] = 1; a->p_duty[0] = 0; break;
		case 0x4007: if (a->enabled[1]) a->len[1] = len_table[v >> 3]; a->p_env_start[1] = 1; a->p_duty[1] = 0; break;
		case 0x400B: if (a->enabled[2]) a->len[2] = len_table[v >> 3]; a->lin_reload = 1; break;
		case 0x400F: if (a->enabled[3]) a->len[3] = len_table[v >> 3]; a->n_env_start = 1; break;
		case 0x4001: a->p_sweep_reload[0] = 1; break;
		case 0x4005: a->p_sweep_reload[1] = 1; break;
		case 0x4010:
			a->dmc_loop = (v & 0x40) != 0;
			if (!(v & 0x80)) { a->dmc_irq = 0; g.cpu.irq_line &= ~IRQ_APU_DMC; }
			break;
		case 0x4011: a->dmc_dac = v & 0x7F; break;
		default: break;
		}
		return;
	}

	if (addr == 0x4015) {
		a->enabled[0] = (v & 0x01) != 0;
		a->enabled[1] = (v & 0x02) != 0;
		a->enabled[2] = (v & 0x04) != 0;
		a->enabled[3] = (v & 0x08) != 0;
		a->enabled[4] = (v & 0x10) != 0;
		if (!(v & 0x01)) a->len[0] = 0;
		if (!(v & 0x02)) a->len[1] = 0;
		if (!(v & 0x04)) a->len[2] = 0;
		if (!(v & 0x08)) a->len[3] = 0;
		if (v & 0x10) {
			a->dmc_sample_len = (a->reg[0x13] * 16) + 1;
			if (a->dmc_bytes == 0) {
				a->dmc_caddr = (u16)(a->reg[0x12] << 6);
				a->dmc_bytes = (a->reg[0x13] << 4) + 1;
				a->dmc_timer = dmc_rate[a->reg[0x10] & 0x0F];
				a->dmc_silence = 0;
				a->dmc_sbits = 8;
			}
		} else {
			a->dmc_bytes = 0;
			a->dmc_silence = 1;
		}
		a->dmc_irq = 0;
		g.cpu.irq_line &= ~IRQ_APU_DMC;
		return;
	}

	if (addr == 0x4017) {
#ifdef CPU_TRACE
		{ extern long dbg_w4017; dbg_w4017++; }
#endif
		a->frame_mode = (v & 0x80) ? 1 : 0;
		a->frame_irq_inhibit = (v & 0x40) ? 1 : 0;
		if (a->frame_irq_inhibit) { a->frame_irq = 0; g.cpu.irq_line &= ~IRQ_APU_FRAME; }
		a->seq_step = 0;
		a->frame_cycle = 0;
		if (a->frame_mode) { clock_quarter(); clock_half(); }  /* 5-step: immediate */
		a->frame_ctr_down = apu_next_delta();   /* re-arm audio-off countdown */
		return;
	}
}

u8 apu_read_status(void)
{
	APU *a = &g.apu;
	u8 r = 0;
	if (a->len[0]) r |= 0x01;
	if (a->len[1]) r |= 0x02;
	if (a->len[2]) r |= 0x04;
	if (a->len[3]) r |= 0x08;
	if (a->dmc_bytes > 0) r |= 0x10;
	if (a->frame_irq) r |= 0x40;
	if (a->dmc_irq)   r |= 0x80;
	a->frame_irq = 0;                         /* reading clears the frame IRQ */
	g.cpu.irq_line &= ~IRQ_APU_FRAME;
	return r;
}

/* Cold slow path: reached (via apu_tick_inline in nesint.h) only when the
 * audio-off frame_ctr_down countdown hits a sequencer boundary. Fires the same
 * quarter/half/IRQ work the old per-cycle apu_tick() did, then re-arms the
 * countdown to the next boundary. Audio synthesis (when on) runs every cycle in
 * apu_tick_inline, unchanged. */
void apu_tick_slow(void)
{
	APU *a = &g.apu;
	const long *seq = a->frame_mode ? seq5 : seq4;
	int nstep = a->frame_mode ? 5 : 4;
	int step  = a->seq_step;
	int half = a->frame_mode ? (step == 1 || step == 4)
	                         : (step == 1 || step == 3);
	int quarter = a->frame_mode ? (step != 3) : 1;
	if (quarter) clock_quarter();
	if (half)    clock_half();
	if (!a->frame_mode && step == 3 && !a->frame_irq_inhibit) {
		a->frame_irq = 1;
		g.cpu.irq_line |= IRQ_APU_FRAME;
	}
	a->seq_step++;
	a->frame_cycle = seq[step];                /* keep in sync with old model */
	if (a->seq_step >= nstep) { a->seq_step = 0; a->frame_cycle = 0; }
	a->frame_ctr_down = apu_next_delta();      /* arm next boundary */
}

void apu_power(void)
{
	memset(&g.apu, 0, sizeof(g.apu));
	g.apu.n_lfsr = 1;
	/* Hold the frame-counter IRQ off until the game configures $4017. */
	g.apu.frame_irq_inhibit = 1;
	g.apu.frame_ctr_down = seq4[0];           /* arm audio-off countdown */
}

void apu_reset(void)
{
	int i;
	for (i = 0; i < 4; i++) g.apu.len[i] = 0;
	g.apu.enabled[0] = g.apu.enabled[1] = g.apu.enabled[2] = 0;
	g.apu.enabled[3] = g.apu.enabled[4] = 0;
	g.apu.frame_irq = 0;
	g.apu.dmc_irq = 0;
	g.apu.dmc_bytes = 0;
	g.apu.dmc_silence = 1;
	g.apu.n_lfsr = 1;
	g.apu.smp_w = g.apu.smp_r = 0;
	g.apu.seq_step = 0;
	g.apu.frame_cycle = 0;
	g.apu.frame_ctr_down = seq4[0];           /* arm audio-off countdown */
	g.cpu.irq_line &= ~(IRQ_APU_FRAME | IRQ_APU_DMC);
}
