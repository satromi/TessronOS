/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cp_stages.h
 *	The compositing shaders' work, written with Mesa's NIR builder: the
 *	stages of compose.c pixel by pixel, with the same integer arithmetic
 *	(docs/tessronos/13-gpu.md 13.10 and 13.11.14).
 *
 *	Each shader is built as a fragment shader that reads what is behind
 *	its pixel from the render target and writes the pixel there. The
 *	V3D's generator (gen_cp_shaders.c) compiles them as they are; the
 *	generator for Intel's GPU (tools/x64/mesa/gen_igt_shaders.c) turns
 *	each into a compute shader over a rectangle of memory first.
 *
 *	Read in by those files after the NIR headers, a die() of theirs and
 *	the nir_shader_compiler_options 'nir_options' the shaders are built
 *	with.
 */

#ifndef _CP_STAGES_H_
#define _CP_STAGES_H_

/* The draw's buffers */
#define B_SRC		0	/* a window's pixels, or the level a step of the spread reads */
#define B_AUX		1	/* what shows through, spread (T2) */
#define B_COV		2	/* the cover of a round corner, one byte a pixel */

/* ---------------------------------------------------------------- parameters */

/*
 * Every fragment shader takes the frame's place on the screen first; the
 * rest are its own. The numbers are written out for the kernel.
 */
enum {
	P_ORGX, P_ORGY,

	/* piece */
	PP_PITCH = 2, PP_OL, PP_OT, PP_OR, PP_OB, PP_A, PP_RAD, PP_TINT, PP_TSTR,
	PP_WASH, PP_WSTR, PP_UND, PP_UPITCH, PP_SW, PP_SH, PP_NPARAM,

	/* shadow */
	PS_OL = 2, PS_OT, PS_OR, PS_OB, PS_DX, PS_DY, PS_SIZE, PS_DARK, PS_RAD, PS_NPARAM,

	/*
	 * halve, smooth3, smooth5, double: a level of the spread from
	 * another, whose pixels that touch the box are VL..VR - 1 and
	 * VT..VB - 1; smoothed with the weights A, B, M
	 */
	PL_PITCH = 2, PL_VL, PL_VT, PL_VR, PL_VB, PL_A, PL_B, PL_M, PL_NPARAM,

	/* fill, copy, invert: the checks of one thing each */
	PD_COLOUR = 2, PD_PITCH, PD_OL, PD_OT, PD_W, PD_H, PD_NPARAM,

	/* moving */
	PM_PITCH = 2, PM_SL, PM_ST, PM_SR, PM_SB, PM_STEPX, PM_STEPY, PM_OL, PM_OT,
	PM_OR, PM_OB, PM_WL, PM_WT, PM_WR, PM_WB, PM_AW, PM_AF, PM_RAD, PM_TINT,
	PM_TSTR, PM_WASH, PM_WSTR, PM_BWASH, PM_BSTR, PM_BAND, PM_NPARAM
};

static const struct {
	const char	*name;
	int		value;
} param_names[] = {
	{ "CP_P_ORGX", P_ORGX }, { "CP_P_ORGY", P_ORGY },
	{ "CP_PP_PITCH", PP_PITCH }, { "CP_PP_OL", PP_OL }, { "CP_PP_OT", PP_OT },
	{ "CP_PP_OR", PP_OR }, { "CP_PP_OB", PP_OB }, { "CP_PP_A", PP_A },
	{ "CP_PP_RAD", PP_RAD }, { "CP_PP_TINT", PP_TINT }, { "CP_PP_TSTR", PP_TSTR },
	{ "CP_PP_WASH", PP_WASH }, { "CP_PP_WSTR", PP_WSTR }, { "CP_PP_UND", PP_UND },
	{ "CP_PP_UPITCH", PP_UPITCH }, { "CP_PP_SW", PP_SW }, { "CP_PP_SH", PP_SH },
	{ "CP_PS_OL", PS_OL }, { "CP_PS_OT", PS_OT }, { "CP_PS_OR", PS_OR },
	{ "CP_PS_OB", PS_OB }, { "CP_PS_DX", PS_DX }, { "CP_PS_DY", PS_DY },
	{ "CP_PS_SIZE", PS_SIZE }, { "CP_PS_DARK", PS_DARK }, { "CP_PS_RAD", PS_RAD },
	{ "CP_PL_PITCH", PL_PITCH }, { "CP_PL_VL", PL_VL }, { "CP_PL_VT", PL_VT },
	{ "CP_PL_VR", PL_VR }, { "CP_PL_VB", PL_VB }, { "CP_PL_A", PL_A },
	{ "CP_PL_B", PL_B }, { "CP_PL_M", PL_M },
	{ "CP_PM_PITCH", PM_PITCH }, { "CP_PM_SL", PM_SL }, { "CP_PM_ST", PM_ST },
	{ "CP_PM_SR", PM_SR }, { "CP_PM_SB", PM_SB }, { "CP_PM_STEPX", PM_STEPX },
	{ "CP_PM_STEPY", PM_STEPY }, { "CP_PM_OL", PM_OL }, { "CP_PM_OT", PM_OT },
	{ "CP_PM_OR", PM_OR }, { "CP_PM_OB", PM_OB }, { "CP_PM_WL", PM_WL },
	{ "CP_PM_WT", PM_WT }, { "CP_PM_WR", PM_WR }, { "CP_PM_WB", PM_WB },
	{ "CP_PM_AW", PM_AW }, { "CP_PM_AF", PM_AF }, { "CP_PM_RAD", PM_RAD },
	{ "CP_PM_TINT", PM_TINT }, { "CP_PM_TSTR", PM_TSTR }, { "CP_PM_WASH", PM_WASH },
	{ "CP_PM_WSTR", PM_WSTR }, { "CP_PM_BWASH", PM_BWASH }, { "CP_PM_BSTR", PM_BSTR },
	{ "CP_PM_BAND", PM_BAND },
	{ "CP_PD_COLOUR", PD_COLOUR }, { "CP_PD_PITCH", PD_PITCH }, { "CP_PD_OL", PD_OL },
	{ "CP_PD_OT", PD_OT }, { "CP_PD_W", PD_W }, { "CP_PD_H", PD_H },
};

/* ---------------------------------------------------------------- building blocks */

static nir_def *P( nir_builder *b, int i )
{
	return nir_load_uniform(b, 1, 32, nir_imm_int(b, 0), .base = i * 4, .range = 4,
				.dest_type = nir_type_uint32);
}

/* A word of a buffer, by its byte offset */
static nir_def *ld( nir_builder *b, int buf, nir_def *off )
{
	return nir_load_ssbo(b, 1, 32, nir_imm_int(b, buf), off,
			     .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER,
			     .align_mul = 4, .align_offset = 0);
}

static nir_def *clampi( nir_builder *b, nir_def *v, nir_def *lo, nir_def *hi_incl )
{
	return nir_imin(b, nir_imax(b, v, lo), hi_incl);
}

static nir_def *ch( nir_builder *b, nir_def *w, int i )
{
	return nir_iand_imm(b, nir_ushr_imm(b, w, 8 * i), 0xFF);
}

static nir_def *pack4( nir_builder *b, nir_def *c0, nir_def *c1, nir_def *c2, nir_def *c3 )
{
	return nir_ior(b, nir_ior(b, c0, nir_ishl_imm(b, c1, 8)),
		       nir_ior(b, nir_ishl_imm(b, c2, 16), nir_ishl_imm(b, c3, 24)));
}

/* t / 255 rounded down, for t up to 65280 (checked in div255_check); the most it is used for is 255 * 255 + 127 */
static unsigned div255_c( unsigned t )
{
	return ( t + 1 + ( t >> 8 ) ) >> 8;
}

static nir_def *div255( nir_builder *b, nir_def *t )
{
	return nir_ushr_imm(b, nir_iadd(b, nir_iadd_imm(b, t, 1), nir_ushr_imm(b, t, 8)), 8);
}

static void div255_check( void )
{
	unsigned t;

	for ( t = 0; t <= 65280; t++ ) {
		if ( div255_c(t) != t / 255 ) die("t / 255 is wrong at %u", t);
	}
}

static nir_def *mul( nir_builder *b, nir_def *x, nir_def *y )
{
	return nir_umul24(b, x, y);	/* both below 2^24 wherever it is used */
}

/* over laid at a (0..255) over under, channel by channel and rounded; the top byte 0 */
static nir_def *over_of( nir_builder *b, nir_def *under, nir_def *over, nir_def *a )
{
	nir_def	*na = nir_isub(b, nir_imm_int(b, 255), a);
	nir_def	*c[3];
	int	i;

	for ( i = 0; i < 3; i++ ) {
		nir_def	*t = nir_iadd(b, mul(b, ch(b, over, i), a), mul(b, ch(b, under, i), na));

		c[i] = div255(b, nir_iadd_imm(b, t, 127));
	}
	return pack4(b, c[0], c[1], c[2], nir_imm_int(b, 0));
}

/* A colour taken toward black by a (0..255), rounded; the top byte 0 */
static nir_def *darker( nir_builder *b, nir_def *col, nir_def *a )
{
	nir_def	*k = nir_isub(b, nir_imm_int(b, 255), a);
	nir_def	*c[3];
	int	i;

	for ( i = 0; i < 3; i++ ) {
		c[i] = div255(b, nir_iadd_imm(b, mul(b, ch(b, col, i), k), 127));
	}
	return pack4(b, c[0], c[1], c[2], nir_imm_int(b, 0));
}

/* ---------------------------------------------------------------- fragment shader frame */

typedef struct {
	nir_builder	b;
	nir_variable	*color;		/* the render target, read for what is behind */
	nir_def		*x, *y;		/* the pixel on the screen */
} FS;

static void fs_begin( FS *f, const char *name, int nssbo )
{
	nir_variable	*pos;
	nir_def		*p;

	f->b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &nir_options, "%s", name);
	f->b.shader->info.num_ssbos = nssbo;

	f->color = nir_variable_create(f->b.shader, nir_var_shader_out, glsl_vec4_type(), "color");
	f->color->data.location = FRAG_RESULT_DATA0;
	f->color->data.driver_location = 0;
	f->color->data.fb_fetch_output = true;

	pos = nir_variable_create(f->b.shader, nir_var_shader_in, glsl_vec4_type(), "pos");
	pos->data.location = VARYING_SLOT_POS;
	pos->data.driver_location = 0;

	p = nir_load_var(&f->b, pos);
	f->x = nir_iadd(&f->b, nir_f2i32(&f->b, nir_channel(&f->b, p, 0)), P(&f->b, P_ORGX));
	f->y = nir_iadd(&f->b, nir_f2i32(&f->b, nir_channel(&f->b, p, 1)), P(&f->b, P_ORGY));
}

/* What is behind the pixel, as a word of four bytes */
static nir_def *fs_behind( FS *f )
{
	nir_builder	*b = &f->b;
	nir_def		*v = nir_load_var(b, f->color);
	nir_def		*c[4];
	int		i;

	for ( i = 0; i < 4; i++ ) {
		c[i] = nir_f2u32(b, nir_fadd_imm(b, nir_fmul_imm(b, nir_channel(b, v, i), 255.0), 0.5));
		c[i] = nir_umin(b, c[i], nir_imm_int(b, 255));
	}
	return pack4(b, c[0], c[1], c[2], c[3]);
}

static void fs_out( FS *f, nir_def *w )
{
	nir_builder	*b = &f->b;
	nir_def		*c[4];
	int		i;

	for ( i = 0; i < 4; i++ ) {
		c[i] = nir_fmul_imm(b, nir_u2f32(b, ch(b, w, i)), 1.0 / 255.0);
	}
	nir_store_var(b, f->color, nir_vec(b, c, 4), 0xF);
}

/* ---------------------------------------------------------------- corners */

/*
 * The cover of the pixel (x, y) of a rectangle (l, t, r, b) whose corners
 * are of radius rad: 'inrow' whether the row is one of a corner's, and
 * the cover itself (255 outside the corners' columns). The table holds
 * rad x rad bytes from the corner in.
 */
static void cover_at( nir_builder *b, nir_def *x, nir_def *y, nir_def *l, nir_def *t,
		      nir_def *r, nir_def *bot, nir_def *rad, nir_def **inrow, nir_def **cov )
{
	nir_def	*ey = nir_imin(b, nir_isub(b, y, t), nir_isub(b, nir_iadd_imm(b, bot, -1), y));
	nir_def	*ex = nir_imin(b, nir_isub(b, x, l), nir_isub(b, nir_iadd_imm(b, r, -1), x));
	nir_def	*rm1 = nir_imax(b, nir_iadd_imm(b, rad, -1), nir_imm_int(b, 0));
	nir_def	*idx, *w, *byte;

	*inrow = nir_iand(b, nir_ilt(b, nir_imm_int(b, 0), rad), nir_ilt(b, ey, rad));
	idx = nir_iadd(b, mul(b, clampi(b, ey, nir_imm_int(b, 0), rm1), rad),
		       clampi(b, ex, nir_imm_int(b, 0), rm1));
	w = ld(b, B_COV, nir_iand_imm(b, idx, ~3));
	byte = nir_iand_imm(b, nir_ushr(b, w, nir_ishl_imm(b, nir_iand_imm(b, idx, 3), 3)), 0xFF);
	*cov = nir_bcsel(b, nir_ilt(b, ex, rad), byte, nir_imm_int(b, 255));
}

/* ---------------------------------------------------------------- the shaders */

/*
 * A part of a window mixed over what is behind it (compose.c lay_piece):
 * its pixel, made less solid in a round corner (and not laid where that
 * leaves nothing), tinted and washed, then laid as it is where it is
 * solid and mixed over what shows through -- spread when there is a
 * spread -- where it is not.
 */
static nir_shader *fs_piece( void )
{
	FS		f;
	nir_builder	*b;
	nir_def		*ol, *ot, *orr, *ob, *cx, *cy, *c, *a0, *a, *inrow, *cov, *ac;
	nir_def		*tstr, *wstr, *und, *uw, *base, *mixed;

	fs_begin(&f, "cp_piece", 3);
	b = &f.b;
	ol = P(b, PP_OL);  ot = P(b, PP_OT);  orr = P(b, PP_OR);  ob = P(b, PP_OB);

	cx = clampi(b, f.x, ol, nir_iadd_imm(b, orr, -1));
	cy = clampi(b, f.y, ot, nir_iadd_imm(b, ob, -1));
	c = ld(b, B_SRC, nir_iadd(b, mul(b, nir_isub(b, cy, ot), P(b, PP_PITCH)),
				  nir_ishl_imm(b, nir_isub(b, cx, ol), 2)));

	a0 = P(b, PP_A);
	cover_at(b, f.x, f.y, ol, ot, orr, ob, P(b, PP_RAD), &inrow, &cov);
	ac = div255(b, nir_iadd_imm(b, mul(b, a0, cov), 127));
	a = nir_bcsel(b, inrow, ac, a0);
	nir_terminate_if(b, nir_iand(b, inrow, nir_ieq_imm(b, a, 0)));

	tstr = P(b, PP_TSTR);
	c = nir_bcsel(b, nir_ine_imm(b, tstr, 0), over_of(b, c, P(b, PP_TINT), tstr), c);
	wstr = P(b, PP_WSTR);
	c = nir_bcsel(b, nir_ine_imm(b, wstr, 0), over_of(b, c, P(b, PP_WASH), wstr), c);

	und = nir_ine_imm(b, P(b, PP_UND), 0);
	uw = ld(b, B_AUX, nir_iadd(b, mul(b, clampi(b, f.y, nir_imm_int(b, 0), nir_iadd_imm(b, P(b, PP_SH), -1)),
					  P(b, PP_UPITCH)),
				  nir_ishl_imm(b, clampi(b, f.x, nir_imm_int(b, 0),
							 nir_iadd_imm(b, P(b, PP_SW), -1)), 2)));
	base = nir_bcsel(b, und, uw, fs_behind(&f));
	mixed = over_of(b, base, c, a);
	fs_out(&f, nir_bcsel(b, nir_uge_imm(b, a, 255), c, mixed));
	return b->shader;
}

/* An integer square root, rounded down, of v below 2^24 */
static nir_def *isqrt( nir_builder *b, nir_def *v )
{
	nir_def	*r = nir_f2u32(b, nir_fsqrt(b, nir_u2f32(b, v)));
	int	i;

	for ( i = 0; i < 2; i++ ) {
		nir_def	*r1 = nir_iadd_imm(b, r, 1);

		r = nir_bcsel(b, nir_ult(b, v, nir_imul(b, r, r)), nir_iadd_imm(b, r, -1), r);
		r = nir_bcsel(b, nir_uge(b, v, nir_imul(b, r1, r1)), r1, r);
	}
	return r;
}

/*
 * The shadow (compose.c cp_shadow and shd_at): the window's outline
 * thrown by the offset; all of the darkness inside it, then less and
 * less until none at 'size' away, worked out in sixteenths. Not where
 * the window stands but in the rows of its round corners, where it is
 * covered only in part.
 */
static nir_shader *fs_shadow( void )
{
	FS		f;
	nir_builder	*b;
	nir_def		*x, *y, *ol, *ot, *orr, *ob, *rad, *inrow_w, *inwin, *cr, *cseg;
	nir_def		*sl, *st, *sr, *sb, *l, *r, *t, *bt, *cx, *cy, *dx, *dy, *d16, *size16;
	nir_def		*dark, *a, *covrow, *cov, *ag;

	fs_begin(&f, "cp_shadow", 3);
	b = &f.b;
	x = f.x;  y = f.y;
	ol = P(b, PS_OL);  ot = P(b, PS_OT);  orr = P(b, PS_OR);  ob = P(b, PS_OB);
	rad = P(b, PS_RAD);

	inrow_w = nir_iand(b, nir_ige(b, y, ot), nir_ilt(b, y, ob));
	inwin = nir_iand(b, inrow_w, nir_iand(b, nir_ige(b, x, ol), nir_ilt(b, x, orr)));
	cover_at(b, x, y, ol, ot, orr, ob, rad, &covrow, &cov);
	cr = nir_iand(b, inrow_w, covrow);
	cseg = nir_iand(b, cr, nir_ior(b, nir_ilt(b, x, nir_iadd(b, ol, rad)),
				       nir_ige(b, x, nir_isub(b, orr, rad))));
	nir_terminate_if(b, nir_iand(b, inwin, nir_inot(b, cseg)));

	sl = nir_iadd(b, ol, P(b, PS_DX));  sr = nir_iadd(b, orr, P(b, PS_DX));
	st = nir_iadd(b, ot, P(b, PS_DY));  sb = nir_iadd(b, ob, P(b, PS_DY));
	l = nir_iadd(b, sl, rad);
	r = nir_isub(b, nir_iadd_imm(b, sr, -1), rad);
	t = nir_iadd(b, st, rad);
	bt = nir_isub(b, nir_iadd_imm(b, sb, -1), rad);
	cx = nir_bcsel(b, nir_ilt(b, x, l), l, nir_bcsel(b, nir_ilt(b, r, x), r, x));
	cy = nir_bcsel(b, nir_ilt(b, y, t), t, nir_bcsel(b, nir_ilt(b, bt, y), bt, y));
	dx = nir_iabs(b, nir_isub(b, x, cx));
	dy = nir_iabs(b, nir_isub(b, y, cy));
	d16 = nir_bcsel(b, nir_ieq_imm(b, dx, 0), nir_ishl_imm(b, dy, 4),
			nir_bcsel(b, nir_ieq_imm(b, dy, 0), nir_ishl_imm(b, dx, 4),
				  isqrt(b, nir_ishl_imm(b, nir_iadd(b, mul(b, dx, dx), mul(b, dy, dy)), 8))));
	d16 = nir_isub(b, d16, nir_ishl_imm(b, rad, 4));
	size16 = nir_ishl_imm(b, P(b, PS_SIZE), 4);
	dark = P(b, PS_DARK);
	ag = nir_udiv(b, mul(b, dark, nir_isub(b, size16, d16)), nir_imax(b, size16, nir_imm_int(b, 1)));
	a = nir_bcsel(b, nir_ige(b, nir_imm_int(b, 0), d16), dark,
		      nir_bcsel(b, nir_ige(b, d16, size16), nir_imm_int(b, 0), ag));
	/* where the window covers part of the pixel, the shadow shows through the rest */
	a = nir_bcsel(b, inwin, div255(b, mul(b, a, nir_isub(b, nir_imm_int(b, 255), cov))), a);
	nir_terminate_if(b, nir_ieq_imm(b, a, 0));
	fs_out(&f, darker(b, fs_behind(&f), a));
	return b->shader;
}

/*
 * The spread (compose.c cp_spread_plan): a level of what shows through
 * worked out from another, one pixel to each fragment, the level read
 * being B_SRC, its pixel (x, y) the word at y * PITCH + 4 x. A read is
 * held to the pixels of that level that touch the box.
 */
static nir_def *lv_x( nir_builder *b, nir_def *x )
{
	return nir_ishl_imm(b, clampi(b, x, P(b, PL_VL), nir_iadd_imm(b, P(b, PL_VR), -1)), 2);
}

static nir_def *lv_y( nir_builder *b, nir_def *y )
{
	return mul(b, clampi(b, y, P(b, PL_VT), nir_iadd_imm(b, P(b, PL_VB), -1)), P(b, PL_PITCH));
}

/*
 * Halving and doubling sum blue and red together in the two halves of
 * one word, and green alone: no sum of theirs passes 16 times 255.
 * Each is rounded once, by 2^s, as compose.c's unlanes does.
 */
static void rbg_add( nir_builder *b, nir_def *rb1, nir_def *g1, nir_def **rb, nir_def **g )
{
	*rb = ( *rb == NULL ) ? rb1 : nir_iadd(b, *rb, rb1);
	*g = ( *g == NULL ) ? g1 : nir_iadd(b, *g, g1);
}

static nir_def *rbg_round( nir_builder *b, nir_def *rb, nir_def *g, int s )
{
	rb = nir_iand_imm(b, nir_ushr_imm(b, nir_iadd_imm(b, rb, ( 1 << ( s - 1 ) ) * 0x00010001), s), 0x00FF00FF);
	g = nir_ushr_imm(b, nir_iadd_imm(b, g, 1 << ( s - 1 )), s);
	return nir_ior(b, rb, nir_ishl_imm(b, g, 8));
}

/* x three times when 'three', else as it is */
static nir_def *times3_if( nir_builder *b, nir_def *x, nir_def *three )
{
	return nir_bcsel(b, three, nir_iadd(b, x, nir_ishl_imm(b, x, 1)), x);
}

/*
 * Halving: the pixel (x, y) from the four by four of the level below
 * from (2x - 1, 2y - 1), weighed 1 3 3 1 each way, of 64
 */
static nir_shader *fs_halve( void )
{
	static const unsigned	wt[4] = { 1, 3, 3, 1 };
	FS			f;
	nir_builder		*b;
	nir_def			*xo[4], *yo[4], *rb = NULL, *g = NULL;
	int			i, j;

	fs_begin(&f, "cp_halve", 1);
	b = &f.b;
	for ( i = 0; i < 4; i++ ) {
		xo[i] = lv_x(b, nir_iadd_imm(b, nir_ishl_imm(b, f.x, 1), i - 1));
		yo[i] = lv_y(b, nir_iadd_imm(b, nir_ishl_imm(b, f.y, 1), i - 1));
	}
	for ( j = 0; j < 4; j++ ) {
		for ( i = 0; i < 4; i++ ) {
			nir_def		*v = ld(b, B_SRC, nir_iadd(b, yo[j], xo[i]));
			nir_def		*vrb = nir_iand_imm(b, v, 0x00FF00FF), *vg = ch(b, v, 1);
			unsigned	w = wt[i] * wt[j];

			if ( w != 1 ) {
				vrb = mul(b, vrb, nir_imm_int(b, (int)w));
				vg = mul(b, vg, nir_imm_int(b, (int)w));
			}
			rbg_add(b, vrb, vg, &rb, &g);
		}
	}
	fs_out(&f, rbg_round(b, rb, g, 6));
	return b->shader;
}

/*
 * Doubling: the pixel (x, y) from the two by two of the level above
 * nearest its centre, from (x / 2, y / 2) toward the side the pixel lies
 * on, the nearer weighed 3 and the further 1 each way, of 16
 */
static nir_shader *fs_double( void )
{
	FS		f;
	nir_builder	*b;
	nir_def		*xodd, *yodd, *xa, *xb, *ya, *yb, *rb = NULL, *g = NULL;
	nir_def		*hrb[2], *hg[2];
	int		j;

	fs_begin(&f, "cp_double", 1);
	b = &f.b;
	xodd = nir_ine_imm(b, nir_iand_imm(b, f.x, 1), 0);
	yodd = nir_ine_imm(b, nir_iand_imm(b, f.y, 1), 0);
	xa = nir_isub(b, nir_ishr_imm(b, f.x, 1), nir_b2i32(b, nir_inot(b, xodd)));
	ya = nir_isub(b, nir_ishr_imm(b, f.y, 1), nir_b2i32(b, nir_inot(b, yodd)));
	xb = lv_x(b, nir_iadd_imm(b, xa, 1));
	yb = lv_y(b, nir_iadd_imm(b, ya, 1));
	xa = lv_x(b, xa);
	ya = lv_y(b, ya);

	/* along each of the two rows: the left weighed 3 on an odd column, the right on an even */
	for ( j = 0; j < 2; j++ ) {
		nir_def	*row = ( j == 0 ) ? ya : yb;
		nir_def	*va = ld(b, B_SRC, nir_iadd(b, row, xa)), *vb = ld(b, B_SRC, nir_iadd(b, row, xb));
		nir_def	*arb = nir_iand_imm(b, va, 0x00FF00FF), *ag = ch(b, va, 1);
		nir_def	*brb = nir_iand_imm(b, vb, 0x00FF00FF), *bg = ch(b, vb, 1);

		hrb[j] = nir_iadd(b, times3_if(b, arb, xodd), times3_if(b, brb, nir_inot(b, xodd)));
		hg[j] = nir_iadd(b, times3_if(b, ag, xodd), times3_if(b, bg, nir_inot(b, xodd)));
	}
	/* and down: the upper row weighed 3 on an odd row, the lower on an even */
	rbg_add(b, times3_if(b, hrb[0], yodd), times3_if(b, hg[0], yodd), &rb, &g);
	rbg_add(b, times3_if(b, hrb[1], nir_inot(b, yodd)), times3_if(b, hg[1], nir_inot(b, yodd)), &rb, &g);
	fs_out(&f, rbg_round(b, rb, g, 4));
	return b->shader;
}

/*
 * Smoothing: the pixel (x, y) from the (2 taps + 1) square round it at
 * its own level, weighed A B M B A each way (taps 2) or B M B (taps 1),
 * of 64 each way; each channel summed alone, since a sum reaches 4096
 * times 255, and rounded once
 */
static nir_shader *fs_smooth( const char *name, int taps )
{
	FS		f;
	nir_builder	*b;
	nir_def		*wt[5], *xo[5], *s[3] = { NULL, NULL, NULL };
	int		i, j, k;

	fs_begin(&f, name, 1);
	b = &f.b;
	wt[0] = wt[4] = P(b, PL_A);
	wt[1] = wt[3] = P(b, PL_B);
	wt[2] = P(b, PL_M);
	for ( i = 2 - taps; i <= 2 + taps; i++ ) {
		xo[i] = lv_x(b, nir_iadd_imm(b, f.x, i - 2));
	}
	for ( j = 2 - taps; j <= 2 + taps; j++ ) {
		nir_def	*yo = lv_y(b, nir_iadd_imm(b, f.y, j - 2)), *h[3] = { NULL, NULL, NULL };

		for ( i = 2 - taps; i <= 2 + taps; i++ ) {
			nir_def	*v = ld(b, B_SRC, nir_iadd(b, yo, xo[i]));

			for ( k = 0; k < 3; k++ ) {
				nir_def	*t = mul(b, ch(b, v, k), wt[i]);

				h[k] = ( h[k] == NULL ) ? t : nir_iadd(b, h[k], t);
			}
		}
		for ( k = 0; k < 3; k++ ) {
			nir_def	*t = mul(b, h[k], wt[j]);

			s[k] = ( s[k] == NULL ) ? t : nir_iadd(b, s[k], t);
		}
	}
	for ( k = 0; k < 3; k++ ) {
		s[k] = nir_ushr_imm(b, nir_iadd_imm(b, s[k], 2048), 12);
	}
	fs_out(&f, pack4(b, s[0], s[1], s[2], nir_imm_int(b, 0)));
	return b->shader;
}

static nir_shader *fs_smooth3( void )
{
	return fs_smooth("cp_smooth3", 1);
}

static nir_shader *fs_smooth5( void )
{
	return fs_smooth("cp_smooth5", 2);
}

/*
 * A window opening or closing (compose.c cp_moving): each pixel of where
 * it is shown taken from the pixel of the window it falls on, the work
 * area by how solid it is and the frame by how solid the frame is, the
 * frame tinted and washed, round corners on where it is shown.
 */
static nir_shader *fs_moving( void )
{
	FS		f;
	nir_builder	*b;
	nir_def		*x, *y, *sl, *st, *wy, *wx, *ay, *ax, *c, *workrow, *bandrow, *inwork;
	nir_def		*a, *inrow, *cov, *tstr, *bstr, *wstr, *tinted, *washed, *frame;

	fs_begin(&f, "cp_moving", 3);
	b = &f.b;
	x = f.x;  y = f.y;
	sl = P(b, PM_SL);  st = P(b, PM_ST);
	wy = nir_ushr_imm(b, nir_imul(b, nir_isub(b, y, st), P(b, PM_STEPY)), 16);
	wx = nir_ushr_imm(b, nir_imul(b, nir_isub(b, x, sl), P(b, PM_STEPX)), 16);
	wy = clampi(b, wy, nir_imm_int(b, 0), nir_iadd_imm(b, nir_isub(b, P(b, PM_OB), P(b, PM_OT)), -1));
	wx = clampi(b, wx, nir_imm_int(b, 0), nir_iadd_imm(b, nir_isub(b, P(b, PM_OR), P(b, PM_OL)), -1));
	ay = nir_iadd(b, P(b, PM_OT), wy);
	ax = nir_iadd(b, P(b, PM_OL), wx);
	c = ld(b, B_SRC, nir_iadd(b, mul(b, wy, P(b, PM_PITCH)), nir_ishl_imm(b, wx, 2)));

	workrow = nir_iand(b, nir_ige(b, ay, P(b, PM_WT)), nir_ilt(b, ay, P(b, PM_WB)));
	bandrow = nir_iand(b, nir_ilt(b, ay, P(b, PM_WT)), nir_ine_imm(b, P(b, PM_BAND), 0));
	inwork = nir_iand(b, workrow, nir_iand(b, nir_ige(b, ax, P(b, PM_WL)), nir_ilt(b, ax, P(b, PM_WR))));
	a = nir_bcsel(b, inwork, P(b, PM_AW), P(b, PM_AF));
	cover_at(b, x, y, sl, st, P(b, PM_SR), P(b, PM_SB), P(b, PM_RAD), &inrow, &cov);
	a = nir_bcsel(b, inrow, div255(b, nir_iadd_imm(b, mul(b, a, cov), 127)), a);
	nir_terminate_if(b, nir_ieq_imm(b, a, 0));

	tstr = P(b, PM_TSTR);
	tinted = nir_bcsel(b, nir_ine_imm(b, tstr, 0), over_of(b, c, P(b, PM_TINT), tstr), c);
	bstr = P(b, PM_BSTR);
	wstr = P(b, PM_WSTR);
	washed = nir_bcsel(b, nir_iand(b, bandrow, nir_ine_imm(b, bstr, 0)),
			   over_of(b, tinted, P(b, PM_BWASH), bstr),
			   nir_bcsel(b, nir_ine_imm(b, wstr, 0), over_of(b, tinted, P(b, PM_WASH), wstr),
				     tinted));
	frame = washed;
	c = nir_bcsel(b, inwork, c, frame);
	fs_out(&f, nir_bcsel(b, nir_uge_imm(b, a, 255), c, over_of(b, fs_behind(&f), c, a)));
	return b->shader;
}

/*
 * Three shaders that do one thing each, for the check on the board to
 * tell which part of the pipeline goes wrong when a scene does: a
 * colour from a parameter (the draws reach the pixels), a pixel read
 * from memory (the TMU), what is behind turned over (the tile buffer
 * read back).
 */
static nir_shader *fs_fill( void )
{
	FS	f;

	fs_begin(&f, "cp_fill", 0);
	fs_out(&f, P(&f.b, PD_COLOUR));
	return f.b.shader;
}

static nir_shader *fs_copy( void )
{
	FS		f;
	nir_builder	*b;
	nir_def		*cx, *cy;

	fs_begin(&f, "cp_copy", 1);
	b = &f.b;
	cx = clampi(b, nir_isub(b, f.x, P(b, PD_OL)), nir_imm_int(b, 0), nir_iadd_imm(b, P(b, PD_W), -1));
	cy = clampi(b, nir_isub(b, f.y, P(b, PD_OT)), nir_imm_int(b, 0), nir_iadd_imm(b, P(b, PD_H), -1));
	fs_out(&f, ld(b, B_SRC, nir_iadd(b, mul(b, cy, P(b, PD_PITCH)), nir_ishl_imm(b, cx, 2))));
	return b->shader;
}

static nir_shader *fs_invert( void )
{
	FS	f;

	fs_begin(&f, "cp_invert", 0);
	fs_out(&f, nir_ixor(&f.b, fs_behind(&f), nir_imm_int(&f.b, 0x00FFFFFF)));
	return f.b.shader;
}

#endif /* _CP_STAGES_H_ */
