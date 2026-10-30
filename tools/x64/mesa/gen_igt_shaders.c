/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gen_igt_shaders.c
 *	The shaders that composite windows on the render engine of Intel's
 *	integrated graphics of the 9th generation, translated on the host
 *	into the engine's code (docs/tessronos/13-gpu.md 13.11.14).
 *
 *	Built by tools/x64/mesa/igt_shaders.sh against Mesa's NIR and Intel
 *	shader compiler (brw), which never go into the kernel. The shaders'
 *	work is the V3D's (tools/mesa/cp_stages.h): each stage of compose.c,
 *	pixel by pixel with the same integer arithmetic. There each is a
 *	fragment shader reading what is behind its pixel from the tile
 *	buffer; here each is turned into a compute shader over a rectangle of
 *	memory, one pixel to an invocation, sixteen to a thread:
 *
 *	  - the pixel is worked out from the thread group (16 pixels of one
 *	    row) and the invocation's place in it, from the rectangle's left
 *	    edge rounded down to 16; those left or right of the rectangle
 *	    write nothing
 *	  - what is behind it is read from the destination in memory (the
 *	    back buffer, or a level of the spread), and the pixel is written
 *	    there unless the stage leaves it alone (a discard of the fragment
 *	    shader)
 *	  - the stage's parameters, the destination and the buffers' addresses
 *	    are the thread's push constants (CURBE), 64 words, loaded for each
 *	    rectangle
 *	  - the buffers are read by their 64 bit addresses
 *
 *	compile with SIMD16 only; no scratch, no shared memory, no barrier,
 *	nothing pushed per thread, no constant data after the code. Written
 *	out as C: the code of all the shaders one after another (each on 64
 *	bytes), the interface descriptor of each, and the commands the kernel
 *	builds its batches from, packed with Mesa's descriptions of the 9th
 *	generation's commands (genxml), with where each field the kernel
 *	fills lies in them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include "dev/intel_device_info.h"
#include "dev/intel_debug.h"
#include "compiler/brw/brw_compiler.h"
#include "compiler/brw/brw_nir.h"

#define __gen_address_type	uint64_t
#define __gen_user_data		void
static inline uint64_t __gen_combine_address( void *d, void *loc, uint64_t a, uint32_t delta )
{
	(void)d;
	(void)loc;
	return a + delta;
}
#include "genxml/gen90_pack.h"

const unsigned *brw_compile_cs( const struct brw_compiler *compiler, struct brw_compile_cs_params *params );

/* The device: a Kaby Lake GT2; the code is the same for every 9th generation part of the kind */
#define PCI_ID		0x5917

/* The push constants: cp_stages.h's parameters in words 0 .. 31, then these */
#define PI_DST		32	/* the destination's address, low and high word */
#define PI_DPITCH	34	/* its bytes from a row to the next */
#define PI_RL		35	/* the rectangle written: left, top, right, bottom */
#define PI_RT		36
#define PI_RR		37
#define PI_RB		38
#define PI_BUF		40	/* each buffer's address, low and high word */
#define NPUSH		64	/* words */
#define LANES		16

/* ---------------------------------------------------------------- the device */

static struct intel_device_info	devinfo;
static struct brw_compiler	*compiler;
static nir_shader_compiler_options nir_options;
static void			*mem;

static void die( const char *fmt, ... )
{
	va_list	ap;

	va_start(ap, fmt);
	fprintf(stderr, "gen_igt_shaders: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void quiet( void *d, unsigned *id, const char *fmt, ... )
{
	va_list	ap;

	(void)d;
	(void)id;
	if ( getenv("IGT_LOG") == NULL ) return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void device_init( void )
{
	process_intel_debug_variable();		/* INTEL_DEBUG=cs: the disassembly (igt_shaders.sh gen) */
	if ( !intel_get_device_info_from_pci_id(PCI_ID, &devinfo) ) die("no device %04x", PCI_ID);
	if ( devinfo.ver != 9 ) die("%04x is not of the 9th generation", PCI_ID);
	mem = ralloc_context(NULL);
	compiler = brw_compiler_create(mem, &devinfo);
	if ( compiler == NULL ) die("no compiler");
	compiler->shader_debug_log = quiet;
	compiler->shader_perf_log = quiet;
	/* the shaders are built as fragment shaders first, with what the compiler wants of those */
	nir_options = compiler->nir_options[MESA_SHADER_FRAGMENT];
}

#include "cp_stages.h"

/* ---------------------------------------------------------------- the fragment shader turned into a compute shader */

static nir_def *P64( nir_builder *b, int i )
{
	return nir_pack_64_2x32_split(b, P(b, i), P(b, i + 1));
}

typedef struct {
	nir_variable	*pos, *color, *killed;
	nir_def		*x, *y, *inside, *dst, *dpitch, *xr;
} CSF;

/* The pixel of the invocation and where it lies in the destination; at the start of the shader */
static void cs_frame( nir_builder *b, CSF *c )
{
	nir_def	*lane = nir_load_subgroup_invocation(b);
	nir_def	*wg = nir_load_workgroup_id(b);
	nir_def	*rl = P(b, PI_RL), *rr = P(b, PI_RR);

	c->x = nir_iadd(b, nir_iand_imm(b, rl, ~( LANES - 1 )),
			nir_iadd(b, nir_imul_imm(b, nir_channel(b, wg, 0), LANES), lane));
	c->y = nir_iadd(b, P(b, PI_RT), nir_channel(b, wg, 1));
	c->inside = nir_iand(b, nir_ige(b, c->x, rl), nir_ilt(b, c->x, rr));
	c->xr = nir_imin(b, c->x, nir_iadd_imm(b, rr, -1));	/* held to the rectangle for reading */
	c->dst = P64(b, PI_DST);
	c->dpitch = P(b, PI_DPITCH);
	nir_store_var(b, c->killed, nir_imm_false(b), 1);
}

/* The byte offset of (x, y) in the destination */
static nir_def *cs_at( nir_builder *b, CSF *c, nir_def *x )
{
	return nir_iadd(b, c->dst, nir_u2u64(b, nir_iadd(b, nir_imul(b, c->y, c->dpitch), nir_ishl_imm(b, x, 2))));
}

static bool cs_lower( nir_builder *b, nir_intrinsic_instr *in, CSF *c )
{
	nir_def		*v = NULL;
	nir_variable	*var;
	int		i;

	b->cursor = nir_before_instr(&in->instr);
	switch ( in->intrinsic ) {
	case nir_intrinsic_load_deref:
		var = nir_intrinsic_get_var(in, 0);
		if ( var == c->pos ) {
			/* the centre of the pixel, as the rasteriser gives it */
			v = nir_vec4(b, nir_fadd_imm(b, nir_i2f32(b, c->x), 0.5), nir_fadd_imm(b, nir_i2f32(b, c->y), 0.5),
				     nir_imm_float(b, 0.0f), nir_imm_float(b, 1.0f));
		} else if ( var == c->color ) {
			/* what is behind: the destination's word, its bytes as the render target's channels */
			nir_def	*w = nir_load_global(b, 1, 32, cs_at(b, c, c->xr), .align_mul = 4);
			nir_def	*ch4[4];

			for ( i = 0; i < 4; i++ ) {
				ch4[i] = nir_fmul_imm(b, nir_u2f32(b, ch(b, w, i)), 1.0 / 255.0);
			}
			v = nir_vec(b, ch4, 4);
		} else {
			return false;
		}
		nir_def_rewrite_uses(&in->def, v);
		nir_instr_remove(&in->instr);
		return true;

	case nir_intrinsic_store_deref: {
		nir_def	*val, *bytes[4], *keep;

		var = nir_intrinsic_get_var(in, 0);
		if ( var != c->color ) return false;
		val = in->src[1].ssa;
		for ( i = 0; i < 4; i++ ) {
			bytes[i] = nir_umin(b, nir_f2u32(b, nir_fadd_imm(b, nir_fmul_imm(b, nir_channel(b, val, i), 255.0),
								     0.5)), nir_imm_int(b, 255));
		}
		keep = nir_iand(b, c->inside, nir_inot(b, nir_load_var(b, c->killed)));
		nir_push_if(b, keep);
		nir_store_global(b, pack4(b, bytes[0], bytes[1], bytes[2], bytes[3]), cs_at(b, c, c->x),
				 .write_mask = 0x1, .align_mul = 4);
		nir_pop_if(b, NULL);
		nir_instr_remove(&in->instr);
		return true;
	}

	case nir_intrinsic_terminate_if:
		/* a fragment left alone: nothing written for the pixel */
		nir_store_var(b, c->killed, nir_ior(b, nir_load_var(b, c->killed), in->src[0].ssa), 1);
		nir_instr_remove(&in->instr);
		return true;

	case nir_intrinsic_load_ssbo: {
		int	buf = (int)nir_src_as_uint(in->src[0]);

		v = nir_load_global(b, in->def.num_components, in->def.bit_size,
				    nir_iadd(b, P64(b, PI_BUF + 2 * buf), nir_u2u64(b, in->src[1].ssa)),
				    .align_mul = nir_intrinsic_align_mul(in), .align_offset = nir_intrinsic_align_offset(in),
				    .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER);
		nir_def_rewrite_uses(&in->def, v);
		nir_instr_remove(&in->instr);
		return true;
	}

	default:
		return false;
	}
}

/*
 * A shader of cp_stages.h made a compute shader of 16 invocations to a
 * group, its parameters pushed; in the form the test (test_igt_shaders.c)
 * runs and the compiler takes
 */
static nir_shader *cs_of( nir_shader *s )
{
	nir_function_impl	*impl = nir_shader_get_entrypoint(s);
	nir_builder		b = nir_builder_create(impl);
	CSF			c;

	memset(&c, 0, sizeof(c));
	nir_foreach_variable_with_modes(var, s, nir_var_shader_in | nir_var_shader_out) {
		if ( var->data.location == VARYING_SLOT_POS ) c.pos = var;
		if ( var->data.location == FRAG_RESULT_DATA0 ) c.color = var;
	}
	if ( c.color == NULL ) die("%s: no colour", s->info.name);
	c.killed = nir_local_variable_create(impl, glsl_bool_type(), "killed");
	b.cursor = nir_before_impl(impl);
	cs_frame(&b, &c);

	nir_foreach_block_safe(block, impl) {
		nir_foreach_instr_safe(instr, block) {
			if ( instr->type == nir_instr_type_intrinsic ) (void)cs_lower(&b, nir_instr_as_intrinsic(instr), &c);
		}
	}
	/* the parameters, those just made too, as the compiler takes push constants */
	nir_foreach_block_safe(block, impl) {
		nir_foreach_instr_safe(instr, block) {
			nir_intrinsic_instr	*in;
			nir_def			*v;

			if ( instr->type != nir_instr_type_intrinsic ) continue;
			in = nir_instr_as_intrinsic(instr);
			if ( in->intrinsic != nir_intrinsic_load_uniform ) continue;
			b.cursor = nir_before_instr(instr);
			v = nir_load_push_data_intel(&b, in->def.num_components, in->def.bit_size, in->src[0].ssa,
						     .base = nir_intrinsic_base(in), .range = nir_intrinsic_range(in));
			nir_def_rewrite_uses(&in->def, v);
			nir_instr_remove(instr);
		}
	}
	nir_progress(true, impl, nir_metadata_none);

	if ( c.pos != NULL ) exec_node_remove(&c.pos->node);
	exec_node_remove(&c.color->node);
	s->info.stage = MESA_SHADER_COMPUTE;
	s->info.internal = false;		/* printed with the rest under INTEL_DEBUG */
	memset(&s->info.cs, 0, sizeof(s->info.cs));
	s->info.num_ssbos = 0;
	s->info.workgroup_size[0] = LANES;
	s->info.workgroup_size[1] = 1;
	s->info.workgroup_size[2] = 1;
	s->info.workgroup_size_variable = false;
	s->info.api_subgroup_size = LANES;
	s->info.max_subgroup_size = LANES;
	s->info.min_subgroup_size = LANES;
	s->num_uniforms = NPUSH * 4;
	s->options = &compiler->nir_options[MESA_SHADER_COMPUTE];

	NIR_PASS(_, s, nir_opt_dce);
	NIR_PASS(_, s, nir_lower_vars_to_ssa);
	NIR_PASS(_, s, nir_lower_global_vars_to_local);
	NIR_PASS(_, s, nir_lower_vars_to_ssa);
	NIR_PASS(_, s, nir_opt_dce);
	NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp | nir_var_shader_temp, NULL);
	nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
	nir_validate_shader(s, "made a compute shader");
	return s;
}

/* ---------------------------------------------------------------- compiling */

#define NFS	10
static const char *fs_names[NFS] = { "piece", "shadow", "halve", "smooth3", "smooth5", "double", "moving",
				     "fill", "copy", "invert" };
static nir_shader *(*const fs_make[NFS])( void ) = { fs_piece, fs_shadow, fs_halve, fs_smooth3, fs_smooth5,
						      fs_double, fs_moving, fs_fill, fs_copy, fs_invert };

typedef struct {
	const char		*name;
	const unsigned		*code;
	unsigned		size;		/* bytes of the SIMD16 program */
	unsigned		at;		/* its place among all of them */
	struct brw_cs_prog_data	*pd;
} PROG;

static PROG	prog[NFS];

static void compile( PROG *p, const char *name, nir_shader *s )
{
	struct brw_nir_compiler_opts	o;
	struct brw_cs_prog_key		key;
	struct brw_compile_cs_params	cp;
	struct brw_cs_prog_data		*pd = rzalloc(mem, struct brw_cs_prog_data);

	memset(&o, 0, sizeof(o));
	brw_preprocess_nir(compiler, s, &o);
	NIR_PASS(_, s, brw_nir_lower_cs_intrinsics, &devinfo, pd);
	pd->base.push_sizes[0] = NPUSH * 4;
	brw_cs_fill_push_const_info(&devinfo, pd, -1);
	memset(&key, 0, sizeof(key));
	memset(&cp, 0, sizeof(cp));
	cp.base.mem_ctx = mem;
	cp.base.nir = s;
	cp.base.key = &key.base;
	cp.base.prog_data = &pd->base;
	p->name = name;
	p->code = brw_compile_cs(compiler, &cp);
	if ( p->code == NULL ) die("%s did not compile: %s", name, cp.base.error_str ? cp.base.error_str : "?");
	if ( pd->prog_mask != 2 ) die("%s: programs %x, not SIMD16 alone", name, pd->prog_mask);
	if ( pd->base.total_scratch != 0 || pd->prog_spilled != 0 ) die("%s spills registers", name);
	if ( pd->base.total_shared != 0 || pd->uses_barrier ) die("%s shares memory or waits at a barrier", name);
	if ( pd->base.num_relocs != 0 || pd->base.const_data_size != 0 ) die("%s carries constant data", name);
	if ( pd->push.per_thread.dwords != 0 ) die("%s wants constants of its own for each thread", name);
	if ( pd->push.cross_thread.regs * 8 > NPUSH || pd->push.cross_thread.regs == 0 ) {
		die("%s pushes %u registers", name, pd->push.cross_thread.regs);
	}
	if ( pd->local_size[0] != LANES || pd->local_size[1] != 1 || pd->local_size[2] != 1 ) die("%s: group size", name);
	p->size = pd->base.program_size - pd->prog_offset[1];
	p->pd = pd;
	fprintf(stderr, "%s: %u bytes, %u registers, %u registers pushed\n", name, p->size, pd->base.grf_used,
		pd->push.cross_thread.regs);
}

static void compile_all( void )
{
	const char	*only = getenv("IGT_ONLY");		/* one shader alone, for looking into the compiler */
	int		i;

	for ( i = 0; i < NFS; i++ ) {
		if ( only != NULL && strcmp(only, fs_names[i]) != 0 ) continue;
		compile(&prog[i], fs_names[i], cs_of(fs_make[i]()));
	}
	if ( only != NULL ) exit(0);
}

/* ---------------------------------------------------------------- writing out */

static FILE	*out;

static void put_words( const char *name, const uint32_t *w, int n )
{
	int	i;

	fprintf(out, "LOCAL CONST UW %s[%d] __attribute__((aligned(64))) = {", name, n);
	for ( i = 0; i < n; i++ ) fprintf(out, "%s0x%08x,", ( i % 6 ) == 0 ? "\n\t" : " ", w[i]);
	fprintf(out, "\n};\n\n");
}

/*
 * Where a field the kernel fills lies in a packed command: packed with
 * the field 0 and with its largest value, the one word that differs and
 * the field's place in it
 */
#define FIELD(name, cmd, fld, width) do { \
		struct GFX9_##cmd _a = { GFX9_##cmd##_header }, _b = { GFX9_##cmd##_header }; \
		uint32_t _wa[GFX9_##cmd##_length], _wb[GFX9_##cmd##_length]; \
		const uint64_t _max = ( (width) >= 64 ) ? ~0ULL : ( 1ULL << (width) ) - 1; \
		_b.fld = _max; \
		GFX9_##cmd##_pack(NULL, _wa, &_a); \
		GFX9_##cmd##_pack(NULL, _wb, &_b); \
		put_field(name, _wa, _wb, GFX9_##cmd##_length, _max); \
	} while ( 0 )

static void put_field( const char *name, const uint32_t *a, const uint32_t *b, int n, uint64_t max )
{
	int		i, at = -1, s;
	uint32_t	d;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] == b[i] ) continue;
		if ( at >= 0 ) die("field %s spans words", name);
		at = i;
	}
	if ( at < 0 ) die("field %s not found", name);
	d = a[at] ^ b[at];
	for ( s = 0; s < 32 && ( ( d >> s ) & 1 ) == 0; s++ ) ;
	if ( ( (uint64_t)d >> s ) != max ) die("field %s: %08x is not %llx shifted", name, d, (unsigned long long)max);
	fprintf(out, "#define IGT_CP_%s_DW\t%d\n#define IGT_CP_%s_SHIFT\t%d\n#define IGT_CP_%s_MASK\t0x%08xU\n", name, at,
		name, s, name, d);
}

/*
 * Where an address the kernel fills lies: packed with 0 and with an
 * address of 48 bits, its low word added to one word and its high to the
 * next, the rest as they were
 */
#define ADDR(name, cmd, fld, ...) do { \
		struct GFX9_##cmd _a = { GFX9_##cmd##_header, __VA_ARGS__ }; \
		struct GFX9_##cmd _b = { GFX9_##cmd##_header, __VA_ARGS__ }; \
		uint32_t _wa[GFX9_##cmd##_length], _wb[GFX9_##cmd##_length]; \
		_b.fld = 0x0000123456789000ULL; \
		GFX9_##cmd##_pack(NULL, _wa, &_a); \
		GFX9_##cmd##_pack(NULL, _wb, &_b); \
		put_addr(name, _wa, _wb, GFX9_##cmd##_length); \
	} while ( 0 )

static void put_addr( const char *name, const uint32_t *a, const uint32_t *b, int n )
{
	int	i, at = -1;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] == b[i] ) continue;
		if ( at < 0 ) {
			at = i;
			if ( b[i] - a[i] != 0x56789000U ) die("address %s: low word %08x", name, b[i] - a[i]);
		} else if ( i == at + 1 ) {
			if ( b[i] - a[i] != 0x1234U ) die("address %s: high word %08x", name, b[i] - a[i]);
		} else {
			die("address %s spans more than two words", name);
		}
	}
	if ( at < 0 ) die("address %s not found", name);
	fprintf(out, "#define IGT_CP_%s_DW\t%d\n", name, at);
}

static const char notice[] =
"/*\n"
" * device/gpu/intel/mesa/igt_cp_shaders.h\n"
" *\n"
" * The shaders that composite windows on the render engine of Intel's\n"
" * integrated graphics of the 9th generation (docs/tessronos/13-gpu.md 13.11.14),\n"
" * generated by \"tools/x64/mesa/igt_shaders.sh\" (tools/x64/mesa/gen_igt_shaders.c,\n"
" * tools/mesa/cp_stages.h) with the NIR and Intel shader compiler of Mesa %s and\n"
" * its descriptions of the commands (genxml). Do not edit; generate it again. The\n"
" * code and the packed commands below are the output of that compiler and of that\n"
" * packing.\n"
" *\n"
" * Copyright (C) 2014-2025 Intel Corporation\n"
" * and the other authors of Mesa's src/compiler/nir, src/intel and src/util, as\n"
" * each of their files says\n"
" *\n"
" * Permission is hereby granted, free of charge, to any person obtaining a\n"
" * copy of this software and associated documentation files (the \"Software\"),\n"
" * to deal in the Software without restriction, including without limitation\n"
" * the rights to use, copy, modify, merge, publish, distribute, sublicense,\n"
" * and/or sell copies of the Software, and to permit persons to whom the\n"
" * Software is furnished to do so, subject to the following conditions:\n"
" *\n"
" * The above copyright notice and this permission notice (including the next\n"
" * paragraph) shall be included in all copies or substantial portions of the\n"
" * Software.\n"
" *\n"
" * THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
" * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
" * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL\n"
" * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
" * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING\n"
" * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS\n"
" * IN THE SOFTWARE.\n"
" */\n\n";

/* A command packed whole, as the kernel copies it */
#define CMD(name, cmd, ...) do { \
		struct GFX9_##cmd _c = { GFX9_##cmd##_header, __VA_ARGS__ }; \
		uint32_t _w[GFX9_##cmd##_length]; \
		GFX9_##cmd##_pack(NULL, _w, &_c); \
		put_words("igt_cp_" name, _w, GFX9_##cmd##_length); \
		fprintf(out, "#define IGT_CP_LEN_%s\t%d\n\n", #cmd, GFX9_##cmd##_length); \
	} while ( 0 )

/* A command of no fields of its own */
#define CMD0(name, cmd) do { \
		struct GFX9_##cmd _c = { GFX9_##cmd##_header }; \
		uint32_t _w[GFX9_##cmd##_length]; \
		GFX9_##cmd##_pack(NULL, _w, &_c); \
		put_words("igt_cp_" name, _w, GFX9_##cmd##_length); \
		fprintf(out, "#define IGT_CP_LEN_%s\t%d\n\n", #cmd, GFX9_##cmd##_length); \
	} while ( 0 )

/* What STATE_BASE_ADDRESS cannot have 0 in, packed or probed */
#define SBA_MOCS	.GeneralStateMOCS = wb, .StatelessDataPortAccessMOCS = wb, .SurfaceStateMOCS = wb, \
			.DynamicStateMOCS = wb, .IndirectObjectMOCS = wb, .InstructionMOCS = wb, \
			.BindlessSurfaceStateMOCS = wb

/* The commands the kernel builds a part's batch from (igt_rcompose.c) */
static void put_commands( void )
{
	const unsigned	wb = 2 << 1;		/* MOCS: entry 2, write-back (igt.c mocs()) */

	fprintf(out, "/* ---- the commands */\n\n");

	/* an end of pipe sync: the caches written back, the pipe idle, a word written after */
	CMD("sync_flush", PIPE_CONTROL,
	    .RenderTargetCacheFlushEnable = true, .DepthCacheFlushEnable = true, .DCFlushEnable = true,
	    .CommandStreamerStallEnable = true, .PostSyncOperation = WriteImmediateData, .ImmediateData = 1);
	ADDR("SYNC_ADDR", PIPE_CONTROL, Address, .PostSyncOperation = WriteImmediateData);
	/* the read-only caches invalidated, after a change of the state's bases or of the pipeline */
	CMD("invalidate", PIPE_CONTROL,
	    .TextureCacheInvalidationEnable = true, .ConstantCacheInvalidationEnable = true,
	    .StateCacheInvalidationEnable = true, .InstructionCacheInvalidateEnable = true,
	    .CommandStreamerStallEnable = true);
	/* between rectangles that read what earlier ones wrote */
	CMD("barrier", PIPE_CONTROL, .DCFlushEnable = true, .CommandStreamerStallEnable = true);
	/* the colour calculator's state pointer cleared before the pipeline turns to GPGPU */
	CMD0("cc_pointers", 3DSTATE_CC_STATE_POINTERS);
	CMD("select_gpgpu", PIPELINE_SELECT, .MaskBits = 3, .PipelineSelection = GPGPU);
	CMD("base", STATE_BASE_ADDRESS,
	    .GeneralStateBaseAddressModifyEnable = true, .GeneralStateMOCS = wb,
	    .StatelessDataPortAccessMOCS = wb,
	    .SurfaceStateBaseAddressModifyEnable = true, .SurfaceStateMOCS = wb,
	    .DynamicStateBaseAddressModifyEnable = true, .DynamicStateMOCS = wb,
	    .IndirectObjectBaseAddressModifyEnable = true, .IndirectObjectMOCS = wb,
	    .InstructionBaseAddressModifyEnable = true, .InstructionMOCS = wb,
	    .GeneralStateBufferSizeModifyEnable = true, .GeneralStateBufferSize = 0xfffff,
	    .DynamicStateBufferSizeModifyEnable = true, .DynamicStateBufferSize = 0xfffff,
	    .IndirectObjectBufferSizeModifyEnable = true, .IndirectObjectBufferSize = 0xfffff,
	    .InstructionBuffersizeModifyEnable = true, .InstructionBufferSize = 0xfffff,
	    .BindlessSurfaceStateBaseAddressModifyEnable = true, .BindlessSurfaceStateMOCS = wb,
	    .BindlessSurfaceStateSize = 0);
	ADDR("BASE_GENERAL", STATE_BASE_ADDRESS, GeneralStateBaseAddress, SBA_MOCS);
	ADDR("BASE_SURFACE", STATE_BASE_ADDRESS, SurfaceStateBaseAddress, SBA_MOCS);
	ADDR("BASE_DYNAMIC", STATE_BASE_ADDRESS, DynamicStateBaseAddress, SBA_MOCS);
	ADDR("BASE_INDIRECT", STATE_BASE_ADDRESS, IndirectObjectBaseAddress, SBA_MOCS);
	ADDR("BASE_INSTRUCTION", STATE_BASE_ADDRESS, InstructionBaseAddress, SBA_MOCS);
	ADDR("BASE_BINDLESS", STATE_BASE_ADDRESS, BindlessSurfaceStateBaseAddress, SBA_MOCS);
	fprintf(out, "\n");
	CMD("vfe", MEDIA_VFE_STATE,
	    .MaximumNumberofThreads = 1, .ResetGatewayTimer = Resettingrelativetimerandlatchingtheglobaltimestamp,
	    .NumberofURBEntries = 2, .URBEntryAllocationSize = 2,
	    .CURBEAllocationSize = ( prog[0].pd->push.cross_thread.regs + 1 ) & ~1u);
	FIELD("VFE_THREADS", MEDIA_VFE_STATE, MaximumNumberofThreads, 16);
	fprintf(out, "\n");
	CMD0("idl", MEDIA_INTERFACE_DESCRIPTOR_LOAD);
	FIELD("IDL_LENGTH", MEDIA_INTERFACE_DESCRIPTOR_LOAD, InterfaceDescriptorTotalLength, 17);
	FIELD("IDL_START", MEDIA_INTERFACE_DESCRIPTOR_LOAD, InterfaceDescriptorDataStartAddress, 32);
	fprintf(out, "\n");
	CMD0("curbe", MEDIA_CURBE_LOAD);
	FIELD("CURBE_LENGTH", MEDIA_CURBE_LOAD, CURBETotalDataLength, 17);
	FIELD("CURBE_START", MEDIA_CURBE_LOAD, CURBEDataStartAddress, 32);
	fprintf(out, "\n");
	CMD("walker", GPGPU_WALKER,
	    .SIMDSize = SIMD16, .ThreadWidthCounterMaximum = 0, .ThreadHeightCounterMaximum = 0,
	    .ThreadDepthCounterMaximum = 0, .ThreadGroupIDZDimension = 1,
	    .RightExecutionMask = 0xffff, .BottomExecutionMask = 0xffffffff);
	FIELD("WALKER_IDO", GPGPU_WALKER, InterfaceDescriptorOffset, 6);
	FIELD("WALKER_XDIM", GPGPU_WALKER, ThreadGroupIDXDimension, 32);
	FIELD("WALKER_YDIM", GPGPU_WALKER, ThreadGroupIDYDimension, 32);
	fprintf(out, "\n");
	CMD0("msf", MEDIA_STATE_FLUSH);
}

/* The interface descriptors, one a shader, in the order of the shaders */
static void put_descriptors( void )
{
	uint32_t	w[NFS * 8];
	int		i;

	for ( i = 0; i < NFS; i++ ) {
		struct GFX9_INTERFACE_DESCRIPTOR_DATA	d;

		memset(&d, 0, sizeof(d));
		d.KernelStartPointer = prog[i].at;
		d.NumberofThreadsinGPGPUThreadGroup = 1;
		d.CrossThreadConstantDataReadLength = prog[i].pd->push.cross_thread.regs;
		d.ConstantURBEntryReadLength = 0;
		d.BarrierEnable = false;
		d.SharedLocalMemorySize = 0;
		d.BindingTableEntryCount = 0;
		d.SamplerCount = 0;
		GFX9_INTERFACE_DESCRIPTOR_DATA_pack(NULL, &w[i * 8], &d);
	}
	fprintf(out, "/* The interface descriptors, one a shader; the code's place from the base of the instructions */\n");
	put_words("igt_cp_idd", w, NFS * 8);
	fprintf(out, "#define IGT_CP_IDD_LEN\t\t%d\n\n", GFX9_INTERFACE_DESCRIPTOR_DATA_length);
}

static void put_code( void )
{
	unsigned	total = 0, i, n;
	uint32_t	*all;

	for ( i = 0; i < NFS; i++ ) {
		prog[i].at = total;
		total += ( prog[i].size + 63 ) & ~63u;
	}
	total += 128;				/* what the engine may fetch past the last */
	all = calloc(total / 4, 4);
	for ( i = 0; i < NFS; i++ ) {
		memcpy((uint8_t *)all + prog[i].at, (const uint8_t *)prog[i].code + prog[i].pd->prog_offset[1],
		       prog[i].size);
	}
	fprintf(out, "/* The code of all the shaders, each on 64 bytes, in the order of the shaders */\n");
	for ( i = 0; i < NFS; i++ ) {
		fprintf(out, "/*   %-8s at %5u, %4u bytes, %3u registers, %u pushed */\n", fs_names[i], prog[i].at,
			prog[i].size, prog[i].pd->base.grf_used, prog[i].pd->push.cross_thread.regs);
	}
	n = total / 4;
	put_words("igt_cp_code", all, (int)n);
	fprintf(out, "#define IGT_CP_CODE_BYTES\t%u\n\n", total);
	free(all);
}

int main( int argc, char **argv )
{
	int	i;

	if ( argc < 3 ) die("usage: gen_igt_shaders <out.h> <mesa version>");
	div255_check();
	glsl_type_singleton_init_or_ref();
	device_init();
	compile_all();

	out = fopen(argv[1], "w");
	if ( out == NULL ) die("cannot write %s", argv[1]);
	fprintf(out, notice, argv[2]);
	fprintf(out, "#ifndef _IGT_CP_SHADERS_H_\n#define _IGT_CP_SHADERS_H_\n\n");
	fprintf(out, "/* The device the code is for: the 9th generation (%s) */\n", devinfo.name);
	fprintf(out, "#define IGT_CP_GEN\t\t%d\n", devinfo.ver);
	fprintf(out, "/* Threads a subslice runs, for MEDIA_VFE_STATE */\n");
	fprintf(out, "#define IGT_CP_THREADS_PER_SS\t%u\n\n", devinfo.max_cs_threads);

	fprintf(out, "/* The draw's buffers (cp_stages.h) */\n");
	fprintf(out, "#define CP_B_SRC\t\t%d\n#define CP_B_AUX\t\t%d\n#define CP_B_COV\t\t%d\n#define CP_NBUF\t\t\t3\n\n",
		B_SRC, B_AUX, B_COV);
	fprintf(out, "/* The draw's parameters (cp_stages.h), words of the push constants */\n");
	for ( i = 0; i < (int)( sizeof(param_names) / sizeof(param_names[0]) ); i++ ) {
		fprintf(out, "#define %-15s\t%d\n", param_names[i].name, param_names[i].value);
	}
	fprintf(out, "#define CP_NPARAM\t\t32\n");
	fprintf(out, "/* and the rest of them: the destination, the rectangle written, the buffers' addresses */\n");
	fprintf(out, "#define IGT_CP_P_DST\t\t%d\n#define IGT_CP_P_DPITCH\t\t%d\n#define IGT_CP_P_RL\t\t%d\n"
		"#define IGT_CP_P_RT\t\t%d\n#define IGT_CP_P_RR\t\t%d\n#define IGT_CP_P_RB\t\t%d\n#define IGT_CP_P_BUF\t\t%d\n"
		"#define IGT_CP_NPUSH\t\t%d\n#define IGT_CP_LANES\t\t%d\n\n", PI_DST, PI_DPITCH, PI_RL, PI_RT, PI_RR, PI_RB,
		PI_BUF, NPUSH, LANES);

	fprintf(out, "/* The shaders, in the order of their code and descriptors */\n");
	for ( i = 0; i < NFS; i++ ) {
		char	up[16];
		int	k;

		for ( k = 0; fs_names[i][k] != 0 && k < 15; k++ ) {
			up[k] = ( fs_names[i][k] >= 'a' && fs_names[i][k] <= 'z' ) ? fs_names[i][k] - 'a' + 'A'
										  : fs_names[i][k];
		}
		up[k] = 0;
		fprintf(out, "#define CP_FS_%s\t\t%d\n", up, i);
	}
	fprintf(out, "#define CP_NFS\t\t\t%d\n\n", NFS);

	/* the code and the commands only where they are used: the rest of the kernel takes the numbers */
	fprintf(out, "#ifdef IGT_CP_WANT_CODE\n\n");
	put_code();
	put_descriptors();
	put_commands();
	fprintf(out, "#endif /* IGT_CP_WANT_CODE */\n\n");
	fprintf(out, "#endif /* _IGT_CP_SHADERS_H_ */\n");
	fclose(out);
	return 0;
}
