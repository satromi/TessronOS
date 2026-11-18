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
 *	gsurf	a window's work area shown from two BOs (OB_WR_GSURF), step
 *		by step as the test asks (gsurf_test below)
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/gpuapp.h>
#include <tk/syscall.h>
#include <ts/proc.h>
#include <ts/ob.h>

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
	if ( gp_ref_bo(0x7FFF0000, a.handle, &c) != E_ID ) return 32;	/* a context nobody has */
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
	if ( gp_wai_fen(0x7FFF0000, f2, 0) != E_ID ) return 111;

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

/*
 * gsurf: a window "ktgsurf" shown from two BOs (OB_WR_GSURF of
 * include/ts/ob.h): BO 0 blue, given and shown; told when it is shown.
 * Keys that are not the BOs', that may not read them, rows of a bad
 * length and one BO twice are refused, and so is a show before any are
 * given. A semaphore made says each step is done. Then, by messages
 * from the test: 1, BO 1 blue with (10, 10)-(50, 40) green, shown by
 * that rectangle, and told; 2, the BOs let go of; 3, given again and
 * BO 0 shown, and told; 4 (the window was closed) a show is refused and
 * the program ends.
 */
#define GS_BLUE		0x0000C0U
#define GS_GREEN	0x00C000U

static T_TSMSG	gs_msg;

static BOOL gs_step( void )
{
	T_CSEM	cs;

	memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 1;
	return (BOOL)( tk_cre_sem(&cs) > 0 );
}

static void gs_fill( UW *p, UINT pitch, INT l, INT t, INT r, INT b, UW colour )
{
	INT	x, y;

	for ( y = t; y < b; y++ ) {
		UW	*row = (UW *)( (UB *)p + (SZ)y * pitch );

		for ( x = l; x < r; x++ ) row[x] = colour;
	}
}

/* Until the window is told BO idx is shown under serial; FALSE after 10 s */
static BOOL gs_told( ID port, INT idx, UW serial )
{
	T_OBNTM	ntm;
	SZ	asz;
	INT	t;

	for ( t = 0; t < 1000; t++ ) {
		while ( ob_rea_rec(port, 0, 0, &ntm, sizeof(ntm), &asz) == E_OK && asz == (SZ)sizeof(ntm) ) {
			if ( ntm.event == OB_E_CHANGE && ntm.recno == OB_WR_GSURF
			  && ntm.code == (UINT)idx && (UW)ntm.x == serial ) {
				return TRUE;
			}
		}
		tk_dly_tsk(10);
	}
	return FALSE;
}

static ER gs_write( ID kw, CONST T_OBWGSURF *g )
{
	SZ	asz;

	return ob_wri_rec(kw, OB_WR_GSURF, 0, g, sizeof(*g), &asz);
}

static INT gsurf_test( void )
{
	CONST char	*j = "{\"rect\":[300,200,560,380],\"attr\":7}";
	T_OBCRE		c;
	T_OBGBMAKE	mk;
	T_OBNTF		req;
	T_OBWPOS	wp;
	T_OBWGSURF	g;
	T_OBWGSTAT	st;
	T_GPBO		bo[2];
	TS_UUID		win, ch, bu[2];
	UW		*p[2];
	UINT		pitch;
	SZ		asz;
	INT		ctx, ww, wh, i;
	ID		kw, port, key[2], weak;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"ktgsurf";
	c.json = (CONST UB *)j;
	for ( c.jsonsz = 0; j[c.jsonsz] != '\0'; c.jsonsz++ ) ;
	if ( ob_cre_obj(&c, &win) != E_OK ) return 400;
	kw = ob_opn_obj(&win, OB_OP_R | OB_OP_W);
	if ( kw <= 0 ) return 401;
	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) != E_OK ) return 402;
	ww = wp.wright - wp.wleft;
	wh = wp.wbottom - wp.wtop;
	if ( ww <= 60 || wh <= 50 ) return 403;
	pitch = ( (UINT)ww * 4 + OB_GS_PITCH - 1 ) / OB_GS_PITCH * OB_GS_PITCH;

	/* the BOs, drawn by the processor here as the GPU would draw them */
	ctx = gp_opn_gpu();
	if ( ctx <= 0 ) return 404;
	for ( i = 0; i < 2; i++ ) {
		if ( gp_cre_bo(ctx, (SZ)pitch * wh, GPU_BO_NOGPU, &bo[i]) != E_OK ) return 405;
		if ( gp_map_bo(ctx, bo[i].handle, (void **)&p[i]) != E_OK ) return 406;
		memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_GPUBO;
		mk.ctx = ctx;
		mk.handle = bo[i].handle;
		mk.name = 0;
		c.arg = &mk;
		c.argsz = sizeof(mk);
		if ( ob_cre_obj(&c, &bu[i]) != E_OK ) return 407;
		key[i] = ob_opn_obj(&bu[i], OB_OP_R);
		if ( key[i] <= 0 ) return 408;
	}
	gs_fill(p[0], pitch, 0, 0, ww, wh, GS_BLUE);

	/* told when a BO is shown */
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) != E_OK ) return 409;
	port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( port <= 0 ) return 410;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	if ( ob_ntf_evt(kw, OB_WR_GSURF, &req, port) <= 0 ) return 411;

	/* refused */
	memset(&g, 0, sizeof(g));
	g.op = OB_GS_SHOW;
	if ( gs_write(kw, &g) != E_OBJ ) return 420;		/* nothing given yet */
	g.op = OB_GS_SET;
	g.w = ww;
	g.h = wh;
	g.pitch = pitch;
	g.format = OB_GS_XRGB8888;
	g.bo[0] = kw;
	g.bo[1] = key[1];
	if ( gs_write(kw, &g) != E_OBJ ) return 421;		/* a key to the window */
	g.bo[0] = 0x7FFF;
	if ( gs_write(kw, &g) != E_ID ) return 422;		/* no key */
	weak = ob_opn_obj(&bu[0], OB_OP_ATRRD);
	if ( weak <= 0 ) return 423;
	g.bo[0] = weak;
	if ( gs_write(kw, &g) != E_OACV ) return 424;		/* may not read it */
	ob_cls_obj(weak);
	g.bo[0] = key[0];
	g.bo[1] = key[0];
	if ( gs_write(kw, &g) != E_PAR ) return 425;		/* one BO twice */
	g.bo[1] = key[1];
	g.pitch = pitch + 4;
	if ( gs_write(kw, &g) != E_PAR ) return 426;		/* rows of a bad length */
	g.pitch = pitch;
	g.h = wh + 64;
	if ( gs_write(kw, &g) != E_PAR ) return 427;		/* more rows than the BOs hold */
	g.h = wh;
	g.format = 0;
	if ( gs_write(kw, &g) != E_PAR ) return 428;

	/* given, and BO 0 shown */
	g.format = OB_GS_XRGB8888;
	if ( gs_write(kw, &g) != E_OK ) return 430;
	if ( ob_rea_rec(kw, OB_WR_GSURF, 0, &st, sizeof(st), &asz) != E_OK || asz != (SZ)sizeof(st)
	  || !st.set || st.shown != -1 ) return 431;
	ob_cls_obj(key[0]);				/* the window holds the BOs */
	ob_cls_obj(key[1]);
	memset(&g, 0, sizeof(g));
	g.op = OB_GS_SHOW;
	g.idx = 0;
	g.serial = 1;
	if ( gs_write(kw, &g) != E_OK ) return 432;
	if ( !gs_told(port, 0, 1) ) return 433;
	if ( ob_rea_rec(kw, OB_WR_GSURF, 0, &st, sizeof(st), &asz) != E_OK
	  || st.shown != 0 || st.shown_serial != 1 || st.waiting != -1 ) return 434;
	if ( !gs_step() ) return 435;

	for ( ;; ) {
		if ( ts_rcv_msg(&gs_msg, 60000) != E_OK ) return 440;
		switch ( gs_msg.type ) {
		case 1:
			/* BO 1 as BO 0 was, but for the rectangle */
			gs_fill(p[1], pitch, 0, 0, ww, wh, GS_BLUE);
			gs_fill(p[1], pitch, 10, 10, 50, 40, GS_GREEN);
			memset(&g, 0, sizeof(g));
			g.op = OB_GS_SHOW;
			g.idx = 1;
			g.serial = 2;
			g.nrect = 1;
			g.rect[0].left = 10;
			g.rect[0].top = 10;
			g.rect[0].right = 50;
			g.rect[0].bottom = 40;
			if ( gs_write(kw, &g) != E_OK ) return 441;
			if ( !gs_told(port, 1, 2) ) return 442;
			if ( !gs_step() ) return 443;
			break;
		case 2:
			memset(&g, 0, sizeof(g));
			g.op = OB_GS_DROP;
			if ( gs_write(kw, &g) != E_OK ) return 450;
			if ( gs_write(kw, &g) != E_OBJ ) return 451;
			if ( ob_rea_rec(kw, OB_WR_GSURF, 0, &st, sizeof(st), &asz) != E_OK || st.set ) return 452;
			if ( !gs_step() ) return 453;
			break;
		case 3:
			for ( i = 0; i < 2; i++ ) {
				key[i] = ob_opn_obj(&bu[i], OB_OP_R);
				if ( key[i] > 0 ) continue;
				/* the objects went with their keys: made again */
				memset(&c, 0, sizeof(c));
				c.type = OB_T_STORAGE;
				c.sub = OB_S_GPUBO;
				mk.ctx = ctx;
				mk.handle = bo[i].handle;
				mk.name = 0;
				c.arg = &mk;
				c.argsz = sizeof(mk);
				if ( ob_cre_obj(&c, &bu[i]) != E_OK ) return 460;
				key[i] = ob_opn_obj(&bu[i], OB_OP_R);
				if ( key[i] <= 0 ) return 461;
			}
			memset(&g, 0, sizeof(g));
			g.op = OB_GS_SET;
			g.w = ww;
			g.h = wh;
			g.pitch = pitch;
			g.format = OB_GS_XRGB8888;
			g.bo[0] = key[0];
			g.bo[1] = key[1];
			if ( gs_write(kw, &g) != E_OK ) return 462;
			memset(&g, 0, sizeof(g));
			g.op = OB_GS_SHOW;
			g.idx = 0;
			g.serial = 3;
			if ( gs_write(kw, &g) != E_OK ) return 463;
			if ( !gs_told(port, 0, 3) ) return 464;
			if ( !gs_step() ) return 465;
			break;
		case 4:
			memset(&g, 0, sizeof(g));
			g.op = OB_GS_SHOW;
			g.serial = 4;
			if ( gs_write(kw, &g) >= E_OK ) return 470;	/* the window is gone */
			return 0;
		default:
			return 480;
		}
	}
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
	else if ( arg[0] == 'g' )	r = gsurf_test();
#ifdef __x86_64__
	else if ( arg[0] == 'p' )	r = place_test();
#endif
	else				r = 99;
	return r;
}
