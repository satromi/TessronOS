/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_test.c
 *	The tests of the render and copy engines at start-up ("igt=2"),
 *	each told on the console on a line of its own that begins
 *	"igt test:" and so kept in KERNEL.TXT: one start of the machine
 *	tells all of them (docs/tessronos/13-gpu.md 13.11.5).
 *
 *	Each job has a time limit, after which the common layer resets the
 *	engine; a test that fails goes on to the next. The tests that may
 *	leave the GPU in a bad state (a batch that never ends) come last,
 *	and a store after them tells whether it came back.
 *
 *	The buffers are BOs of the kernel's space; the processor reaches
 *	their pages through the linear mapping, a page at a time.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "sysman/pfalloc.h"
#include "igt.h"

LOCAL INT	t_pass, t_run;
LOCAL char	t_failed[160];

LOCAL void note( CONST char *name, BOOL ok )
{
	INT	k = 0;

	t_run++;
	if ( ok ) {
		t_pass++;
		return;
	}
	while ( t_failed[k] != '\0' ) k++;
	if ( k + 20 < (INT)sizeof(t_failed) ) {
		if ( k != 0 ) t_failed[k++] = ' ';
		while ( *name != '\0' && k < (INT)sizeof(t_failed) - 1 ) t_failed[k++] = *name++;
		t_failed[k] = '\0';
	}
}

/* ---------------------------------------------------------------- buffers */

typedef struct {
	T_GPUBO	*bo;
	UW	gpu;
	UD	bytes;
} TBUF;

LOCAL BOOL tb_make( TBUF *b, UD bytes )
{
	T_GPUBOINFO	bi;
	ER		er;

	b->bo = knl_gpu_bo_create((SZ)bytes, 0, &er);
	if ( b->bo == NULL ) {
		tm_printf((UB *)"igt test: no BO of %ld bytes (%d)\n", bytes, er);
		return FALSE;
	}
	(void)knl_gpu_bo_ref(b->bo, &bi);
	b->gpu = bi.offset;
	b->bytes = (UD)bi.size;
	return TRUE;
}

LOCAL void tb_drop( TBUF *b )
{
	if ( b->bo != NULL ) knl_gpu_bo_put(b->bo);
	b->bo = NULL;
}

Inline UW *tb_at( TBUF *b, UD off )
{
	return (UW *)( (UB *)PA2VA(knl_gpu_bo_page(b->bo, off >> PAGE_SHIFT)) + ( off & ( PAGE_SIZE - 1 ) ) );
}

LOCAL void tb_fill( TBUF *b, UW v )
{
	UD	off;

	for ( off = 0; off < b->bytes; off += 4 ) *tb_at(b, off) = v;
}

/* Every line of the buffer written back and dropped from the processor's caches */
LOCAL void tb_flush( TBUF *b )
{
	UD	p;

	for ( p = 0; p < b->bytes; p += PAGE_SIZE ) igt_clflush(tb_at(b, p), PAGE_SIZE);
}

/* A batch being written */
typedef struct {
	TBUF	*b;
	UD	n;		/* dwords */
} BATCH;

LOCAL void bput( BATCH *bt, UW v )
{
	*tb_at(bt->b, bt->n * 4) = v;
	bt->n++;
}

/* The batch closed, written back; its length in bytes */
LOCAL UW bend( BATCH *bt )
{
	bput(bt, MI_BATCH_BUFFER_END);
	if ( bt->n & 1 ) bput(bt, MI_NOOP);
	tb_flush(bt->b);
	return (UW)( bt->n * 4 );
}

LOCAL void store_dw( BATCH *bt, UD addr, UW v )
{
	bput(bt, MI_STORE_DWORD_IMM);
	bput(bt, (UW)addr);
	bput(bt, (UW)( addr >> 32 ));
	bput(bt, v);
}

LOCAL void color_blt( BATCH *bt, UD dst, UW pitch, INT x0, INT y0, INT x1, INT y1, UW color )
{
	bput(bt, XY_COLOR_BLT | BLT_WRITE_RGBA);
	bput(bt, BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | pitch);
	bput(bt, ( (UW)y0 << 16 ) | (UW)x0);
	bput(bt, ( (UW)y1 << 16 ) | (UW)x1);
	bput(bt, (UW)dst);
	bput(bt, (UW)( dst >> 32 ));
	bput(bt, color);
}

LOCAL void copy_blt( BATCH *bt, UD dst, UW dpitch, INT dx, INT dy, INT w, INT h, UD src, UW spitch, INT sx, INT sy )
{
	bput(bt, XY_SRC_COPY_BLT | BLT_WRITE_RGBA);
	bput(bt, BLT_DEPTH_32 | BLT_ROP_SRC_COPY | dpitch);
	bput(bt, ( (UW)dy << 16 ) | (UW)dx);
	bput(bt, ( (UW)( dy + h ) << 16 ) | (UW)( dx + w ));
	bput(bt, (UW)dst);
	bput(bt, (UW)( dst >> 32 ));
	bput(bt, ( (UW)sy << 16 ) | (UW)sx);
	bput(bt, spitch);
	bput(bt, (UW)src);
	bput(bt, (UW)( src >> 32 ));
}

LOCAL CONST char *eng_name( INT eng )
{
	return ( eng == IGT_RCS ) ? "rcs" : "bcs";
}

/* ---------------------------------------------------------------- the tests */

/* Dwords stored by MI_STORE_DATA_IMM on an engine, through its page tables */
LOCAL void t_store( INT eng, CONST char *name )
{
	TBUF	bb = { 0 }, tg = { 0 };
	BATCH	bt;
	T_GPUBO	*bos[2];
	UD	t0;
	INT	i, wrong = 0;
	ER	er = E_NOMEM;

	if ( tb_make(&bb, PAGE_SIZE) && tb_make(&tg, 2 * PAGE_SIZE) ) {
		tb_fill(&tg, 0xdeadbeef);
		tb_flush(&tg);
		bt.b = &bb;
		bt.n = 0;
		for ( i = 0; i < 16; i++ ) store_dw(&bt, tg.gpu + (UD)i * 260, 0xc0ffee00 + (UW)i);
		bos[0] = bb.bo;
		bos[1] = tg.bo;
		t0 = igt_now_us();
		er = igt_kjob(eng, bb.gpu, bend(&bt), bos, 2);
		t0 = igt_now_us() - t0;
		tb_flush(&tg);
		for ( i = 0; i < 16; i++ ) {
			if ( *tb_at(&tg, (UD)i * 260) != 0xc0ffee00 + (UW)i ) wrong++;
		}
		if ( *tb_at(&tg, 4) != 0xdeadbeef ) wrong++;		/* not written between */
		tm_printf((UB *)"igt test: %s %s %s: job %d, %d of 17 words wrong (first %08x), %ld us\n", name,
			  eng_name(eng), ( er == E_OK && wrong == 0 ) ? "ok" : "FAIL", er, wrong, *tb_at(&tg, 0), t0);
	} else {
		tm_printf((UB *)"igt test: %s %s FAIL: no memory\n", name, eng_name(eng));
	}
	note(name, er == E_OK && wrong == 0);
	tb_drop(&bb);
	tb_drop(&tg);
}

/* A rectangle filled by the copy engine, nothing past it touched */
LOCAL void t_fill( INT w, INT h, CONST char *name )
{
	TBUF	bb = { 0 }, tg = { 0 };
	BATCH	bt;
	T_GPUBO	*bos[2];
	UW	pitch = (UW)( ( w * 4 + 63 ) & ~63 ) + 64;	/* a pitch wider than the rectangle */
	UD	bytes = (UD)pitch * (UD)( h + 1 ), t0, off;
	INT	x, y, wrong = 0, past = 0;
	ER	er = E_NOMEM;

	if ( tb_make(&bb, PAGE_SIZE) && tb_make(&tg, ( bytes + PAGE_SIZE - 1 ) & ~(UD)( PAGE_SIZE - 1 )) ) {
		tb_fill(&tg, 0x11111111);
		tb_flush(&tg);
		bt.b = &bb;
		bt.n = 0;
		color_blt(&bt, tg.gpu, pitch, 0, 0, w, h, 0x00a5c3e1);
		bos[0] = bb.bo;
		bos[1] = tg.bo;
		t0 = igt_now_us();
		er = igt_kjob(IGT_BCS, bb.gpu, bend(&bt), bos, 2);
		t0 = igt_now_us() - t0;
		tb_flush(&tg);
		for ( y = 0; y <= h; y++ ) {
			for ( x = 0; x < (INT)( pitch / 4 ); x++ ) {
				UW	v = *tb_at(&tg, (UD)y * pitch + (UD)x * 4);
				BOOL	in = ( x < w && y < h );

				if ( in && v != 0x00a5c3e1 ) wrong++;
				if ( !in && v != 0x11111111 ) past++;
			}
		}
		for ( off = (UD)pitch * (UD)( h + 1 ); off < tg.bytes; off += 4 ) {
			if ( *tb_at(&tg, off) != 0x11111111 ) past++;
		}
		tm_printf((UB *)"igt test: %s %dx%d %s: job %d, %d of %d pixels wrong, %d written outside, %ld us, %ld MB/s\n",
			  name, w, h, ( er == E_OK && wrong == 0 && past == 0 ) ? "ok" : "FAIL", er, wrong, w * h, past,
			  t0, ( t0 != 0 ) ? (UD)w * (UD)h * 4 / t0 : 0);
	} else {
		tm_printf((UB *)"igt test: %s FAIL: no memory\n", name);
	}
	note(name, er == E_OK && wrong == 0 && past == 0);
	tb_drop(&bb);
	tb_drop(&tg);
}

/* A rectangle copied by the copy engine, from and to places not aligned */
LOCAL void t_copy( INT w, INT h, INT sx, INT sy, INT dx, INT dy, CONST char *name )
{
	TBUF	bb = { 0 }, sb = { 0 }, db = { 0 };
	BATCH	bt;
	T_GPUBO	*bos[3];
	UW	sp = (UW)( ( sx + w ) * 4 + 12 ), dp = (UW)( ( dx + w ) * 4 + 36 );
	UD	t0;
	INT	x, y, wrong = 0, past = 0;
	ER	er = E_NOMEM;

	if ( tb_make(&bb, PAGE_SIZE) && tb_make(&sb, ( (UD)sp * (UD)( sy + h ) + PAGE_SIZE - 1 ) & ~4095UL)
	  && tb_make(&db, ( (UD)dp * (UD)( dy + h + 1 ) + PAGE_SIZE - 1 ) & ~4095UL) ) {
		for ( y = 0; y < sy + h; y++ ) {
			for ( x = 0; x < (INT)( sp / 4 ); x++ ) {
				*tb_at(&sb, (UD)y * sp + (UD)x * 4) = ( (UW)y << 16 ) ^ (UW)x ^ 0x5a000000;
			}
		}
		tb_fill(&db, 0x22222222);
		tb_flush(&sb);
		tb_flush(&db);
		bt.b = &bb;
		bt.n = 0;
		copy_blt(&bt, db.gpu, dp, dx, dy, w, h, sb.gpu, sp, sx, sy);
		bos[0] = bb.bo;
		bos[1] = sb.bo;
		bos[2] = db.bo;
		t0 = igt_now_us();
		er = igt_kjob(IGT_BCS, bb.gpu, bend(&bt), bos, 3);
		t0 = igt_now_us() - t0;
		tb_flush(&db);
		for ( y = 0; y < dy + h + 1; y++ ) {
			for ( x = 0; x < (INT)( dp / 4 ); x++ ) {
				UW	v = *tb_at(&db, (UD)y * dp + (UD)x * 4);
				BOOL	in = ( x >= dx && x < dx + w && y >= dy && y < dy + h );

				if ( in ) {
					UW	want = ( (UW)( y - dy + sy ) << 16 ) ^ (UW)( x - dx + sx ) ^ 0x5a000000;

					if ( v != want ) wrong++;
				} else if ( v != 0x22222222 ) {
					past++;
				}
			}
		}
		tm_printf((UB *)"igt test: %s %dx%d from (%d,%d) to (%d,%d) %s: job %d, %d pixels wrong, %d written outside, "
			  "%ld us\n", name, w, h, sx, sy, dx, dy, ( er == E_OK && wrong == 0 && past == 0 ) ? "ok" : "FAIL",
			  er, wrong, past, t0);
	} else {
		tm_printf((UB *)"igt test: %s FAIL: no memory\n", name);
	}
	note(name, er == E_OK && wrong == 0 && past == 0);
	tb_drop(&bb);
	tb_drop(&sb);
	tb_drop(&db);
}

/* Jobs queued on both engines in turn: their fences in order, every one done */
LOCAL void t_queue( void )
{
	TBUF		bb[4], tg = { 0 };
	T_GPUJOB	j;
	T_GPUBO		*bos[2];
	D		f[4];
	INT		i, wrong = 0;
	ER		er = E_OK;
	BOOL		order = TRUE;

	knl_memset(bb, 0, sizeof(bb));
	if ( !tb_make(&tg, PAGE_SIZE) ) {
		note("queue", FALSE);
		return;
	}
	tb_fill(&tg, 0);
	tb_flush(&tg);
	for ( i = 0; i < 4; i++ ) {
		BATCH	bt;

		if ( !tb_make(&bb[i], PAGE_SIZE) ) {
			er = E_NOMEM;
			break;
		}
		bt.b = &bb[i];
		bt.n = 0;
		store_dw(&bt, tg.gpu + (UD)i * 64, 0x51000000 + (UW)i);
		knl_memset(&j, 0, sizeof(j));
		j.kind = GPU_JOB_BATCH;
		j.u.batch.start_lo = bb[i].gpu;
		j.u.batch.len = bend(&bt);
		j.u.batch.engine = (UW)( i & 1 );
		bos[0] = bb[i].bo;
		bos[1] = tg.bo;
		f[i] = knl_gpu_submit(&j, bos, 2);
		if ( f[i] <= 0 ) {
			er = (ER)f[i];
			break;
		}
		if ( i > 0 && f[i] != f[i - 1] + 1 ) order = FALSE;
	}
	if ( er == E_OK ) er = knl_gpu_wait(f[3], 4 * 1000 * 1000);
	tb_flush(&tg);
	for ( i = 0; i < 4; i++ ) {
		if ( *tb_at(&tg, (UD)i * 64) != 0x51000000 + (UW)i ) wrong++;
	}
	tm_printf((UB *)"igt test: queue of 4 on both engines %s: last job %d, fences in order %s, %d of 4 wrong\n",
		  ( er == E_OK && wrong == 0 && order ) ? "ok" : "FAIL", er, order ? "yes" : "no", wrong);
	note("queue", er == E_OK && wrong == 0 && order);
	for ( i = 0; i < 4; i++ ) tb_drop(&bb[i]);
	tb_drop(&tg);
}

/*
 * Whether the GPU and the processor see each other's writes without
 * the processor's caches being flushed: a source written through the
 * caches and not flushed, copied by the copy engine into a destination
 * whose lines the processor holds, read again without flushing
 */
LOCAL void t_coherence( void )
{
	TBUF	bb = { 0 }, sb = { 0 }, db = { 0 };
	BATCH	bt;
	T_GPUBO	*bos[3];
	UW	i, n = PAGE_SIZE / 4, newv = 0, stale = 0, other = 0;
	volatile UW	sink = 0;
	ER	er = E_NOMEM;

	if ( tb_make(&bb, PAGE_SIZE) && tb_make(&sb, PAGE_SIZE) && tb_make(&db, PAGE_SIZE) ) {
		tb_fill(&db, 0x33333333);
		tb_flush(&db);
		for ( i = 0; i < n; i++ ) sink += *tb_at(&db, i * 4);		/* its lines in the cache */
		for ( i = 0; i < n; i++ ) *tb_at(&sb, i * 4) = 0x77000000 + i;	/* left in the cache */
		bt.b = &bb;
		bt.n = 0;
		copy_blt(&bt, db.gpu, PAGE_SIZE, 0, 0, (INT)n, 1, sb.gpu, PAGE_SIZE, 0, 0);
		bos[0] = bb.bo;
		bos[1] = sb.bo;
		bos[2] = db.bo;
		er = igt_kjob(IGT_BCS, bb.gpu, bend(&bt), bos, 3);
		for ( i = 0; i < n; i++ ) {
			UW	v = *tb_at(&db, i * 4);

			if ( v == 0x77000000 + i ) newv++;
			else if ( v == 0x33333333 ) stale++;
			else other++;
		}
	}
	tm_printf((UB *)"igt test: coherence %s: job %d; read back without flushing: %d new, %d stale, %d other of %d "
		  "(new: the GPU read the processor's cache and the processor saw the GPU's writes)\n",
		  ( er == E_OK && newv == n ) ? "ok" : "FAIL", er, newv, stale, other, n);
	note("coherence", er == E_OK && newv == n);
	(void)sink;
	tb_drop(&bb);
	tb_drop(&sb);
	tb_drop(&db);
}

/*
 * Address spaces kept apart: a job in a space of its own (as a
 * process's) stores to a BO of its space and to the address of a BO of
 * the kernel's; the first is written, the kernel's BO is not (the
 * address leads to the scratch page there)
 */
LOCAL void t_spaces( void )
{
	TBUF		kb = { 0 }, pb = { 0 }, bb = { 0 };
	T_GPUAS		*as = NULL;
	T_GPUJOB	j;
	T_GPUBO		*bos[3];
	BATCH		bt;
	UD		pva = 0x0000123400000000ULL, bva = 0x0000123500000000ULL;
	D		f;
	INT		changed = 0;
	UW		got = 0;
	ER		er = E_NOMEM;

	if ( !tb_make(&kb, PAGE_SIZE) || !tb_make(&pb, PAGE_SIZE) || !tb_make(&bb, PAGE_SIZE) ) goto out;
	as = knl_gpu_as_create(&er);
	if ( as == NULL ) goto out;
	er = knl_gpu_as_bind_at(as, pb.bo, pva);
	if ( er >= E_OK ) er = knl_gpu_as_bind_at(as, bb.bo, bva);
	if ( er < E_OK ) goto out;
	tb_fill(&kb, 0x44444444);
	tb_fill(&pb, 0);
	tb_flush(&kb);
	tb_flush(&pb);
	bt.b = &bb;
	bt.n = 0;
	store_dw(&bt, pva + 16, 0x5a5a0001);
	store_dw(&bt, kb.gpu + 16, 0x5a5a0002);		/* the kernel's BO's address, not mapped in this space */
	knl_memset(&j, 0, sizeof(j));
	j.kind = GPU_JOB_BATCH;
	j.u.batch.start_lo = (UW)bva;
	j.u.batch.start_hi = (UW)( bva >> 32 );
	j.u.batch.len = bend(&bt);
	j.u.batch.engine = GPU_ENG_RENDER;
	j.u.batch.flags = GPU_BATCH_FRESH;
	bos[0] = bb.bo;
	bos[1] = pb.bo;
	bos[2] = kb.bo;
	f = knl_gpu_submit_as(&j, bos, 3, as);
	er = ( f > 0 ) ? knl_gpu_wait(f, 4 * 1000 * 1000) : (ER)f;
	tb_flush(&kb);
	tb_flush(&pb);
	{
		UD	off;

		for ( off = 0; off < PAGE_SIZE; off += 4 ) {
			if ( *tb_at(&kb, off) != 0x44444444 ) changed++;
		}
	}
	got = *tb_at(&pb, 16);
out:
	tm_printf((UB *)"igt test: spaces %s: job %d in a space of its own, its BO %s (%08x), the kernel's BO at %08x: "
		  "%d words changed; %ld tables in the space\n",
		  ( er == E_OK && got == 0x5a5a0001 && changed == 0 ) ? "ok" : "FAIL", er,
		  ( got == 0x5a5a0001 ) ? "written" : "NOT written", got, kb.gpu, changed,
		  ( as != NULL ) ? ((IGTVM *)knl_gpu_as_priv(as))->npt : 0);
	note("spaces", er == E_OK && got == 0x5a5a0001 && changed == 0);
	if ( as != NULL ) {
		if ( bb.bo != NULL ) knl_gpu_as_unbind_at(as, bb.bo, bva);
		if ( pb.bo != NULL ) knl_gpu_as_unbind_at(as, pb.bo, pva);
		knl_gpu_as_put(as);
	}
	tb_drop(&kb);
	tb_drop(&pb);
	tb_drop(&bb);
}

/* A batch that jumps to itself for ever: the job fails, the engine is reset */
LOCAL void t_hang( INT eng )
{
	TBUF	bb = { 0 };
	BATCH	bt;
	T_GPUBO	*bos[1];
	UD	t0;
	ER	er = E_NOMEM;

	if ( tb_make(&bb, PAGE_SIZE) ) {
		bt.b = &bb;
		bt.n = 0;
		bput(&bt, MI_ARB_CHECK);
		bput(&bt, MI_BATCH_BUFFER_START | MI_BATCH_PPGTT);
		bput(&bt, bb.gpu);
		bput(&bt, 0);
		bos[0] = bb.bo;
		t0 = igt_now_us();
		er = igt_kjob(eng, bb.gpu, bend(&bt), bos, 1);
		t0 = igt_now_us() - t0;
		tm_printf((UB *)"igt test: hang %s %s: job %d after %ld ms, resets of the engine %ld\n", eng_name(eng),
			  ( er == E_IO ) ? "ok" : "FAIL", er, t0 / 1000, knl_igt.eng[eng].resets);
	}
	note("hang", er == E_IO);
	tb_drop(&bb);
}

EXPORT void igt_test_start( void )
{
	INT	k;

	t_pass = t_run = 0;
	t_failed[0] = '\0';
	tm_printf((UB *)"igt test: start\n");
	t_store(IGT_BCS, "store");
	t_store(IGT_RCS, "store");
	t_fill(64, 64, "fill");
	t_fill(100, 37, "fill uneven");
	t_fill(1920, 1080, "fill large");
	t_copy(256, 256, 0, 0, 0, 0, "copy");
	t_copy(173, 61, 7, 3, 21, 5, "copy unaligned");
	t_queue();
	t_coherence();
	t_spaces();
	t_hang(IGT_BCS);
	t_store(IGT_BCS, "after hang");
	t_hang(IGT_RCS);
	t_store(IGT_RCS, "after hang");
	for ( k = 0; k < IGT_NENG; k++ ) {
		IGTENG	*e = &knl_igt.eng[k];

		tm_printf((UB *)"igt test: interrupts %s: jobs %ld, user %ld, context switch %ld, errors %ld; "
			  "ended by the look %ld, without word of idle %ld; resets %ld\n", e->name, e->jobs, e->irq_user,
			  e->irq_ctx, e->irq_err, e->by_poll, e->no_idle, e->resets);
		tm_printf((UB *)"igt test: interrupts %s: RING_IMR %08x HWSTAM %08x, its bits of GT IER %04x IMR %04x ISR %04x\n",
			  e->name, igt_erd(e, RING_IMR), igt_erd(e, RING_HWSTAM), ( igt_rd(GT_IER(0)) >> e->shift ) & 0xffff,
			  ( igt_rd(GT_IMR(0)) >> e->shift ) & 0xffff, ( igt_rd(GT_ISR(0)) >> e->shift ) & 0xffff);
	}
	tm_printf((UB *)"igt test: interrupt %d taken %ld times (spurious %ld, the display's %ld%s)\n", knl_igt.intno,
		  knl_igt.irq_n, knl_igt.irq_spurious, knl_igt.irq_display,
		  knl_igt.irq_off ? ", turned off: only the look" : "");
	note("interrupts", knl_igt.intno == 0 || knl_igt.eng[IGT_BCS].irq_user > 0);
	tm_printf((UB *)"igt test: summary %d/%d passed%s%s\n", t_pass, t_run, ( t_failed[0] != '\0' ) ? "; failed: " : "",
		  t_failed);
}

#endif /* X64 */
