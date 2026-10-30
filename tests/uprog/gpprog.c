/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpprog.c
 *	A program the kernel tests start as a process (tests/ktest/ktest_gl.c)
 *	to reach the GPU through SVC class 10 (gp_, include/ts/gpuapp.h,
 *	docs/tessronos/13-gpu.md 13.5), built with lib/libts like any
 *	program; on /boot as GPPROG.ELF.
 *
 *	The start-up argument says what to do. The exit code is 0 when every
 *	step went as it should, or the number of the first step that did not.
 *
 *	bo	contexts and BOs: made, mapped (at the same address each
 *		time, readable and writable), looked at, shared under a name
 *		and taken in by another context, which sees the same pages;
 *		deleted; numbers that are not the caller's refused. Jobs
 *		answer E_NOEXS when there is no GPU.
 *	job ADDR jobs on the GPU the test put in (GPU_KIND_TEST): a job of
 *		nothing and one of control lists on BOs, their fences in
 *		order, a BO waited for, a job naming a number that is not
 *		the context's refused. Then two contexts each submit a job
 *		whose binner list is a BO of their own and whose renderer list
 *		is ADDR (hex), a BO of the kernel's: the test looks at what
 *		each job's address space has there
 *	hold	BOs made and mapped in two contexts, a name shared, and the
 *		program ends with them all held: the test sees they go
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/gpuapp.h>

static UB	arg[16];

static INT bo_test( void )
{
	T_GPUINFO	info;
	T_GPUJOB	job;
	T_GPBO		a, b, c, d;
	UW		*p, *q, *pa;
	INT		ctx, ctx2, name, i;
	UINT		h, fl;
	ER		er;

	ctx = gp_opn_gpu();
	if ( ctx <= 0 ) return 1;
	er = gp_ref_gpu(&info);
	if ( er != E_OK && er != E_NOEXS ) return 2;
	/* without a GPU only BOs outside its MMU are made */
	fl = ( er == E_NOEXS ) ? GPU_BO_NOGPU : 0;
	if ( fl != 0 && gp_cre_bo(ctx, 4096, 0, &c) != E_NOEXS ) return 8;

	if ( gp_cre_bo(ctx, 3 * 4096 - 100, fl, &a) != E_OK ) return 3;
	if ( a.handle == 0 || a.size != 3 * 4096 ) return 4;
	if ( gp_cre_bo(ctx, 4096, GPU_BO_NOGPU, &b) != E_OK || b.handle == a.handle ) return 5;
	if ( gp_cre_bo(ctx, 0, fl, &c) != E_PAR ) return 6;
	if ( gp_cre_bo(ctx, 4096, 0x100, &c) != E_PAR ) return 7;

	/* mapped: zeroed, written and read; the same address when asked again */
	if ( gp_map_bo(ctx, a.handle, (void **)&p) != E_OK || p == NULL ) return 10;
	for ( i = 0; i < 3 * 1024; i++ ) {
		if ( p[i] != 0 ) return 11;
	}
	for ( i = 0; i < 3 * 1024; i++ ) p[i] = 0x5A000000U + (UW)i;
	if ( gp_map_bo(ctx, a.handle, (void **)&q) != E_OK || q != p ) return 12;
	if ( q[3 * 1024 - 1] != 0x5A000000U + 3 * 1024 - 1 ) return 13;
	if ( gp_ref_bo(ctx, a.handle, &c) != E_OK || c.handle != a.handle || c.size != a.size
	  || c.offset != a.offset ) return 14;

	/* shared, and taken in by a second context: the same pages at the same address */
	name = gp_shr_bo(ctx, a.handle);
	if ( name <= 0 ) return 20;
	if ( gp_shr_bo(ctx, a.handle) != name ) return 21;
	ctx2 = gp_opn_gpu();
	if ( ctx2 <= 0 || ctx2 == ctx ) return 22;
	if ( gp_imp_bo(ctx2, name, &d) != E_OK || d.size != a.size ) return 23;
	if ( gp_map_bo(ctx2, d.handle, (void **)&pa) != E_OK || pa != p ) return 24;
	if ( pa[5] != 0x5A000005U ) return 25;
	er = gp_imp_bo(ctx2, name ^ 0x100, &c);		/* the same slot, another generation */
	if ( er != E_NOEXS ) return 26;
	er = gp_ref_gpu(&info);

	/* numbers of another context, and of none, refused */
	if ( gp_del_bo(ctx2, b.handle == d.handle ? b.handle + 1 : b.handle) != E_ID ) return 30;
	if ( gp_map_bo(ctx, 999, (void **)&q) != E_ID ) return 31;
	if ( gp_ref_bo(GP_CTX_MAX + 1, a.handle, &c) != E_ID ) return 32;
	if ( gp_cls_gpu(0) != E_ID ) return 33;
	if ( gp_map_bo(ctx, a.handle, (void **)0x10) != E_MACV ) return 34;

	/* no GPU: a job is refused */
	if ( er == E_NOEXS ) {
		job.kind = GPU_JOB_NOP;
		job.after = 0;
		h = a.handle;
		if ( gp_sub_job(ctx, &job, &h, 1) != E_NOEXS ) return 40;
	}

	/* the second context closed: the first still has its pages */
	if ( gp_del_bo(ctx2, d.handle) != E_OK ) return 50;
	if ( gp_del_bo(ctx2, d.handle) != E_ID ) return 51;
	if ( gp_cls_gpu(ctx2) != E_OK ) return 52;
	if ( gp_cls_gpu(ctx2) != E_ID ) return 53;
	if ( p[7] != 0x5A000007U ) return 54;

	/* deleted: the name goes with the number it was shared from */
	if ( gp_del_bo(ctx, a.handle) != E_OK ) return 60;
	ctx2 = gp_opn_gpu();
	if ( gp_imp_bo(ctx2, name, &d) != E_NOEXS ) return 61;
	if ( gp_cls_gpu(ctx2) != E_OK ) return 62;
	if ( gp_cls_gpu(ctx) != E_OK ) return 63;
	return 0;
}

static INT job_test( void )
{
	T_GPUINFO	info;
	T_GPUJOB	job;
	T_GPBO		a, b, c;
	UINT		h[2], probe = 0;
	D		f1, f2;
	INT		ctx, ctx2, i;

	if ( gp_ref_gpu(&info) != E_OK || info.kind != GPU_KIND_TEST ) return 100;
	ctx = gp_opn_gpu();
	if ( ctx <= 0 ) return 101;
	if ( gp_cre_bo(ctx, 8192, 0, &a) != E_OK || a.offset == 0 ) return 102;
	if ( gp_cre_bo(ctx, 4096, 0, &b) != E_OK || b.offset == 0 || b.offset == a.offset ) return 103;
	h[0] = a.handle;
	h[1] = b.handle;

	job.kind = GPU_JOB_NOP;
	job.after = 0;
	f1 = gp_sub_job(ctx, &job, h, 0);
	if ( f1 <= 0 ) return 104;
	job.kind = GPU_JOB_CL;
	job.after = f1;
	job.u.cl.bcl_start = a.offset;
	job.u.cl.bcl_end = a.offset + 64;
	job.u.cl.rcl_start = a.offset + 4096;
	job.u.cl.rcl_end = a.offset + 4096 + 64;
	job.u.cl.qma = b.offset;
	job.u.cl.qms = 4096;
	job.u.cl.qts = 0;
	job.u.cl.flags = GPU_CL_FLUSH_CACHE;
	f2 = gp_sub_job(ctx, &job, h, 2);
	if ( f2 <= f1 ) return 105;
	if ( gp_wai_fen(ctx, f2, 2000000) != E_OK ) return 106;
	if ( gp_wai_fen(ctx, f1, 0) != E_OK ) return 107;
	if ( gp_wai_bo(ctx, a.handle, 2000000) != E_OK ) return 108;
	h[1] = 777;
	if ( gp_sub_job(ctx, &job, h, 2) != E_ID ) return 109;
	if ( gp_sub_job(ctx, &job, h, GP_JOB_BO_MAX + 1) != E_PAR ) return 110;
	if ( gp_wai_fen(GP_CTX_MAX, f2, 0) != E_ID ) return 111;

	/* a job of each of two contexts, both naming the kernel's BO */
	for ( i = 4; arg[i] != '\0'; i++ ) {
		UB d = arg[i];

		probe = ( probe << 4 ) | (UINT)( ( d <= '9' ) ? d - '0' : d - 'a' + 10 );
	}
	ctx2 = gp_opn_gpu();
	if ( ctx2 <= 0 ) return 113;
	if ( gp_cre_bo(ctx2, 8192, 0, &c) != E_OK ) return 114;
	job.kind = GPU_JOB_CL;
	job.after = 0;
	job.u.cl.bcl_start = a.offset;
	job.u.cl.bcl_end = a.offset + 64;
	job.u.cl.rcl_start = probe;
	job.u.cl.rcl_end = probe + 64;
	job.u.cl.qma = 0;
	job.u.cl.qms = 0;
	h[0] = a.handle;
	f1 = gp_sub_job(ctx, &job, h, 1);
	job.u.cl.bcl_start = c.offset;
	job.u.cl.bcl_end = c.offset + 64;
	h[0] = c.handle;
	f2 = gp_sub_job(ctx2, &job, h, 1);
	if ( f1 <= 0 || f2 <= f1 ) return 115;
	if ( gp_wai_fen(ctx2, f2, 2000000) != E_OK ) return 116;
	if ( gp_cls_gpu(ctx2) != E_OK ) return 117;
	if ( gp_cls_gpu(ctx) != E_OK ) return 112;
	return 0;
}

static INT hold_test( void )
{
	T_GPUINFO	info;
	T_GPBO		a;
	void		*p;
	INT		ctx, ctx2, i;
	UINT		fl = ( gp_ref_gpu(&info) == E_OK ) ? 0 : GPU_BO_NOGPU;

	ctx = gp_opn_gpu();
	ctx2 = gp_opn_gpu();
	if ( ctx <= 0 || ctx2 <= 0 ) return 200;
	for ( i = 0; i < 70; i++ ) {			/* more than a new context has room for */
		if ( gp_cre_bo(i % 2 ? ctx : ctx2, 4096 * ( 1 + i % 3 ), fl, &a) != E_OK ) return 201;
		if ( i % 4 == 0 && gp_map_bo(i % 2 ? ctx : ctx2, a.handle, &p) != E_OK ) return 202;
	}
	if ( gp_shr_bo(ctx, a.handle) <= 0 ) return 203;
	tm_putstring((CONST UB *)"gpprog: holding 70 BOs in 2 contexts\n");
	return 0;				/* ends with them all held */
}

#ifdef __x86_64__
/*
 * place: BOs placed where the program chooses (gp_bnd_bo), for a GPU
 * whose user side driver chooses its addresses; the test's GPU records
 * what was entered in the context's space and taken out. A BO placed
 * over part of another takes the other out of the space whole, and the
 * other, moved or deleted afterwards, leaves the new one's place be (a
 * driver that keeps unused BOs for later gives one's address to another
 * before it gives the first a new one). Two BOs are left placed when the
 * program ends.
 */
static INT place_test( void )
{
	T_GPBO	a, b, c, d;
	INT	ctx;

	ctx = gp_opn_gpu();
	if ( ctx <= 0 ) return 300;
	if ( gp_cre_bo(ctx, 8192, 0, &a) != E_OK ) return 301;
	if ( a.offset != 0 ) return 302;			/* nowhere until placed */
	if ( gp_bnd_bo(ctx, a.handle, 0xffff800000123000ULL) != E_OK ) return 303;	/* canonical */
	if ( gp_bnd_bo(ctx, a.handle, 0xffff800000123000ULL) != E_OK ) return 304;	/* there already */
	if ( gp_bnd_bo(ctx, a.handle, 0x200000) != E_OK ) return 305;		/* moved */
	if ( gp_bnd_bo(ctx, 999, 0x300000) != E_ID ) return 306;
	if ( gp_bnd_bo(ctx, a.handle, 0) != E_PAR ) return 307;
	if ( gp_cre_bo(ctx, 4096, 0, &b) != E_OK ) return 308;
	if ( gp_bnd_bo(ctx, b.handle, 0x400000) != E_OK ) return 309;
	if ( gp_del_bo(ctx, b.handle) != E_OK ) return 310;		/* taken out with it */
	if ( gp_cre_bo(ctx, 8192, 0, &c) != E_OK ) return 311;
	if ( gp_bnd_bo(ctx, c.handle, 0x600000) != E_OK ) return 312;
	if ( gp_cre_bo(ctx, 4096, 0, &d) != E_OK ) return 313;
	if ( gp_bnd_bo(ctx, d.handle, 0x601000) != E_OK ) return 314;		/* over c's second page */
	if ( gp_bnd_bo(ctx, c.handle, 0x700000) != E_OK ) return 315;		/* c, no longer placed, elsewhere */
	if ( gp_del_bo(ctx, c.handle) != E_OK ) return 316;
	tm_putstring((CONST UB *)"gpprog: place: done, two BOs left placed\n");
	return 0;
}
#endif

int main( void )
{
	SZ	n = ts_get_arg(arg, sizeof(arg) - 1);
	INT	r;

	if ( n < 0 ) n = 0;
	arg[n] = '\0';
	if ( arg[0] == 'b' )		r = bo_test();
	else if ( arg[0] == 'j' )	r = job_test();
	else if ( arg[0] == 'h' )	r = hold_test();
#ifdef __x86_64__
	else if ( arg[0] == 'p' )	r = place_test();
#endif
	else				r = 99;
	return r;
}
