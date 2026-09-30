/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_time.c
 *	Time management on the tickless timer: monotonic time, delay
 *	accuracy, cyclic and alarm handlers (ms and us), tk_set_tim with
 *	restore, the microsecond wait calls, ticklessness and the RTC.
 *	Setting the time and the zone from a process, through the clock
 *	device and the calendar object (design 12.3.1), with the program
 *	tests/uprog/svcprog.c (/boot/SVCPROG.ELF).
 *
 *	Tolerances allow for QEMU scheduling jitter on the host; the lower
 *	bounds are exact ("never early").
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/dt.h>
#include <ts/tsfs.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>

IMPORT UD	knl_timer_irq_count;		/* timer interrupts taken */

#define TOL_MS		20			/* lateness tolerance (ms) */
#define TOL_NS		(3 * 1000000ULL)	/* lateness tolerance (ns) */
#define TOL_LATE_NS	(50 * 1000000ULL)	/* the same for a handler call */

LOCAL volatile INT	cyc_count;
LOCAL volatile INT	alm_count;
LOCAL volatile void	*hdr_exinf;
LOCAL volatile UD	hdr_time;

LOCAL void cyc_handler( void *exinf )
{
	cyc_count++;
	hdr_exinf = exinf;
}

LOCAL void alm_handler( void *exinf )
{
	alm_count++;
	hdr_exinf = exinf;
	ts_get_mono((UD *)&hdr_time);
}

LOCAL UD systim_ms( CONST SYSTIM *t )
{
	return ((UD)(UW)t->hi << 32) | t->lo;
}

LOCAL UD mono( void )
{
	UD	ns = 0;

	ts_get_mono(&ns);
	return ns;
}

/* operating time is monotonic and advances by the delay */
LOCAL void test_monotonic( void )
{
	SYSTIM	t1, t2;
	UD	d, n1, n2;
	INT	i;

	KT_ASSERT_ER(tk_get_otm(&t1), E_OK);
	for ( i = 0; i < 5; i++ ) {
		KT_ASSERT_ER(tk_get_otm(&t2), E_OK);
		KT_ASSERT(systim_ms(&t2) >= systim_ms(&t1));
		t1 = t2;
	}
	n1 = mono();
	for ( i = 0; i < 100; i++ ) {
		n2 = mono();
		KT_ASSERT(n2 > n1);			/* strictly increasing at ns resolution */
		n1 = n2;
	}
	KT_ASSERT_ER(tk_get_otm(&t1), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(100), E_OK);
	KT_ASSERT_ER(tk_get_otm(&t2), E_OK);
	d = systim_ms(&t2) - systim_ms(&t1);
	KT_ASSERT(d >= 100 && d <= 100 + TOL_MS);
}

/* tk_set_tim moves the system time but not the operating time; restored at the end */
LOCAL void test_set_tim( void )
{
	SYSTIM	tim, otm1, otm2, now, saved;
	UD	d;

	KT_ASSERT_ER(tk_get_tim(&saved), E_OK);

	tim.hi = 0x12;
	tim.lo = 0x34567890;
	KT_ASSERT_ER(tk_get_otm(&otm1), E_OK);
	KT_ASSERT_ER(tk_set_tim(&tim), E_OK);
	KT_ASSERT_ER(tk_get_tim(&now), E_OK);
	d = systim_ms(&now) - systim_ms(&tim);
	KT_ASSERT(d < TOL_MS);
	KT_ASSERT_ER(tk_dly_tsk(30), E_OK);
	KT_ASSERT_ER(tk_get_tim(&now), E_OK);
	d = systim_ms(&now) - systim_ms(&tim);
	KT_ASSERT(d >= 30 && d < 30 + TOL_MS);
	KT_ASSERT_ER(tk_get_otm(&otm2), E_OK);
	d = systim_ms(&otm2) - systim_ms(&otm1);
	KT_ASSERT(d >= 30 && d < 30 + TOL_MS);	/* unaffected by tk_set_tim */

	/* put the clock back: saved + elapsed operating time */
	d = systim_ms(&saved) + (systim_ms(&otm2) - systim_ms(&otm1));
	tim.hi = (W)(d >> 32);
	tim.lo = (UW)d;
	KT_ASSERT_ER(tk_set_tim(&tim), E_OK);
}

/* ms and us readings of the same clocks agree */
LOCAL void test_get_tim_u( void )
{
	SYSTIM		tim;
	SYSTIM_U	tim_u, otm_u;
	UW		ofs;
	UD		ms;

	KT_ASSERT_ER(tk_get_tim(&tim), E_OK);
	KT_ASSERT_ER(tk_get_tim_u(&tim_u, &ofs), E_OK);
	KT_ASSERT(ofs < 1000);
	ms = (UD)tim_u / 1000;
	KT_ASSERT(ms >= systim_ms(&tim) && ms <= systim_ms(&tim) + TOL_MS);

	KT_ASSERT_ER(tk_get_otm_u(&otm_u, NULL), E_OK);
	KT_ASSERT((UD)otm_u * 1000 <= mono());
	KT_ASSERT(mono() - (UD)otm_u * 1000 < TOL_NS);
}

/* tk_dly_tsk_u: never early, late by less than the tolerance */
LOCAL void test_dly_us( void )
{
	UD	t1, t2, d, worst = 0;
	INT	i;

	for ( i = 0; i < 10; i++ ) {
		t1 = mono();
		KT_ASSERT_ER(tk_dly_tsk_u(1000), E_OK);
		t2 = mono();
		d = t2 - t1;
		KT_ASSERT(d >= 1000000ULL);
		KT_ASSERT(d < 1000000ULL + TOL_NS);
		if ( d - 1000000ULL > worst ) worst = d - 1000000ULL;
	}
	tm_printf((UB*)"  tk_dly_tsk_u(1000): worst lateness %d ns\n", (INT)worst);

	t1 = mono();
	KT_ASSERT_ER(tk_dly_tsk_u(0), E_OK);		/* no wait */
	KT_ASSERT(mono() - t1 < TOL_NS);
}

/* microsecond waits on the synchronisation objects */
LOCAL void test_wait_us( void )
{
	T_CSEM	csem;
	T_CFLG	cflg;
	T_CMTX	cmtx;
	ID	sem, flg, mtx;
	UINT	ptn;
	UD	t1, d;

	csem.exinf = NULL; csem.sematr = TA_TFIFO | TA_FIRST; csem.isemcnt = 0; csem.maxsem = 1;
	sem = tk_cre_sem(&csem);
	KT_ASSERT(sem > 0);
	cflg.exinf = NULL; cflg.flgatr = TA_TFIFO | TA_WMUL; cflg.iflgptn = 0;
	flg = tk_cre_flg(&cflg);
	KT_ASSERT(flg > 0);
	cmtx.exinf = NULL; cmtx.mtxatr = TA_TFIFO;
	mtx = tk_cre_mtx(&cmtx);
	KT_ASSERT(mtx > 0);

	t1 = mono();
	KT_ASSERT_ER(tk_slp_tsk_u(2500), E_TMOUT);
	d = mono() - t1;
	KT_ASSERT(d >= 2500000ULL && d < 2500000ULL + TOL_NS);

	t1 = mono();
	KT_ASSERT_ER(tk_wai_sem_u(sem, 1, 1500), E_TMOUT);
	d = mono() - t1;
	KT_ASSERT(d >= 1500000ULL && d < 1500000ULL + TOL_NS);

	KT_ASSERT_ER(tk_wai_flg_u(flg, 1, TWF_ORW, &ptn, TMO_POL), E_TMOUT);
	KT_ASSERT_ER(tk_set_flg(flg, 1), E_OK);
	KT_ASSERT_ER(tk_wai_flg_u(flg, 1, TWF_ORW, &ptn, 1000), E_OK);
	KT_ASSERT_EQ(ptn, 1);

	KT_ASSERT_ER(tk_loc_mtx_u(mtx, TMO_FEVR), E_OK);
	KT_ASSERT_ER(tk_unl_mtx(mtx), E_OK);

	KT_ASSERT_ER(tk_slp_tsk_u(-2), E_PAR);

	KT_ASSERT_ER(tk_del_mtx(mtx), E_OK);
	KT_ASSERT_ER(tk_del_flg(flg), E_OK);
	KT_ASSERT_ER(tk_del_sem(sem), E_OK);
}

/* cyclic handler (ms): count over a window, stop, phase */
LOCAL void test_cyclic( void )
{
	T_CCYC	ccyc;
	T_RCYC	rcyc;
	ID	id;

	ccyc.exinf = (void *)0x77;
	ccyc.cycatr = TA_HLNG;
	ccyc.cychdr = (FP)cyc_handler;
	ccyc.cyctim = 20;
	ccyc.cycphs = 0;
	id = tk_cre_cyc(&ccyc);
	KT_ASSERT(id > 0);

	cyc_count = 0;
	KT_ASSERT_ER(tk_sta_cyc(id), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(205), E_OK);
	KT_ASSERT_ER(tk_stp_cyc(id), E_OK);
	KT_ASSERT(cyc_count >= 9 && cyc_count <= 11);
	KT_ASSERT_EQ((BINT)hdr_exinf, 0x77);
	KT_ASSERT_ER(tk_ref_cyc(id, &rcyc), E_OK);
	KT_ASSERT_EQ(rcyc.cycstat, TCYC_STP);

	cyc_count = 0;
	KT_ASSERT_ER(tk_dly_tsk(50), E_OK);
	KT_ASSERT_EQ(cyc_count, 0);			/* stopped */
	KT_ASSERT_ER(tk_del_cyc(id), E_OK);
	KT_ASSERT_ER(tk_sta_cyc(id), E_NOEXS);
}

/* cyclic handler (us): 2.5ms period without drift over 200ms */
LOCAL void test_cyclic_u( void )
{
	T_CCYC_U	ccyc;
	T_RCYC_U	rcyc;
	ID		id;
	UD		t0, t1;
	INT		expect;

	ccyc.exinf = (void *)0x78;
	ccyc.cycatr = TA_HLNG | TA_STA;
	ccyc.cychdr = (FP)cyc_handler;
	ccyc.cyctim_u = 2500;
	ccyc.cycphs_u = 2500;
	cyc_count = 0;
	t0 = mono();
	id = tk_cre_cyc_u(&ccyc);
	KT_ASSERT(id > 0);
	KT_ASSERT_ER(tk_ref_cyc_u(id, &rcyc), E_OK);
	KT_ASSERT(rcyc.lfttim_u > 0 && rcyc.lfttim_u <= 2500);
	KT_ASSERT_EQ(rcyc.cycstat, TCYC_STA);

	KT_ASSERT_ER(tk_dly_tsk(200), E_OK);
	KT_ASSERT_ER(tk_stp_cyc(id), E_OK);
	t1 = mono();
	expect = (INT)((t1 - t0) / 2500000ULL);		/* handlers due in the window */
	/* Never more than are due: the period is kept by adding to the
	   previous expiry, so activations cannot bunch up. Some are
	   swallowed when the emulator stalls a processor, and how many
	   depends on how busy the machine underneath is, so the floor
	   is a share of what was due rather than a fixed number. */
	if ( cyc_count < expect ) {
		tm_printf((UB*)"  cyclic: %d of %d due in the window\n",
				  (INT)cyc_count, (INT)expect);
	}
	KT_ASSERT(cyc_count <= expect);
	KT_ASSERT(cyc_count >= expect - expect / 4);
	KT_ASSERT_EQ((BINT)hdr_exinf, 0x78);
	KT_ASSERT_ER(tk_del_cyc(id), E_OK);
}

/* alarm handler (ms): fires once after the relative time; stop cancels */
LOCAL void test_alarm( void )
{
	T_CALM	calm;
	T_RALM	ralm;
	ID	id;

	calm.exinf = (void *)0x99;
	calm.almatr = TA_HLNG;
	calm.almhdr = (FP)alm_handler;
	id = tk_cre_alm(&calm);
	KT_ASSERT(id > 0);

	alm_count = 0;
	KT_ASSERT_ER(tk_sta_alm(id, 30), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(10), E_OK);
	KT_ASSERT_EQ(alm_count, 0);
	KT_ASSERT_ER(tk_ref_alm(id, &ralm), E_OK);
	KT_ASSERT(ralm.lfttim > 0 && ralm.lfttim <= 30);
	KT_ASSERT_ER(tk_dly_tsk(40), E_OK);
	KT_ASSERT_EQ(alm_count, 1);
	KT_ASSERT_EQ((BINT)hdr_exinf, 0x99);
	KT_ASSERT_ER(tk_dly_tsk(40), E_OK);
	KT_ASSERT_EQ(alm_count, 1);			/* one shot */

	KT_ASSERT_ER(tk_sta_alm(id, 30), E_OK);
	KT_ASSERT_ER(tk_stp_alm(id), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(50), E_OK);
	KT_ASSERT_EQ(alm_count, 1);			/* cancelled */
	KT_ASSERT_ER(tk_del_alm(id), E_OK);
}

/* alarm handler (us): 1.5ms, timing of the handler call */
LOCAL void test_alarm_u( void )
{
	T_CALM		calm;
	T_RALM_U	ralm;
	ID		id;
	UD		t0, d;

	calm.exinf = (void *)0x9a;
	calm.almatr = TA_HLNG;
	calm.almhdr = (FP)alm_handler;
	id = tk_cre_alm(&calm);
	KT_ASSERT(id > 0);

	alm_count = 0;
	t0 = mono();
	KT_ASSERT_ER(tk_sta_alm_u(id, 1500), E_OK);
	KT_ASSERT_ER(tk_ref_alm_u(id, &ralm), E_OK);
	/* A relative time is rounded up onto the slack grid, so what
	   is left can be a grid step more than what was asked for. */
	if ( ralm.lfttim_u > 1500 + CNF_TIMER_SLACK_NS / 1000 ) {
		tm_printf((UB*)"  left %d us after asking for 1500\n",
				  (INT)ralm.lfttim_u);
	}
	KT_ASSERT(ralm.lfttim_u <= 1500 + CNF_TIMER_SLACK_NS / 1000);
	if ( ralm.lfttim_u == 0 ) {
		/* a host stall of over 1.5ms between the two calls: it has fired */
		KT_ASSERT_EQ(alm_count, 1);
	} else {
		KT_ASSERT_EQ(ralm.almstat, TALM_STA);
	}
	KT_ASSERT_ER(tk_dly_tsk_u(4000), E_OK);
	KT_ASSERT_EQ(alm_count, 1);
	KT_ASSERT_EQ((BINT)hdr_exinf, 0x9a);
	d = hdr_time - t0;
	/* Never early. The handler runs from the timer interrupt, which an
	   emulated processor can leave waiting for tens of milliseconds, so
	   the lateness is only held to a generous bound. */
	KT_ASSERT(d >= 1500000ULL);
	KT_ASSERT(d < 1500000ULL + TOL_LATE_NS);
	tm_printf((UB*)"  alarm lateness: %d ns\n", (INT)(d - 1500000ULL));
	KT_ASSERT_ER(tk_ref_alm_u(id, &ralm), E_OK);
	KT_ASSERT_EQ(ralm.lfttim_u, 0);
	KT_ASSERT_EQ(ralm.almstat, TALM_STP);
	KT_ASSERT_ER(tk_del_alm(id), E_OK);
}

/* no periodic tick: a 200ms delay takes only a few timer interrupts */
LOCAL void test_tickless( void )
{
	UD	n0, n1;

	n0 = knl_timer_irq_count;
	KT_ASSERT_ER(tk_dly_tsk(200), E_OK);
	n1 = knl_timer_irq_count;
	tm_printf((UB*)"  timer interrupts during a 200ms delay: %d\n", (INT)(n1 - n0));
#ifdef RPI5
	/* the Ethernet receiver looks at its ring on a timer (no interrupt yet) */
	KT_SKIP("the Ethernet receiver polls, so other timers run during the delay");
#endif
#if TS_NETSTACK_NETBSD
	/* the timers of the NetBSD stack are counted in ticks of its own clock */
	KT_SKIP("the network stack's clock ticks 100 times a second");
#endif
	KT_ASSERT(n1 - n0 >= 1 && n1 - n0 <= 3);
}

/* the system clock was initialised from the RTC at boot */
LOCAL void test_rtc( void )
{
	SYSTIM	tim;
	UD	ms;

	KT_ASSERT_ER(tk_get_tim(&tim), E_OK);
	ms = systim_ms(&tim);
	if ( ms < 365ULL * 86400 * 1000 ) {
		KT_SKIP("no RTC (system time near 1985)");
	}
	/* between 2020-01-01 and 2100-01-01 in TRON time (ms since 1985-01-01) */
	KT_ASSERT(ms > 1104537600000ULL && ms < 3629059200000ULL);
	tm_printf((UB*)"  RTC: %d days since 1985-01-01\n", (INT)(ms / 86400000ULL));
}

/* ---------------------------------------------------------------- the clock and the calendar */

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_CLOCK	22		/* as tests/uprog/svcprog.c */
#define ZONE		540		/* minutes east: what the tests set */

#ifdef CNF_OB_SYSVOL
#define SYSVOL		CNF_OB_SYSVOL
#define SYSVOL_FLAGS	TSFS_STORE_BLK
#else
#define SYSVOL		CNF_OB_STORE
#define SYSVOL_FLAGS	0
#endif

LOCAL TS_UUID	cal;			/* the calendar */
LOCAL ID	cport;			/* where the notices come */

/* The program run to set the time and the zone, as the user u or (NULL) the administrator */
LOCAL INT clock_run( CONST TS_UUID *u )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UW	arg[8];
	ID	pid;

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_CLOCK;
	if ( u != NULL ) knl_memcpy(&arg[1], u, sizeof(TS_UUID));
	arg[5] = ZONE;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	if ( pid <= 0 ) return -1;
	psts.exitcd = -1;
	if ( ts_wai_prc(pid, &psts, 10000) < E_OK ) return -2;
	if ( psts.exitcd != 0 ) {
		tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
	}
	return psts.exitcd;
}

/* The notices of changes to the clock waiting at the port */
LOCAL INT changes( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	INT	n = 0;

	while ( ob_rea_rec(cport, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz == (SZ)sizeof(m) ) {
		if ( m.event == OB_E_CHANGE && ts_uuid_cmp(&m.uuid, &ob_uuid_clock) == 0 ) n++;
	}
	return n;
}

/* Whether the calendar's metadata holds the text w */
LOCAL BOOL cal_says( CONST char *w )
{
	UB	*j = (UB *)Kmalloc(OB_ATR_MAX);
	SZ	asz = 0;
	INT	i, k, len = (INT)knl_strlen(w);
	BOOL	yes = FALSE;
	ID	key;

	if ( j == NULL ) return FALSE;
	key = ob_opn_obj(&cal, OB_OP_ATRRD);
	if ( key > 0 && ob_get_atr(key, j, OB_ATR_MAX, &asz) >= E_OK ) {
		for ( i = 0; i + len <= (INT)asz && !yes; i++ ) {
			for ( k = 0; k < len && j[i + k] == (UB)w[k]; k++ ) ;
			yes = (BOOL)( k == len );
		}
	}
	if ( key > 0 ) ob_cls_obj(key);
	Kfree(j);
	return yes;
}

/*
 * A process of a user who is not an administrator is refused every way
 * of setting the time and the zone, and nothing is told; one of the
 * administrator's may, the zone is written into the calendar's
 * metadata, and each change is told on the clock. The clock is what is
 * watched: a key kept on the calendar would keep it from being written.
 */
LOCAL void test_clock_set( void )
{
	CONST char	*uj = "{\"name\":\"ktclock\",\"tessronos\":{\"user\":{\"name\":\"ktclock\",\"groups\":[]}}}";
	T_OBCRE		c;
	T_OBNTF		req;
	T_FSTAT		st;
	TS_UUID		user, ch;
	INT		z0 = 0, z = 0, n;
	ID		kclk;

	if ( fs_stat(SVCPROG, &st) < EX_OK ) KT_SKIP("no " SVCPROG);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CALENDAR, &cal), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)uj;
	c.jsonsz = (SZ)knl_strlen(uj);
	if ( ob_cre_obj(&c, &user) < E_OK ) KT_SKIP("no store for a user object");
	KT_ASSERT_ER(ob_set_pwd(&user, (CONST UB *)"ktest-pw"), E_OK);

	/* told of what changes on the clock */
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	KT_ASSERT_ER(ob_cre_obj(&c, &ch), E_OK);
	cport = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	kclk = ob_opn_obj(&ob_uuid_clock, OB_OP_ATRRD);
	KT_ASSERT(cport > 0 && kclk > 0);
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	KT_ASSERT(ob_ntf_evt(kclk, OB_REC_ANY, &req, cport) > 0);
	KT_ASSERT_ER(dt_getsystz(&z0), E_OK);
	if ( z0 == ZONE ) {
		KT_ASSERT_ER(dt_setsystz(0), E_OK);	/* so that setting it is a change */
		z0 = 0;
	}
	(void)changes();

	/* refused, and nothing changes */
	KT_ASSERT_EQ(clock_run(&user), 0);
	KT_ASSERT_ER(dt_getsystz(&z), E_OK);
	KT_ASSERT_EQ(z, z0);
	KT_ASSERT_EQ(changes(), 0);

	/* the administrator's: set, kept and told */
	KT_ASSERT_EQ(clock_run(NULL), 0);
	KT_ASSERT_ER(dt_getsystz(&z), E_OK);
	KT_ASSERT_EQ(z, ZONE);
	KT_ASSERT(cal_says("\"tz\":540"));
	tk_dly_tsk(50);
	n = changes();
	tm_printf((UB*)"  told %d times on the clock\n", n);
	KT_ASSERT(n >= 4);			/* three settings of the time, and the zone */

	/* the zone set by the kernel is told too, and one not changed is not */
	KT_ASSERT_ER(dt_setsystz(ZONE), E_OK);
	KT_ASSERT_EQ(changes(), 0);
	KT_ASSERT_ER(dt_setsystz(0), E_OK);
	KT_ASSERT_EQ(changes(), 1);
	KT_ASSERT(cal_says("\"tz\":0}"));
	KT_ASSERT_ER(dt_setsystz(ZONE), E_OK);
	KT_ASSERT_EQ(changes(), 1);

	ob_cls_obj(kclk);
	ob_cls_obj(cport);
	(void)ob_del_obj(&ch);
	KT_ASSERT_ER(ob_del_obj(&user), E_OK);
}

/*
 * The zone is the calendar's: with its volume taken off, the kernel may
 * set the system's zone in memory, but the volume attached again brings
 * back what the calendar keeps. Then the zone as it was before.
 */
LOCAL void test_zone_kept( void )
{
	INT	z = 0, t;
	ER	er = E_OBJ;

	if ( !cal_says("\"tz\":540") ) KT_SKIP("the zone was not set");
	for ( t = 0; t < 20 && ( er = ob_det_vol(SYSVOL) ) == E_OBJ; t++ ) {
		tk_dly_tsk(100);			/* something on it is still open */
	}
	if ( er < E_OK ) {
		tm_printf((UB*)"  %s could not be taken off (%d)\n", SYSVOL, (INT)er);
	}
	KT_ASSERT_ER(er, E_OK);
	if ( er >= E_OK ) {
		KT_ASSERT_ER(dt_setsystz(60), E_OK);	/* no calendar: the kernel's, in memory */
		KT_ASSERT_ER(dt_getsystz(&z), E_OK);
		KT_ASSERT_EQ(z, 60);
		KT_ASSERT_ER(ob_att_vol(SYSVOL, SYSVOL_FLAGS), E_OK);
		KT_ASSERT_ER(dt_getsystz(&z), E_OK);
		KT_ASSERT_EQ(z, ZONE);			/* what the calendar keeps */
	}
	KT_ASSERT_ER(dt_setsystz(0), E_OK);
	KT_ASSERT(cal_says("\"tz\":0}"));
}

EXPORT void ktest_time( void )
{
	KT_RUN(test_monotonic);
	KT_RUN(test_set_tim);
	KT_RUN(test_get_tim_u);
	KT_RUN(test_dly_us);
	KT_RUN(test_wait_us);
	KT_RUN(test_cyclic);
	KT_RUN(test_cyclic_u);
	KT_RUN(test_alarm);
	KT_RUN(test_alarm_u);
	KT_RUN(test_tickless);
	KT_RUN(test_rtc);
	KT_RUN(test_clock_set);
	KT_RUN(test_zone_kept);
}
