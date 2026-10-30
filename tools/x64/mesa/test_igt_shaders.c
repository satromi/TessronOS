/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	test_igt_shaders.c
 *	The compositing shaders for Intel's render engine checked on the
 *	host, before any machine runs them (docs/tessronos/13-gpu.md
 *	13.11.14).
 *
 *	Each shader is built as gen_igt_shaders.c builds it -- cp_stages.h's
 *	fragment shader made a compute shader -- and run here by an
 *	interpreter of NIR, one invocation at a time: its thread group, its
 *	place in the group, its push constants and memory by 64 bit address
 *	(the test's buffers placed above 4 GB, so that the addresses' upper
 *	halves matter). Random scenes are laid both with compose.c (built
 *	for the host, tools/mesa/hostshim) and with the kernel's plan
 *	(device/gpu/intel/igt_rplan.c) run through the shaders, and the two
 *	screens compared to the top byte.
 *
 *	The rectangles of one group (from one with a barrier to the next) may
 *	run in any order on the GPU, all at once: here every read in a group
 *	sees memory as it was when the group began, and two writes of one
 *	word in a group that differ are a fault, so that a plan that relies on
 *	an order it does not ask for shows. Every read is checked to fall
 *	inside a buffer, every write to land in its rectangle of its
 *	destination.
 *
 *	What this does not check is Mesa's translation into the engine's code
 *	and the hardware; the check at start-up on the machine does.
 */

#define main gen_igt_main
#include "gen_igt_shaders.c"
#undef main

#include "compiler/nir/nir_constant_expressions.h"
#include "hr.h"

/* ---------------------------------------------------------------- memory */

typedef struct {
	uint64_t	base;
	uint8_t		*p;
	uint64_t	size;
	int		wr;		/* a shader may write it */
	uint8_t		*snap;		/* as it was when the group began (wr) */
	uint32_t	*wval;		/* written in the group: the value, and whether */
	uint8_t		*wmark;
} REGION;

#define NREGION		64
static REGION	region[NREGION];
static int	nregion;
static long	oob, bad_writes, conflicts, faults;

static void fault( const char *what, uint64_t a )
{
	if ( faults++ < 10 ) fprintf(stderr, "  %s at %llx\n", what, (unsigned long long)a);
}

static void region_set( uint64_t base, void *p, uint64_t size, int wr )
{
	REGION	*r;

	if ( nregion == NREGION ) die("regions");
	r = &region[nregion++];
	r->base = base;
	r->p = p;
	r->size = size;
	r->wr = wr;
	if ( wr ) {
		r->snap = malloc(size);
		r->wval = calloc(size / 4, 4);
		r->wmark = calloc(size / 4, 1);
	}
}

static REGION *region_of( uint64_t addr )
{
	int	i;

	for ( i = 0; i < nregion; i++ ) {
		REGION	*r = &region[i];

		if ( addr >= r->base && addr + 4 <= r->base + r->size ) return r;
	}
	return NULL;
}

/* The address the GPU sees of the test's memory */
static uint64_t gpu_of( const void *p, uint64_t len )
{
	int	i;

	for ( i = 0; i < nregion; i++ ) {
		REGION	*r = &region[i];

		if ( (const uint8_t *)p >= r->p && (const uint8_t *)p + len <= r->p + r->size ) {
			return r->base + (uint64_t)( (const uint8_t *)p - r->p );
		}
	}
	return 0;
}

/* A new group: what may be written, as it is now; nothing written in it yet */
static void group_begin( void )
{
	int	i;

	for ( i = 0; i < nregion; i++ ) {
		if ( !region[i].wr ) continue;
		memcpy(region[i].snap, region[i].p, region[i].size);
		memset(region[i].wmark, 0, region[i].size / 4);
	}
}

static uint32_t mem_read( uint64_t addr )
{
	REGION		*r = region_of(addr);
	uint32_t	v;

	if ( r == NULL || ( addr & 3 ) != 0 ) {
		oob++;
		fault("a read outside the buffers", addr);
		return 0xDEADBEEF;
	}
	memcpy(&v, ( r->wr ? r->snap : r->p ) + ( addr - r->base ), 4);
	return v;
}

/* A shader's write, which may go only to 'want', the word of its pixel in its destination */
static void mem_write( uint64_t addr, uint32_t v, uint64_t want )
{
	REGION		*r = region_of(addr);
	uint64_t	i;

	if ( r == NULL || !r->wr || addr != want ) {
		bad_writes++;
		fault("a write astray", addr);
		return;
	}
	i = ( addr - r->base ) / 4;
	if ( r->wmark[i] && r->wval[i] != v ) {
		conflicts++;
		fault("two writes of one word in a group that differ", addr);
	}
	r->wmark[i] = 1;
	r->wval[i] = v;
	memcpy(r->p + ( addr - r->base ), &v, 4);
}

/* ---------------------------------------------------------------- the interpreter */

typedef struct {
	uint32_t		wx, wy, lane;
	const uint32_t		*push;
	uint64_t		want;		/* where the pixel's word is */
	int			wrote;
} INV;

enum { J_NONE, J_BREAK, J_HALT };

typedef struct {
	nir_shader		*s;
	nir_function_impl	*impl;
	nir_const_value		(*val)[NIR_MAX_VEC_COMPONENTS];
	nir_block		*prev;
} RUN;

static int exec_list( RUN *r, struct exec_list *list, INV *iv );

static void exec_alu( RUN *r, nir_alu_instr *alu )
{
	nir_const_value	src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
	nir_const_value	*srcs[NIR_ALU_MAX_INPUTS];
	unsigned	bit_size = 0, i, j;

	memset(src, 0, sizeof(src));
	if ( !nir_alu_type_get_type_size(nir_op_infos[alu->op].output_type) ) bit_size = alu->def.bit_size;
	for ( i = 0; i < nir_op_infos[alu->op].num_inputs; i++ ) {
		if ( bit_size == 0 && !nir_alu_type_get_type_size(nir_op_infos[alu->op].input_types[i]) ) {
			bit_size = alu->src[i].src.ssa->bit_size;
		}
		for ( j = 0; j < nir_ssa_alu_instr_src_components(alu, i); j++ ) {
			src[i][j] = r->val[alu->src[i].src.ssa->index][alu->src[i].swizzle[j]];
		}
		srcs[i] = src[i];
	}
	if ( bit_size == 0 ) bit_size = 32;
	memset(r->val[alu->def.index], 0, sizeof(r->val[0]));
	nir_eval_const_opcode(alu->op, r->val[alu->def.index], NULL, alu->def.num_components, bit_size, srcs,
			      r->s->info.float_controls_execution_mode);
}

static int as_bool( nir_const_value v, unsigned bit_size )
{
	return ( bit_size == 1 ) ? v.b : ( v.u32 != 0 );
}

static void exec_intrinsic( RUN *r, nir_intrinsic_instr *in, INV *iv )
{
	nir_const_value	*d = nir_intrinsic_infos[in->intrinsic].has_dest ? r->val[in->def.index] : NULL;
	unsigned	c;

	switch ( in->intrinsic ) {
	case nir_intrinsic_load_push_data_intel: {
		uint32_t	off = nir_intrinsic_base(in) + r->val[in->src[0].ssa->index][0].u32;

		if ( ( off & 3 ) != 0 || off / 4 + in->num_components > NPUSH ) die("a push constant at %u", off);
		for ( c = 0; c < in->num_components; c++ ) d[c].u32 = iv->push[off / 4 + c];
		return;
	}
	case nir_intrinsic_load_workgroup_id:
		d[0].u32 = iv->wx;
		d[1].u32 = iv->wy;
		d[2].u32 = 0;
		return;
	case nir_intrinsic_load_subgroup_invocation:
		d[0].u32 = iv->lane;
		return;
	case nir_intrinsic_load_global:
	case nir_intrinsic_load_global_constant: {
		uint64_t	a = r->val[in->src[0].ssa->index][0].u64;

		if ( in->def.bit_size != 32 ) die("a load of %u bits", in->def.bit_size);
		for ( c = 0; c < in->num_components; c++ ) d[c].u32 = mem_read(a + 4 * c);
		return;
	}
	case nir_intrinsic_store_global: {
		nir_const_value	*v = r->val[in->src[0].ssa->index];
		uint64_t	a = r->val[in->src[1].ssa->index][0].u64;
		unsigned	mask = nir_intrinsic_write_mask(in);

		if ( in->src[0].ssa->bit_size != 32 || in->src[0].ssa->num_components != 1 || mask != 1 ) {
			die("a store of %u x %u bits", in->src[0].ssa->num_components, in->src[0].ssa->bit_size);
		}
		mem_write(a, v[0].u32, iv->want);
		iv->wrote = 1;
		return;
	}
	default:
		die("the interpreter does not know %s", nir_intrinsic_infos[in->intrinsic].name);
	}
}

static int exec_block( RUN *r, nir_block *b, INV *iv )
{
	nir_const_value	tmp[64][NIR_MAX_VEC_COMPONENTS];
	unsigned	n = 0, i;

	nir_foreach_phi(phi, b) {
		nir_phi_src	*src = nir_phi_get_src_from_block(phi, r->prev);

		if ( n >= 64 ) die("phis");
		if ( src != NULL ) memcpy(tmp[n], r->val[src->src.ssa->index], sizeof(tmp[0]));
		else memset(tmp[n], 0, sizeof(tmp[0]));
		n++;
	}
	i = 0;
	nir_foreach_phi(phi, b) memcpy(r->val[phi->def.index], tmp[i++], sizeof(tmp[0]));

	nir_foreach_instr(instr, b) {
		switch ( instr->type ) {
		case nir_instr_type_phi:
			break;
		case nir_instr_type_alu:
			exec_alu(r, nir_instr_as_alu(instr));
			break;
		case nir_instr_type_load_const: {
			nir_load_const_instr	*lc = nir_instr_as_load_const(instr);

			memset(r->val[lc->def.index], 0, sizeof(r->val[0]));
			memcpy(r->val[lc->def.index], lc->value, sizeof(nir_const_value) * lc->def.num_components);
			break;
		}
		case nir_instr_type_undef:
			memset(r->val[nir_instr_as_undef(instr)->def.index], 0, sizeof(r->val[0]));
			break;
		case nir_instr_type_intrinsic:
			exec_intrinsic(r, nir_instr_as_intrinsic(instr), iv);
			break;
		case nir_instr_type_jump: {
			nir_jump_instr	*jp = nir_instr_as_jump(instr);

			r->prev = b;
			if ( jp->type == nir_jump_break ) return J_BREAK;
			if ( jp->type == nir_jump_halt || jp->type == nir_jump_return ) return J_HALT;
			die("a jump of type %d", jp->type);
		}
		default:
			die("the interpreter does not know instruction type %d", instr->type);
		}
	}
	r->prev = b;
	return J_NONE;
}

static int exec_list( RUN *r, struct exec_list *list, INV *iv )
{
	foreach_list_typed(nir_cf_node, node, node, list) {
		int	j = J_NONE;

		switch ( node->type ) {
		case nir_cf_node_block:
			j = exec_block(r, nir_cf_node_as_block(node), iv);
			break;
		case nir_cf_node_if: {
			nir_if	*nif = nir_cf_node_as_if(node);
			int	c = as_bool(r->val[nif->condition.ssa->index][0], nif->condition.ssa->bit_size);

			j = exec_list(r, c ? &nif->then_list : &nif->else_list, iv);
			break;
		}
		default:
			die("cf node %d", node->type);
		}
		if ( j != J_NONE ) return j;
	}
	return J_NONE;
}

static RUN	runs[NFS];

static void runs_make( void )
{
	int	i;

	for ( i = 0; i < NFS; i++ ) {
		RUN	*r = &runs[i];

		r->s = cs_of(fs_make[i]());
		r->impl = nir_shader_get_entrypoint(r->s);
		nir_index_ssa_defs(r->impl);
		r->val = calloc(r->impl->ssa_alloc + 1, sizeof(r->val[0]));
	}
}

/* One rectangle as the GPU runs it: every invocation of every thread group */
static long px_run;

static void run_rect( const HR_RD *d )
{
	uint64_t	dst = (uint64_t)d->par[PI_DST] | ( (uint64_t)d->par[PI_DST + 1] << 32 );
	uint32_t	nx = (uint32_t)( ( d->r - ( d->l & ~( LANES - 1 ) ) + LANES - 1 ) / LANES );
	uint32_t	ny = (uint32_t)( d->b - d->t ), wx, wy, lane;
	RUN		*r = &runs[d->fs];
	INV		iv;

	if ( d->fs < 0 || d->fs >= NFS ) die("shader %d", d->fs);
	for ( wy = 0; wy < ny; wy++ ) {
		for ( wx = 0; wx < nx; wx++ ) {
			for ( lane = 0; lane < LANES; lane++ ) {
				int	x = ( d->l & ~( LANES - 1 ) ) + (int)( wx * LANES + lane ), y = d->t + (int)wy;

				memset(&iv, 0, sizeof(iv));
				iv.wx = wx;
				iv.wy = wy;
				iv.lane = lane;
				iv.push = d->par;
				iv.want = ( x >= d->l && x < d->r ) ? dst + (uint64_t)y * d->par[PI_DPITCH] + (uint64_t)x * 4
								    : ~0ULL;
				r->prev = NULL;
				(void)exec_list(r, &r->impl->body, &iv);
				if ( iv.wrote ) px_run++;
			}
		}
	}
}

/* ---------------------------------------------------------------- the scenes */

#define SW		150
#define SH		100
#define PITCH		( SW * 4 + 64 )		/* rows longer than the screen, as a back buffer's may be */
#define NW		3

#define A_BACK		0x100000000ULL
#define A_GROUND	0x200000000ULL
#define A_T1		0x300000000ULL
#define A_T2		0x400000000ULL
#define A_COV		0x500000000ULL
#define A_WIN		0x600000000ULL

#define T_BYTES		( PITCH * ( SH + ( ( SH - 1 ) >> 1 ) + 1 ) )	/* cp_spread_bytes(PITCH, SH) */

static uint8_t	ground[PITCH * SH], back_c[PITCH * SH], back_g[PITCH * SH], t1[T_BYTES], t2[T_BYTES];
static uint8_t	pix[NW][PITCH * SH];
static uint8_t	cov[HC_ROUND_MAX + 1][HC_ROUND_MAX * HC_ROUND_MAX + 4];
static uint64_t	cov_addr[HC_ROUND_MAX + 1];
static uint32_t	seed = 7;
static HR_RD	rd[2048];
static long	nrect, ngroup;

static uint32_t rnd( void )
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static int rr( int lo, int hi )
{
	return lo + (int)( rnd() % (uint32_t)( hi - lo + 1 ) );
}

static HC_RECT rect( int l, int t, int r, int b )
{
	HC_RECT	x = { l, t, r, b };

	return x;
}

static void fill( uint8_t *p, int n )
{
	int	i;

	for ( i = 0; i < n; i++ ) p[i] = (uint8_t)rnd();
}

/* A random window over pixels k, with a random look */
static void random_win( HC_WIN *w, int k )
{
	int	bar = rr(0, 1) ? rr(4, 14) : 0;
	int	ww = rr(12, SW - 8), hh = rr(bar + 8, SH - 8), l = rr(-8, SW - ww + 4), t = rr(-6, SH - hh + 4);

	memset(w, 0, sizeof(*w));
	if ( l < 0 ) l = 0;
	if ( l + ww > SW ) ww = SW - l;
	w->outer = rect(l, t, l + ww, t + hh);
	w->work = rect(l + 2, t + 2 + bar, l + ww - 3, t + hh - 3);
	w->band = w->outer;
	w->band.bottom = bar ? w->work.top : w->outer.top;
	w->pixels = pix[k];
	w->pitch = PITCH;
	w->a_frame = rr(0, 2) ? 255 : (unsigned)rr(0, 255);
	w->a_work = rr(0, 3) ? 255 : (unsigned)rr(0, 255);
	w->round = rr(0, 2) ? 0 : rr(1, 14);
	w->blur = rr(0, 1) ? 0 : rr(1, HC_BLUR_MAX);
	w->wash = rnd() & 0xFFFFFF;
	w->wash_str = rr(0, 1) ? 0 : (unsigned)rr(0, 255);
	w->bar_wash = rnd() & 0xFFFFFF;
	w->bar_str = rr(0, 1) ? 0 : (unsigned)rr(0, 255);
	w->tint = rnd() & 0xFFFFFF;
	w->tint_str = rr(0, 2) ? 0 : (unsigned)rr(0, 255);
	if ( rr(0, 1) ) {
		w->shd_dx = rr(-8, 8);
		w->shd_dy = rr(-8, 8);
		w->shd_size = rr(0, 10);
		w->shd_dark = (unsigned)rr(1, 255);
	}
	w->shown = w->outer;
	w->fade = 255;
	if ( rr(0, 5) == 0 ) {
		int	sw = ww * rr(180, 255) / 256, sh = hh * rr(180, 255) / 256;

		w->shown.left = l + ( ww - sw ) / 2;
		w->shown.top = t + ( hh - sh ) / 2;
		w->shown.right = w->shown.left + sw;
		w->shown.bottom = w->shown.top + sh;
		w->fade = (unsigned)rr(20, 255);
	}
}

/* The processor's way: the ground over the part, then each window's stages */
static void cpu_part( HC_RECT box, const HC_WIN *w, int nw )
{
	int	y, i;

	for ( y = box.top; y < box.bottom; y++ ) {
		memcpy(back_c + y * PITCH + box.left * 4, ground + y * PITCH + box.left * 4,
		       (size_t)( box.right - box.left ) * 4);
	}
	for ( i = 0; i < nw; i++ ) hc_window(back_c, PITCH, box, SH, &w[i]);
}

/* The shaders' way: the kernel's plan, each group as the GPU may run it */
static int gpu_part( HC_RECT box, const HC_WIN *w, int nw )
{
	int	n = 0, laid, i;

	laid = hr_plan(back_g, PITCH, box, SH, (const uint32_t *)ground, PITCH, w, nw, gpu_of, A_T1, A_T2, cov_addr,
		       rd, 2048, &n);
	if ( laid < 0 ) die("the plan failed (%d)", laid);
	for ( i = 0; i < n; i++ ) {
		if ( i == 0 || rd[i].barrier ) {
			group_begin();
			ngroup++;
		}
		run_rect(&rd[i]);
	}
	nrect += n;
	return laid;
}

int main( int argc, char **argv )
{
	int	nscene = ( argc > 1 ) ? atoi(argv[1]) : 200;
	int	s, k, nw, bad = 0, laid;
	long	diff_px = 0, top_diff = 0;
	HC_WIN	w[NW];

	div255_check();
	glsl_type_singleton_init_or_ref();
	device_init();
	runs_make();

	region_set(A_BACK, back_g, sizeof(back_g), 1);
	region_set(A_GROUND, ground, sizeof(ground), 0);
	region_set(A_T1, t1, sizeof(t1), 1);
	region_set(A_T2, t2, sizeof(t2), 1);
	for ( k = 0; k <= HC_ROUND_MAX; k++ ) {
		const uint8_t	*c = ( k > 0 ) ? hc_corner_cov(k) : NULL;

		memset(cov[k], 0, sizeof(cov[k]));
		if ( c != NULL ) memcpy(cov[k], c, (size_t)( k * k ));
		cov_addr[k] = A_COV + (uint64_t)k * 0x1000;
		region_set(cov_addr[k], cov[k], sizeof(cov[k]), 0);
	}
	for ( k = 0; k < NW; k++ ) region_set(A_WIN + (uint64_t)k * 0x1000000, pix[k], sizeof(pix[k]), 0);

	for ( s = 0; s < nscene; s++ ) {
		HC_RECT	box;
		long	d = 0;
		int	i;

		fill(ground, sizeof(ground));
		for ( k = 0; k < NW; k++ ) fill(pix[k], sizeof(pix[k]));
		fill(back_c, sizeof(back_c));
		memcpy(back_g, back_c, sizeof(back_c));
		fill(t1, sizeof(t1));
		fill(t2, sizeof(t2));
		nw = rr(0, NW);
		for ( k = 0; k < nw; k++ ) random_win(&w[k], k);
		if ( rr(0, 2) == 0 ) {
			box = rect(0, 0, SW, SH);
		} else {
			box.left = rr(0, SW - 20);
			box.top = rr(0, SH - 10);
			box.right = rr(box.left + 1, SW);
			box.bottom = rr(box.top + 1, SH);
		}
		cpu_part(box, w, nw);
		laid = gpu_part(box, w, nw);
		if ( laid != nw ) die("scene %d: the plan laid %d of %d windows", s, laid, nw);
		for ( i = 0; i < (int)sizeof(back_c); i += 4 ) {
			uint32_t	a, b;

			memcpy(&a, back_c + i, 4);
			memcpy(&b, back_g + i, 4);
			if ( a == b ) continue;
			if ( d < 3 ) {
				fprintf(stderr, "  scene %d: at (%d,%d) shaders %08x, processor %08x\n", s,
					( i % PITCH ) / 4, i / PITCH, b, a);
			}
			if ( ( ( a ^ b ) & 0x00FFFFFF ) == 0 ) top_diff++;
			d++;
		}
		if ( d != 0 ) bad++;
		diff_px += d;
	}
	printf("test_igt_shaders: %d scenes, %d differ (%ld pixels, %ld only in the top byte), %ld rectangles in %ld "
	       "groups, %ld pixels written; %ld reads outside the buffers, %ld writes astray, %ld writes that differ "
	       "within a group\n", nscene, bad, diff_px, top_diff, nrect, ngroup, px_run, oob, bad_writes, conflicts);
	return ( bad == 0 && oob == 0 && bad_writes == 0 && conflicts == 0 ) ? 0 : 1;
}
