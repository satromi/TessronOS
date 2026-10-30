/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_devob.c
 *	The resources that were kernel functions, as device objects
 *	(docs/tessronos/08-object.md 8.4): the random source 乱数, the screen
 *	画面, the input 入力 and each keyboard and pointer, each device on the
 *	USB buses, the system object's records made from them, and the
 *	state of the hardware.
 *
 *	For each: what it is (type, subtype, attributes), its records read
 *	and written, its protection -- a process of a user who is not an
 *	administrator is given what is everyone's and refused the rest
 *	(tests/uprog/svcprog.c, SP_DEVOB) -- the links of its record 0, and
 *	its coming and going told on the list of the devices, with a
 *	keyboard taken out and plugged back in through the QEMU monitor
 *	(tools/qemu_qmp.py). test_measure prints what the object path costs
 *	against the kernel function it replaced.
 *
 *	test_cpu_limit takes the limit of the processors' clock (CPU_MHZ)
 *	and the watchdog (WATCHDOG) from the machine's settings and reads
 *	them back from the hardware's record.
 *
 *	The banks of pins and the temperature and clocks of the Raspberry
 *	Pi are checked on the board at start (objtest:, kernel/sysdepend/
 *	rpi5/objtest.c); here QEMU says what it does not know.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/uuid.h>
#include <ts/hid.h>
#include <ts/disp.h>
#include <ts/usb.h>
#include <ts/snd.h>
#include <ts/proc.h>
#include <ts/time.h>
#include <ts/ob.h>
#include <ts/fs.h>
#include <ts/cpuclk.h>
#include <ts/lockup.h>
#include <ts/wm.h>
#include <ts/conf.h>
#include <ts/om.h>
#include <ts/sysdef.h>
#include <tk/tkernel.h>
#include "../../peripheral_kernel/obj/obj.h"

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_DEVOB	27		/* as tests/uprog/svcprog.c */
#define N_CALLS		2000
#define TEXT_MAX	8192

LOCAL UB	txt[TEXT_MAX];

LOCAL UD mono_ns( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns;
}

LOCAL INT same_bytes( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n && x[i] == y[i]; i++ ) ;
	return ( i == n ) ? 0 : 1;
}

/* Whether the text holds w */
LOCAL BOOL has( CONST UB *t, INT len, CONST char *w )
{
	INT	i, k, n = (INT)knl_strlen(w);

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == n ) return TRUE;
	}
	return FALSE;
}

/* How many lines begin with w */
LOCAL INT lines( CONST UB *t, INT len, CONST char *w )
{
	INT	i = 0, k, n = (INT)knl_strlen(w), cnt = 0;

	while ( i < len ) {
		for ( k = 0; k < n && i + k < len && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == n ) cnt++;
		while ( i < len && t[i] != '\n' ) i++;
		i++;
	}
	return cnt;
}

/* A record of an object into txt: its length or an error */
LOCAL INT rec( CONST TS_UUID *u, UINT ops, INT recno )
{
	SZ	asz = 0;
	ER	er;
	ID	k = ob_opn_obj(u, ops);

	if ( k <= 0 ) return (INT)k;
	er = ob_rea_rec(k, recno, 0, txt, TEXT_MAX - 1, &asz);
	(void)ob_cls_obj(k);
	if ( er < E_OK ) return (INT)er;
	txt[asz] = 0;
	return (INT)asz;
}

/* The attributes of an object into txt */
LOCAL INT atr( CONST TS_UUID *u )
{
	SZ	asz = 0;
	ER	er;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	if ( k <= 0 ) return (INT)k;
	er = ob_get_atr(k, txt, TEXT_MAX - 1, &asz);
	(void)ob_cls_obj(k);
	if ( er < E_OK ) return (INT)er;
	txt[asz] = 0;
	return (INT)asz;
}

/* Whether record 0 of an object links to u */
LOCAL BOOL links_to( CONST TS_UUID *from, CONST TS_UUID *u )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n = rec(from, OB_OP_R, 0);

	(void)ts_uuid_to_str(u, us, sizeof(us));
	return (BOOL)( n > 0 && has(txt, n, us) );
}

/* The list of the devices holds a link to u */
LOCAL BOOL in_list( CONST TS_UUID *u )
{
	return links_to(&ob_uuid_devlist, u);
}

/* ---------------------------------------------------------------- notices */

LOCAL TS_UUID	port_u;
LOCAL T_OBNTM	seen[64];		/* the notices that came and were not yet looked for */
LOCAL INT	nseen = 0;
#define SEEN_MAX	64
LOCAL ID	port = 0;

LOCAL ER port_make( void )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &port_u) < E_OK ) return E_SYS;
	port = ob_opn_obj(&port_u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	return ( port > 0 ) ? E_OK : E_SYS;
}

LOCAL void port_gone( void )
{
	nseen = 0;
	if ( port > 0 ) (void)ob_cls_obj(port);
	(void)ob_del_obj(&port_u);
	port = 0;
}

/* Told of events on an object, to the port; the key that asks */
LOCAL ID watch( CONST TS_UUID *u, UINT events )
{
	T_OBNTF	req;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	if ( k <= 0 ) return k;
	knl_memset(&req, 0, sizeof(req));
	req.events = events;
	if ( ob_ntf_evt(k, OB_REC_ANY, &req, port) <= 0 ) {
		(void)ob_cls_obj(k);
		return E_SYS;
	}
	return k;
}

/* Those that are at the port now, kept with the others */
LOCAL void take_in( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz == (SZ)sizeof(m) ) {
		if ( nseen < SEEN_MAX ) seen[nseen++] = m;
		asz = 0;
	}
}

/*
 * The notices of event ev naming u (recno, or -2 any) that came, or
 * come within ms; those counted are taken, the others kept for the next
 */
LOCAL INT told( CONST TS_UUID *u, UINT ev, INT recno, INT ms )
{
	INT	n = 0, waited = 0, i, k;

	for (;;) {
		take_in();
		for ( i = 0, k = 0; i < nseen; i++ ) {
			if ( seen[i].event == ev && ts_uuid_cmp(&seen[i].uuid, u) == 0
			  && ( recno == -2 || seen[i].recno == recno ) ) {
				n++;
			} else {
				seen[k++] = seen[i];
			}
		}
		nseen = k;
		if ( n > 0 || waited >= ms ) break;
		tk_dly_tsk(50);
		waited += 50;
	}
	return n;
}

LOCAL void port_drain( void )
{
	take_in();
	nseen = 0;
}

/* ---------------------------------------------------------------- the process */

LOCAL TS_UUID	user;
LOCAL BOOL	have_user = FALSE;

LOCAL INT prog_run( UINT what, UINT a2, CONST TS_UUID *u )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UW	arg[8];
	ID	pid;

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_DEVOB;
	arg[1] = what;
	arg[2] = a2;
	if ( u != NULL ) knl_memcpy(&arg[2], u, sizeof(TS_UUID));
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	if ( pid <= 0 ) return -1;
	psts.exitcd = -1;
	if ( ts_wai_prc(pid, &psts, 30000) < E_OK ) return -2;
	return psts.exitcd;
}

/* A user who is not an administrator, made once */
LOCAL BOOL user_made( void )
{
	CONST char	*uj = "{\"name\":\"ktdevob\",\"tessronos\":{\"user\":{\"name\":\"ktdevob\",\"groups\":[]}}}";
	T_OBCRE		c;

	if ( have_user ) return TRUE;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)uj;
	c.jsonsz = (SZ)knl_strlen(uj);
	if ( ob_cre_obj(&c, &user) < E_OK ) return FALSE;
	if ( ob_set_pwd(&user, (CONST UB *)"ktest-pw") < E_OK ) {
		(void)ob_del_obj(&user);
		return FALSE;
	}
	have_user = TRUE;
	return TRUE;
}

/* ---------------------------------------------------------------- the random source */

LOCAL void test_random( void )
{
	T_OBREF	r;
	static UD	prt_b[KT_PRT_UD];	/* a protection with room for its entries */
	T_OBPRT		*prt = (T_OBPRT *)prt_b;
	T_OBREC	rr[4];
	UB	a[64], b[64];
	SZ	asz = 0;
	INT	i, same = 0, cnt = 0;
	ID	k;

	KT_ASSERT_ER(ob_ref_obj(&ob_uuid_random, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_DEVICE);
	KT_ASSERT_EQ(r.sub, OB_S_CHAR);
	KT_ASSERT(has(r.name, OB_NAME_MAX, "乱数"));
	KT_ASSERT(in_list(&ob_uuid_random));

	k = ob_opn_obj(&ob_uuid_random, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(k > 0);
	if ( k <= 0 ) return;
	KT_ASSERT_ER(ob_rea_rec(k, OB_RND_DATA, 0, a, sizeof(a), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(a));
	KT_ASSERT_ER(ob_rea_rec(k, OB_RND_DATA, 0, b, sizeof(b), &asz), E_OK);
	for ( i = 0; i < (INT)sizeof(a); i++ ) {
		if ( a[i] == b[i] ) same++;
	}
	KT_ASSERT(same < 8);
	KT_ASSERT_ER(ob_wri_rec(k, OB_RND_DATA, 0, a, sizeof(a), &asz), E_OK);	/* stirred */
	KT_ASSERT_ER(ob_rea_rec(k, 2, 0, a, sizeof(a), &asz), E_NOEXS);
	KT_ASSERT_ER(ob_lst_rec(k, rr, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, OB_RND_NREC);
	(void)ob_cls_obj(k);

	KT_ASSERT(atr(&ob_uuid_random) > 0 && has(txt, TEXT_MAX, "\"kind\":\"random\""));
	KT_ASSERT(rec(&ob_uuid_random, OB_OP_R, 0) > 0 && has(txt, TEXT_MAX, "<tad"));
	KT_ASSERT_ER(ob_get_prt(&ob_uuid_random, prt, KT_PRT_SIZE, NULL), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&prt->owner, &ob_user_system), 0);
	KT_ASSERT_EQ(prt->mode, 0644);
}

/* ---------------------------------------------------------------- the screen */

LOCAL void test_display( void )
{
	T_DISPSPEC	s, s2;
	T_OBREF		r;
	static UD	prt_b[KT_PRT_UD];	/* a protection with room for its entries */
	T_OBPRT		*prt = (T_OBPRT *)prt_b;
	char		want[48], us[TS_UUID_STRLEN + 1];
	UB		*px;
	SZ		asz = 0;
	UINT		w0, h0;
	INT		n;
	ID		k, kd, ks;

	if ( ts_disp_ref(&s) < E_OK ) KT_SKIP(KT_NO_SCREEN);
	w0 = s.width;
	h0 = s.height;
	KT_ASSERT_ER(ob_ref_obj(&ob_uuid_display, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_DISPLAY);
	KT_ASSERT_EQ(r.nrec, OB_DSP_NREC);
	KT_ASSERT(in_list(&ob_uuid_display));

	/* the mode: as the driver says */
	n = rec(&ob_uuid_display, OB_OP_R, OB_DSP_MODE);
	KT_ASSERT(n > 0);
	tm_sprintf((UB *)want, (UB *)"SIZE\t%dx%d\n", (INT)s.width, (INT)s.height);
	KT_ASSERT(has(txt, n, want));
#if defined(X64)
	KT_ASSERT(has(txt, n, "MODES\t") && has(txt, n, "DRIVER\t"));
#else
	KT_ASSERT(has(txt, n, "MODES\t") && has(txt, n, "DRIVER\tbochs-display"));
#endif
	n = atr(&ob_uuid_display);
	tm_sprintf((UB *)want, (UB *)"\"width\":%d", (INT)s.width);
	KT_ASSERT(n > 0 && has(txt, n, want) && has(txt, n, "\"modes\":[\"640x480\""));

	/* the system object's record 3 is a view of it */
	n = rec(&ob_uuid_system, OB_OP_R, OB_SYS_DISPLAY);
	(void)ts_uuid_to_str(&ob_uuid_display, us, sizeof(us));
	KT_ASSERT(n > 0 && has(txt, n, us) && has(txt, n, "SIZE\t"));

	/* the pixels: only with x */
	KT_ASSERT_ER(ob_get_prt(&ob_uuid_display, prt, KT_PRT_SIZE, NULL), E_OK);
	KT_ASSERT_EQ(prt->mode, 0664);
	KT_ASSERT(prt->nrmask == 1 && OB_PRT_RMASK(prt)[0].recno == OB_DSP_PIXELS
		  && ( OB_PRT_RMASK(prt)[0].ops & OB_OP_EXEC ) != 0);
	KT_ASSERT_EQ(rec(&ob_uuid_display, OB_OP_R, OB_DSP_PIXELS), E_OACV);
	px = (UB *)Kmalloc(4096);
	k = ob_opn_obj(&ob_uuid_display, OB_OP_R | OB_OP_EXEC);
	KT_ASSERT(k > 0 && px != NULL);
	if ( k > 0 && px != NULL ) {
		KT_ASSERT_ER(ob_rea_rec(k, OB_DSP_PIXELS, 0, px, 4096, &asz), E_OK);
		KT_ASSERT_EQ(asz, 4096);
		KT_ASSERT_EQ(same_bytes(px, ts_disp_buffer(), 4096), 0);
		KT_ASSERT_ER(ob_wri_rec(k, OB_DSP_PIXELS, 0, px, 4096, &asz), E_OACV);
	}
	if ( k > 0 ) (void)ob_cls_obj(k);
	if ( px != NULL ) Kfree(px);

	/* another size, told on the object and on the system's view */
	KT_ASSERT_ER(port_make(), E_OK);
	kd = watch(&ob_uuid_display, OB_E_CHANGE);
	ks = watch(&ob_uuid_system, OB_E_CHANGE);
	KT_ASSERT(kd > 0 && ks > 0);
	k = ob_opn_obj(&ob_uuid_display, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(k > 0);
#if defined(X64)
	/* the UEFI's frame buffer keeps the size the loader chose */
	if ( ob_wri_rec(k, OB_DSP_MODE, 0, "800x600", 7, &asz) == E_NOSPT ) {
		tm_printf((UB *)"  the screen keeps its size (UEFI GOP)\n");
		if ( k > 0 ) (void)ob_cls_obj(k);
		if ( kd > 0 ) (void)ob_cls_obj(kd);
		if ( ks > 0 ) (void)ob_cls_obj(ks);
		port_gone();
		return;
	}
#else
	KT_ASSERT_ER(ob_wri_rec(k, OB_DSP_MODE, 0, "800x600", 7, &asz), E_OK);
#endif
	KT_ASSERT(ts_disp_ref(&s2) >= E_OK && s2.width == 800 && s2.height == 600);
	KT_ASSERT(told(&ob_uuid_display, OB_E_CHANGE, OB_DSP_MODE, 2000) >= 1);
	KT_ASSERT(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_DISPLAY, 2000) >= 1);
	KT_ASSERT_ER(ob_wri_rec(k, OB_DSP_MODE, 0, "wide", 4, &asz), E_PAR);
	tm_sprintf((UB *)want, (UB *)"%dx%d", (INT)w0, (INT)h0);
	KT_ASSERT_ER(ob_wri_rec(k, OB_DSP_MODE, 0, want, (SZ)knl_strlen(want), &asz), E_OK);
	KT_ASSERT(ts_disp_ref(&s2) >= E_OK && s2.width == w0 && s2.height == h0);
	if ( k > 0 ) (void)ob_cls_obj(k);
	if ( kd > 0 ) (void)ob_cls_obj(kd);
	if ( ks > 0 ) (void)ob_cls_obj(ks);
	port_gone();
}

/* ---------------------------------------------------------------- the input */

LOCAL INT events_read( ID k, T_HIDEV *ev, INT max )
{
	SZ	asz = 0;

	if ( ob_rea_rec(k, OB_IN_EVENTS, 0, ev, sizeof(T_HIDEV) * max, &asz) < E_OK ) return -1;
	return (INT)( asz / sizeof(T_HIDEV) );
}

LOCAL INT with_code( CONST T_HIDEV *ev, INT n, UINT code )
{
	INT	i, c = 0;

	for ( i = 0; i < n; i++ ) {
		if ( ( ev[i].type == HID_EV_KEY_DOWN || ev[i].type == HID_EV_KEY_UP ) && ev[i].code == code ) c++;
	}
	return c;
}

LOCAL void test_input( void )
{
	T_OBREF		r;
	static UD	prt_b[KT_PRT_UD];	/* a protection with room for its entries */
	T_OBPRT		*prt = (T_OBPRT *)prt_b;
	T_HIDSTAT	st0, st1;
	T_HIDATTR	a, a2;
	T_HIDEV		e[2], got[32];
	SZ		asz = 0;
	INT		n;
	ID		ka, kb, kw;

	KT_ASSERT_ER(ob_ref_obj(&ob_uuid_input, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_INPUT);
	KT_ASSERT_EQ(r.nrec, OB_IN_NREC + 1);
	KT_ASSERT(in_list(&ob_uuid_input));
	n = atr(&ob_uuid_input);
	KT_ASSERT(n > 0 && has(txt, n, "\"kind\":\"input\"") && has(txt, n, "\"keyboards\":"));
	n = rec(&ob_uuid_input, OB_OP_R, OB_IN_STATE);
	KT_ASSERT(n > 0 && has(txt, n, "KEYBOARDS\t"));
	KT_ASSERT_ER(ob_get_prt(&ob_uuid_input, prt, KT_PRT_SIZE, NULL), E_OK);
	KT_ASSERT(prt->nrmask == 1 && OB_PRT_RMASK(prt)[0].recno == OB_IN_EVENTS
		  && ( OB_PRT_RMASK(prt)[0].ops & OB_OP_EXEC ) != 0);
	KT_ASSERT_EQ(rec(&ob_uuid_input, OB_OP_R, OB_IN_EVENTS), E_OACV);

	/* two who look, one who puts in: both see what was put in */
	ka = ob_opn_obj(&ob_uuid_input, OB_OP_R | OB_OP_EXEC);
	kb = ob_opn_obj(&ob_uuid_input, OB_OP_R | OB_OP_EXEC);
	kw = ob_opn_obj(&ob_uuid_input, OB_OP_WRITE | OB_OP_EXEC);
	KT_ASSERT(ka > 0 && kb > 0 && kw > 0);
	KT_ASSERT_ER(ts_hid_stat(&st0), E_OK);
	knl_memset(e, 0, sizeof(e));
	e[0].type = HID_EV_KEY_DOWN;
	e[0].code = 0x68;			/* F13: no window does anything with it */
	e[1].type = HID_EV_KEY_UP;
	e[1].code = 0x68;
	KT_ASSERT_ER(ob_wri_rec(kw, OB_IN_EVENTS, 0, e, sizeof(e), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(e));
	KT_ASSERT_ER(ob_wri_rec(kw, OB_IN_EVENTS, 0, e, 5, &asz), E_PAR);	/* whole events only */
	n = events_read(ka, got, 32);
	KT_ASSERT_EQ(with_code(got, n, 0x68), 2);
	n = events_read(kb, got, 32);
	KT_ASSERT_EQ(with_code(got, n, 0x68), 2);		/* a copy: not taken by the first */
	n = events_read(ka, got, 32);
	KT_ASSERT_EQ(with_code(got, n, 0x68), 0);		/* and each key reads on from where it was */
	/* and they went into the input the window manager takes */
	KT_ASSERT_ER(ts_hid_stat(&st1), E_OK);
	KT_ASSERT(st1.events >= st0.events + 2);

	/* how the input is taken */
	KT_ASSERT_ER(ts_hid_getattr(&a), E_OK);
	knl_memset(&a2, 0, sizeof(a2));
	KT_ASSERT_ER(ob_rea_rec(ka, OB_IN_ATTR, 0, &a2, sizeof(a2), &asz), E_OK);
	KT_ASSERT_EQ(same_bytes(&a, &a2, sizeof(a)), 0);
	KT_ASSERT_ER(ob_wri_rec(kw, OB_IN_ATTR, 0, &a, sizeof(a), &asz), E_OK);

	if ( ka > 0 ) (void)ob_cls_obj(ka);
	if ( kb > 0 ) (void)ob_cls_obj(kb);
	if ( kw > 0 ) (void)ob_cls_obj(kw);
}

/* each keyboard and pointer: an object, with what it is and a link to its USB device */
LOCAL void test_hid_devices( void )
{
	T_HIDINFO	h[16];
	TS_UUID		u, ud, *all;
	T_OBREF		r;
	INT		n, i, len, nabs = 0, nrel = 0, nkbd = 0, cnt = 0, nin = 0;

	n = knl_hid_list(h, 16);
	if ( n <= 0 ) KT_SKIP("no keyboard or pointer");
	for ( i = 0; i < n && i < 16; i++ ) {
#ifdef X64
		if ( h[i].usbdev == 0 ) continue;	/* the i8042's: ktest_pcdev */
#endif
		KT_ASSERT_ER(knl_obinput_uuid(h[i].id, &u), E_OK);
		KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
		KT_ASSERT_EQ(r.sub, OB_S_INPUT);
		KT_ASSERT(r.name[0] == 'h' && r.name[1] == 'i' && r.name[2] == 'd');
		KT_ASSERT(in_list(&u));
		len = atr(&u);
		KT_ASSERT(len > 0 && has(txt, len, "\"vid\":\"") && has(txt, len, "\"pid\":\"")
			  && has(txt, len, "\"port\":\""));
		if ( h[i].keyboard ) {
			nkbd++;
			KT_ASSERT(has(txt, len, "\"kind\":\"keyboard\""));
		} else {
			KT_ASSERT(has(txt, len, "\"kind\":\"pointer\""));
			if ( h[i].absolute ) {
				nabs++;
				KT_ASSERT(has(txt, len, "\"pointing\":\"absolute\""));
			} else {
				nrel++;
				KT_ASSERT(has(txt, len, "\"pointing\":\"relative\""));
			}
		}
		tm_printf((UB *)"  %s %s\n", r.name, h[i].keyboard ? "keyboard"
				   : h[i].absolute ? "pointer, absolute" : "pointer, relative");
		KT_ASSERT_ER(knl_obusb_uuid(h[i].usbdev, &ud), E_OK);
		KT_ASSERT(links_to(&u, &ud));
		KT_ASSERT(links_to(&ud, &u));
		KT_ASSERT(links_to(&u, &ob_uuid_input));
		KT_ASSERT(links_to(&ob_uuid_input, &u));
	}
#if !KT_BOARD
	/* the QEMU machine: two keyboards, a tablet, a mouse */
	KT_ASSERT(nkbd >= 2 && nabs >= 1 && nrel >= 1);
#endif
	all = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	KT_ASSERT(all != NULL);
	if ( all != NULL ) {
		KT_ASSERT_ER(ob_lst_obj(OB_T_DEVICE, OB_S_INPUT, NULL, all, 256, &cnt), E_OK);
		for ( i = 0; i < cnt; i++ ) {
			if ( ob_ref_obj(&all[i], &r) >= E_OK && r.sub == OB_S_INPUT ) nin++;
		}
		KT_ASSERT_EQ(nin, cnt);			/* the list asked by subtype has only those */
		KT_ASSERT_EQ(cnt, n + 1);		/* and 入力 */
		Kfree(all);
	}
}

/* ---------------------------------------------------------------- the USB devices */

LOCAL void test_usb_devices( void )
{
	T_USBDEV	*d;
	T_HIDINFO	h[16];
	TS_UUID		u, f, *all;
	T_OBREF		r;
	char		want[48];
	INT		n, i, k, len, nh, cnt = 0, nlinked = 0;

	d = (T_USBDEV *)Kmalloc(sizeof(T_USBDEV) * 32);
	all = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	KT_ASSERT(d != NULL && all != NULL);
	if ( d == NULL || all == NULL ) {
		if ( d != NULL ) Kfree(d);
		if ( all != NULL ) Kfree(all);
		return;
	}
	n = ts_usb_lst_dev(d, 32);
	nh = knl_hid_list(h, 16);
	if ( n <= 0 ) {
		Kfree(d);
		Kfree(all);
		KT_SKIP("no USB device");
	}
	for ( i = 0; i < n && i < 32; i++ ) {
		KT_ASSERT_ER(knl_obusb_uuid(d[i].dev, &u), E_OK);
		KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
		KT_ASSERT_EQ(r.sub, OB_S_USB);
		KT_ASSERT(in_list(&u));
		len = rec(&u, OB_OP_R, OB_USB_INFO);
		tm_sprintf((UB *)want, (UB *)"VID\t%04x\nPID\t%04x\n", (INT)d[i].vendor, (INT)d[i].product);
		KT_ASSERT(len > 0 && has(txt, len, want) && has(txt, len, "SPEED\t")
			  && has(txt, len, "PORT\t") && has(txt, len, "PRODUCT\t"));
		len = atr(&u);
		KT_ASSERT(len > 0 && has(txt, len, "\"kind\":\"usb\"") && has(txt, len, "\"class\":")
			  && has(txt, len, "\"speed\":\"") && has(txt, len, "\"product\":"));
		tm_printf((UB *)"  %s %04x:%04x\n", r.name, (INT)d[i].vendor, (INT)d[i].product);

		/* what it provides, and the hub it hangs off */
		if ( d[i].devnm[0] != 0 && knl_obdev_uuid(d[i].devnm, &f) >= E_OK ) {
			KT_ASSERT(links_to(&u, &f));
			nlinked++;
		}
		if ( d[i].role == USB_ROLE_AUDIO && knl_obdev_uuid((CONST UB *)SND_DEVNM, &f) >= E_OK ) {
			KT_ASSERT(links_to(&u, &f));
			nlinked++;
		}
		for ( k = 0; k < nh && k < 16; k++ ) {
			if ( h[k].usbdev == d[i].dev && knl_obinput_uuid(h[k].id, &f) >= E_OK ) {
				KT_ASSERT(links_to(&u, &f));
				nlinked++;
			}
		}
		if ( d[i].parent != 0 && knl_obusb_uuid(d[i].parent, &f) >= E_OK ) {
			KT_ASSERT(links_to(&u, &f));
			KT_ASSERT(links_to(&f, &u));
		}
	}
	KT_ASSERT(nlinked >= 1);
	KT_ASSERT_ER(ob_lst_obj(OB_T_DEVICE, OB_S_USB, NULL, all, 256, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, n);

	/* the system object's record 4, made from them */
	len = rec(&ob_uuid_system, OB_OP_R, OB_SYS_USB);
	KT_ASSERT(len > 0);
	KT_ASSERT_EQ(lines(txt, len, "DEVICE\t"), n);
	KT_ASSERT_EQ(lines(txt, len, "OBJECT\t"), n);
	Kfree(d);
	Kfree(all);
}

/* ---------------------------------------------------------------- the hardware's state */

LOCAL void test_hw_state( void )
{
	INT	len;
	T_OBREF	r;

	KT_ASSERT_ER(ob_ref_obj(&ob_uuid_system, &r), E_OK);
	KT_ASSERT_EQ(r.nrec, OB_SYS_NREC);
	len = rec(&ob_uuid_system, OB_OP_R, OB_SYS_HW);
	KT_ASSERT(len > 0);
	tm_printf((UB *)"  %s", txt);
#if KT_BOARD
	KT_ASSERT(has(txt, len, "ARM_HZ\t"));
#else
	/* QEMU: nothing to ask, and said so */
	KT_ASSERT(has(txt, len, "TEMPERATURE\tunknown\n") && has(txt, len, "ARM_HZ\tunknown\n")
		  && has(txt, len, "V3D_HZ\tunknown\n"));
#endif
	KT_ASSERT(rec(&ob_uuid_system, OB_OP_R, 0) > 0 && has(txt, TEXT_MAX, "Records 1 to 9"));
}

/*
 * The limit of the processors' clock (include/ts/cpuclk.h): the clock
 * asked of the firmware is the least of the firmware's most, the
 * setting CPU_MHZ and the fan's slowing, and not below the firmware's
 * least; the setting is taken from the machine's settings with
 * everything else in them, CPU_MHZ_DEFAULT with no number, and shows in
 * the hardware's record with the watchdog's seconds (WATCHDOG). QEMU
 * has no firmware: the limit is kept and nothing is asked.
 */
#define MHZ(x)	( (UW)(x) * 1000000U )

LOCAL INT limit_now( void )
{
	T_CPUCLK	c;

	knl_cpu_clock(&c);
	return (INT)c.limit_mhz;
}

LOCAL void test_cpu_limit( void )
{
	T_CPUCLK	c0, c;
	char		was[32], wd_was[32];
	BOOL		had, wd_had;
	INT		len;

	/* the Raspberry Pi 5's range: 1.5 to 2.4GHz */
	KT_ASSERT_EQ(knl_cpu_want(1800, 0, MHZ(1500), MHZ(2400)), MHZ(1800));
	KT_ASSERT_EQ(knl_cpu_want(CPU_MHZ_MOST, 0, MHZ(1500), MHZ(2400)), MHZ(2400));
	KT_ASSERT_EQ(knl_cpu_want(3000, 0, MHZ(1500), MHZ(2400)), MHZ(2400));	/* held to the most */
	KT_ASSERT_EQ(knl_cpu_want(1000, 0, MHZ(1500), MHZ(2400)), MHZ(1500));	/* and to the least */
	KT_ASSERT_EQ(knl_cpu_want(0, MHZ(1800), MHZ(1500), MHZ(2400)), MHZ(1800));	/* the fan's slowing */
	KT_ASSERT_EQ(knl_cpu_want(2200, MHZ(1800), MHZ(1500), MHZ(2400)), MHZ(1800));
	KT_ASSERT_EQ(knl_cpu_want(1500, MHZ(1800), MHZ(1500), MHZ(2400)), MHZ(1500));	/* never above the limit */
	KT_ASSERT_EQ(knl_cpu_want(2000, 0, MHZ(1500), MHZ(2400)), MHZ(2000));	/* the fan's end: back to the limit */
	/* the Raspberry Pi 4's: 0.6 to 1.8GHz */
	KT_ASSERT_EQ(knl_cpu_want(2200, 0, MHZ(600), MHZ(1800)), MHZ(1800));
	KT_ASSERT_EQ(knl_cpu_want(1500, 0, MHZ(600), MHZ(1800)), MHZ(1500));
	/* a firmware that tells nothing */
	KT_ASSERT_EQ(knl_cpu_want(1800, 0, 0, 0), MHZ(1800));
	KT_ASSERT_EQ(knl_cpu_want(0, 0, 0, 0), 0);

	had = wm_dev_conf_get("CPU_MHZ", was, sizeof(was));
	wd_had = wm_dev_conf_get("WATCHDOG", wd_was, sizeof(wd_was));
	if ( wm_dev_conf_put("CPU_MHZ", "2200") < E_OK ) KT_SKIP("no machine settings object");
	knl_cpu_clock(&c0);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	knl_cpu_clock(&c);
	tm_printf((UB *)"  cpu: limit %d MHz, now %d MHz, the firmware %d..%d MHz, taken %d, asked %d\n",
		  (INT)c.limit_mhz, (INT)( c.hz / 1000000 ), (INT)( c.min_hz / 1000000 ),
		  (INT)( c.max_hz / 1000000 ), (INT)c.taken, (INT)c.applied);
	KT_ASSERT_EQ(c.limit_mhz, 2200);
	KT_ASSERT(c.taken > c0.taken);
#if !KT_BOARD
	KT_ASSERT_EQ(c.applied, 0);		/* nothing to ask */
	KT_ASSERT_EQ(c.hz, 0);
#endif
	len = rec(&ob_uuid_system, OB_OP_R, OB_SYS_HW);
	KT_ASSERT(len > 0 && has(txt, len, "ARM_LIMIT_MHZ\t2200\n"));
	KT_ASSERT(has(txt, len, "UNDERVOLT\t") && has(txt, len, "ARM_HEAT_HZ\t"));

	KT_ASSERT_ER(wm_dev_conf_put("CPU_MHZ", "0"), E_OK);	/* the most */
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(limit_now(), CPU_MHZ_MOST);
	len = rec(&ob_uuid_system, OB_OP_R, OB_SYS_HW);
	KT_ASSERT(len > 0 && has(txt, len, "ARM_LIMIT_MHZ\t0\n"));
	KT_ASSERT_ER(wm_dev_conf_put("CPU_MHZ", ""), E_OK);	/* no value: the default */
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(limit_now(), CPU_MHZ_DEFAULT);
	KT_ASSERT_ER(wm_dev_conf_put("CPU_MHZ", "1500"), E_OK);
	KT_ASSERT_ER(wm_cpu_conf_apply(), E_OK);		/* as at the start: the clock alone */
	KT_ASSERT_EQ(limit_now(), 1500);
	KT_ASSERT_ER(wm_dev_conf_put("CPU_MHZ", "fast"), E_OK);	/* not a number */
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(limit_now(), CPU_MHZ_DEFAULT);

	/* the watchdog with it */
	KT_ASSERT_ER(wm_dev_conf_put("WATCHDOG", "45"), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(knl_wdog_get(NULL), 45);
	len = rec(&ob_uuid_system, OB_OP_R, OB_SYS_HW);
	KT_ASSERT(len > 0 && has(txt, len, "WATCHDOG_S\t45\n") && has(txt, len, "WATCHDOG_HW\t"));
	KT_ASSERT_ER(wm_dev_conf_put("WATCHDOG", "0"), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(knl_wdog_get(NULL), 0);

	/* as it was */
	KT_ASSERT_ER(wm_dev_conf_put("CPU_MHZ", had ? was : ""), E_OK);
	KT_ASSERT_ER(wm_dev_conf_put("WATCHDOG", wd_had ? wd_was : ""), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(knl_wdog_get(NULL), wd_had ? cf_num(wd_was, LK_RESET_S_DEFAULT) : LK_RESET_S_DEFAULT);
}

/* ---------------------------------------------------------------- the wallpaper, a link */

/*
 * The wallpaper is a link of 機器の設定's record 0 (relationship
 * CF_ROLE_WALL): a machine whose settings say it only in the WALLPAPER
 * line, as a system before wrote it, has the link made from the line;
 * and the link, once there, is what is laid, whatever the line says.
 */
LOCAL void test_wall_link( void )
{
	static CONST char plain[] = "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"機器の設定\">\n<document>\n"
				   "<p>機器の設定</p>\n</document>\n</tad>\n";
	TS_UUID	box, conf, pic[2], got;
	UB	*rec0;
	SZ	size = 0;
	INT	n = 0, len;
	char	was[CF_VAL_MAX], line[80], us[40];
	BOOL	had;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_WALL_BOX, &box), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CONF_DEV, &conf), E_OK);
	if ( ob_lst_lnk(&box, NULL, pic, 2, &n) < E_OK || n < 2 ) KT_SKIP("fewer than two pictures in 壁紙");
	rec0 = om_obj_record(&conf, 0, &size);
	KT_ASSERT(rec0 != NULL);
	if ( rec0 == NULL ) return;
	had = wm_dev_conf_get("WALLPAPER", was, sizeof(was));

	/* as a system before left it: the line alone */
	KT_ASSERT_ER(om_obj_record_put(&conf, 0, OB_RT_TAD, 0, (CONST UB *)plain, (SZ)knl_strlen(plain)), E_OK);
	KT_ASSERT(!knl_conf_link_get(SYSDEF_CONF_DEV, CF_ROLE_WALL, &got));
	(void)ts_uuid_to_str(&pic[0], us, sizeof(us));
	tm_sprintf((UB *)line, (UB *)"%s 2", us);
	KT_ASSERT_ER(wm_dev_conf_put("WALLPAPER", line), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT(knl_conf_link_get(SYSDEF_CONF_DEV, CF_ROLE_WALL, &got) && ts_uuid_cmp(&got, &pic[0]) == 0);
	len = rec(&conf, OB_OP_R, 0);
	KT_ASSERT(len > 0 && has(txt, len, "relationship=\"" CF_ROLE_WALL "\"") && has(txt, len, us));

	/* the link moved to the other picture: that one is laid, the line saying the first all the while */
	KT_ASSERT_ER(knl_conf_link_put(SYSDEF_CONF_DEV, CF_ROLE_WALL, &pic[1], "壁紙："), E_OK);
	KT_ASSERT(knl_conf_link_get(SYSDEF_CONF_DEV, CF_ROLE_WALL, &got) && ts_uuid_cmp(&got, &pic[1]) == 0);
	len = rec(&conf, OB_OP_R, 0);
	KT_ASSERT(len > 0 && !has(txt, len, us));		/* one link with the role, in place of the other */
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	if ( kt_screen() ) {
		(void)ts_uuid_to_str(&pic[1], us, sizeof(us));
		len = rec(&ob_uuid_system, OB_OP_R, OB_SYS_WALL);
		KT_ASSERT(len > 0 && has(txt, len, us));
	}

	/* as it was */
	KT_ASSERT_ER(om_obj_record_put(&conf, 0, OB_RT_TAD, 0, rec0, size), E_OK);
	Kfree(rec0);
	KT_ASSERT_ER(wm_dev_conf_put("WALLPAPER", had ? was : ""), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
}

/*
 * The system's objects found from the root システム by their links'
 * relationships: the fonts' box, as the root links it; a root made
 * before the links had relationships given them at the start
 * (knl_sys_mark), and an object the root no longer links found by the
 * identity it was made with.
 */
LOCAL void test_sys_find( void )
{
	TS_UUID	root, fonts, got;
	UB	*rec0, *cut;
	SZ	size = 0;
	INT	i, k = 0, m;
	static CONST char tag[] = " relationship=\"system.fonts\"";

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &root), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_FONT_BOX, &fonts), E_OK);
	KT_ASSERT(knl_sys_find("system.fonts", NULL, &got) && ts_uuid_cmp(&got, &fonts) == 0);
	KT_ASSERT(knl_sys_find("system.nosuch", SYSDEF_FONT_BOX, &got) && ts_uuid_cmp(&got, &fonts) == 0);
	KT_ASSERT(!knl_sys_find("system.nosuch", NULL, &got));

	/* the relationship taken off, as a root made before has it: given back at the start */
	rec0 = om_obj_record(&root, 0, &size);
	KT_ASSERT(rec0 != NULL);
	if ( rec0 == NULL ) return;
	cut = (UB *)Kmalloc(size + 1);
	KT_ASSERT(cut != NULL);
	if ( cut == NULL ) {
		Kfree(rec0);
		return;
	}
	m = (INT)knl_strlen(tag);
	for ( i = 0; i < (INT)size; ) {
		INT	j;

		for ( j = 0; j < m && i + j < (INT)size && rec0[i + j] == (UB)tag[j]; j++ ) ;
		if ( j == m ) {
			i += m;
			continue;
		}
		cut[k++] = rec0[i++];
	}
	KT_ASSERT(k < (INT)size);
	KT_ASSERT_ER(om_obj_record_put(&root, 0, OB_RT_TAD, 0, cut, (SZ)k), E_OK);
	KT_ASSERT(!knl_conf_link_get(SYSDEF_SYSTEM_BOX, "system.fonts", &got));
	KT_ASSERT(knl_sys_find("system.fonts", SYSDEF_FONT_BOX, &got) && ts_uuid_cmp(&got, &fonts) == 0);
	knl_sys_mark();
	KT_ASSERT(knl_conf_link_get(SYSDEF_SYSTEM_BOX, "system.fonts", &got) && ts_uuid_cmp(&got, &fonts) == 0);

	/* as it was */
	KT_ASSERT_ER(om_obj_record_put(&root, 0, OB_RT_TAD, 0, rec0, size), E_OK);
	Kfree(cut);
	Kfree(rec0);
}

/* text with every 'cut' taken out, into out: its length */
LOCAL INT cut_out( CONST UB *in, INT len, CONST char *cut, UB *out )
{
	INT	i, k = 0, m = (INT)knl_strlen(cut);

	for ( i = 0; i < len; ) {
		INT	j;

		for ( j = 0; j < m && i + j < len && in[i + j] == (UB)cut[j]; j++ ) ;
		if ( j == m ) {
			i += m;
			continue;
		}
		out[k++] = in[i++];
	}
	return k;
}

/*
 * What is emptied at each start: tmp, in the list of 機器の設定 (links
 * with CF_ROLE_CLEAR in record 0), has its links taken off as its
 * volume is attached; a settings object made before there was a list
 * (no CLEAR_LIST line) is given tmp in it once, and a list without tmp
 * that says it was made is left so.
 */
LOCAL void test_clear_start( void )
{
	TS_UUID	conf, tmp, fonts, got;
	UB	*rec0, *rec1, *cut0, *cut1;
	SZ	s0 = 0, s1 = 0;
	char	path[FS_PATH_MAX];
	INT	n = -1, k0, k1;
	static CONST char role[] = " relationship=\"" CF_ROLE_CLEAR "\"";
	static CONST char line[] = "CLEAR_LIST\t1\n";

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CONF_DEV, &conf), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_TMP, &tmp), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_FONT_BOX, &fonts), E_OK);
	KT_ASSERT(knl_conf_link_get(SYSDEF_CONF_DEV, CF_ROLE_CLEAR, &got) && ts_uuid_cmp(&got, &tmp) == 0);
	KT_ASSERT_ER(knl_obfile_path_of(&tmp, (UB *)path, sizeof(path)), E_OK);

	/* something put in tmp, gone when the volume is attached */
	KT_ASSERT_ER(om_obj_link_add(&tmp, &fonts), E_OK);
	KT_ASSERT(ob_lst_lnk(&tmp, NULL, NULL, 0, &n) >= E_OK && n == 1);
	knl_sys_clear_start(path);
	KT_ASSERT(ob_lst_lnk(&tmp, NULL, NULL, 0, &n) >= E_OK && n == 0);

	rec0 = om_obj_record(&conf, 0, &s0);
	rec1 = om_obj_record(&conf, 1, &s1);
	cut0 = (UB *)Kmalloc(s0 + 1);
	cut1 = (UB *)Kmalloc(s1 + 1);
	KT_ASSERT(rec0 != NULL && rec1 != NULL && cut0 != NULL && cut1 != NULL);
	if ( rec0 == NULL || rec1 == NULL || cut0 == NULL || cut1 == NULL ) goto out;
	k0 = cut_out(rec0, (INT)s0, role, cut0);
	k1 = cut_out(rec1, (INT)s1, line, cut1);
	KT_ASSERT(k0 < (INT)s0 && k1 < (INT)s1);

	/* as R1.110 has it: no list, no line; tmp put in the list, and the line written */
	KT_ASSERT_ER(om_obj_record_put(&conf, 0, OB_RT_TAD, 0, cut0, (SZ)k0), E_OK);
	KT_ASSERT_ER(om_obj_record_put(&conf, 1, OB_RT_SYSDATA, 0, cut1, (SZ)k1), E_OK);
	KT_ASSERT(!knl_conf_link_get(SYSDEF_CONF_DEV, CF_ROLE_CLEAR, &got));
	KT_ASSERT_ER(om_obj_link_add(&tmp, &fonts), E_OK);
	knl_sys_clear_start(path);
	KT_ASSERT(knl_conf_link_get(SYSDEF_CONF_DEV, CF_ROLE_CLEAR, &got) && ts_uuid_cmp(&got, &tmp) == 0);
	KT_ASSERT(ob_lst_lnk(&tmp, NULL, NULL, 0, &n) >= E_OK && n == 0);
	{
		char	v[8];

		KT_ASSERT(wm_dev_conf_get(CF_CLEAR_LIST, v, sizeof(v)) && v[0] == '1');
	}

	/* the list made, and tmp taken out of it: left so, and tmp kept */
	KT_ASSERT_ER(om_obj_record_put(&conf, 0, OB_RT_TAD, 0, cut0, (SZ)k0), E_OK);
	KT_ASSERT_ER(om_obj_record_put(&conf, 1, OB_RT_SYSDATA, 0, rec1, s1), E_OK);
	KT_ASSERT_ER(om_obj_link_add(&tmp, &fonts), E_OK);
	knl_sys_clear_start(path);
	KT_ASSERT(!knl_conf_link_get(SYSDEF_CONF_DEV, CF_ROLE_CLEAR, &got));
	KT_ASSERT(ob_lst_lnk(&tmp, NULL, NULL, 0, &n) >= E_OK && n == 1);
	KT_ASSERT_ER(om_obj_links_set(&tmp, NULL, 0), E_OK);

	/* as it was */
	KT_ASSERT_ER(om_obj_record_put(&conf, 0, OB_RT_TAD, 0, rec0, s0), E_OK);
	KT_ASSERT_ER(om_obj_record_put(&conf, 1, OB_RT_SYSDATA, 0, rec1, s1), E_OK);
out:
	if ( rec0 != NULL ) Kfree(rec0);
	if ( rec1 != NULL ) Kfree(rec1);
	if ( cut0 != NULL ) Kfree(cut0);
	if ( cut1 != NULL ) Kfree(cut1);
}

/* ---------------------------------------------------------------- coming and going */

LOCAL BOOL wait_gen( UW gen, INT ms )
{
	T_USBSTAT	st;
	INT		i;

	for ( i = 0; i < ms / 50; i++ ) {
		if ( ts_usb_stat(&st) >= E_OK && st.gen != gen ) return TRUE;
		tk_dly_tsk(50);
	}
	return FALSE;
}

/* The keyboard on the second controller's port 2 (QEMU's "hpk"), and its keyboard object */
LOCAL BOOL kbd2( TS_UUID *pu, TS_UUID *ph )
{
	T_USBDEV	*d = (T_USBDEV *)Kmalloc(sizeof(T_USBDEV) * 32);
	T_HIDINFO	h[16];
	INT		n, i, k, nh;
	BOOL		found = FALSE;

	if ( d == NULL ) return FALSE;
	n = ts_usb_lst_dev(d, 32);
	nh = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 32 && !found; i++ ) {
		if ( d[i].hc != 1 || d[i].root_port != 1 || d[i].depth != 0 ) continue;
		for ( k = 0; k < nh && k < 16 && !found; k++ ) {
			if ( h[k].usbdev == d[i].dev && h[k].keyboard ) {
				found = ( knl_obusb_uuid(d[i].dev, pu) >= E_OK
				       && knl_obinput_uuid(h[k].id, ph) >= E_OK );
			}
		}
	}
	Kfree(d);
	return found;
}

LOCAL void test_hotplug( void )
{
	T_USBSTAT	st;
	T_OBREF		r;
	TS_UUID		u, h, u2, h2;
	ID		kl, ki;

	if ( !kbd2(&u, &h) ) KT_SKIP("no keyboard on the second controller");
	KT_ASSERT_ER(port_make(), E_OK);
	kl = watch(&ob_uuid_devlist, OB_E_ATTACH | OB_E_DETACH);
	ki = watch(&ob_uuid_input, OB_E_CHANGE);
	KT_ASSERT(kl > 0 && ki > 0);
	port_drain();

	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"hpk\"}}\n");
	if ( !wait_gen(st.gen, 5000) ) {
		if ( kl > 0 ) (void)ob_cls_obj(kl);
		if ( ki > 0 ) (void)ob_cls_obj(ki);
		port_gone();
		KT_SKIP("no QEMU monitor to take a device out with");
	}
	KT_ASSERT(told(&u, OB_E_DETACH, -2, 3000) >= 1);
	KT_ASSERT(told(&h, OB_E_DETACH, -2, 3000) >= 1);
	KT_ASSERT(told(&ob_uuid_input, OB_E_CHANGE, OB_IN_STATE, 3000) >= 1);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_NOEXS);
	KT_ASSERT_ER(ob_ref_obj(&h, &r), E_NOEXS);
	KT_ASSERT(!in_list(&u));

	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":"
		  "{\"driver\":\"usb-kbd\",\"bus\":\"xhci2.0\",\"port\":\"2\",\"id\":\"hpk\"}}\n");
	KT_ASSERT(wait_gen(st.gen, 5000));
	/* back where it was: the same objects */
	KT_ASSERT(told(&u, OB_E_ATTACH, -2, 3000) >= 1);
	KT_ASSERT(told(&h, OB_E_ATTACH, -2, 3000) >= 1);
	KT_ASSERT(kbd2(&u2, &h2));
	KT_ASSERT_EQ(ts_uuid_cmp(&u, &u2), 0);
	KT_ASSERT_EQ(ts_uuid_cmp(&h, &h2), 0);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT(in_list(&h));

	if ( kl > 0 ) (void)ob_cls_obj(kl);
	if ( ki > 0 ) (void)ob_cls_obj(ki);
	port_gone();
}

/* ---------------------------------------------------------------- as a program */

LOCAL void test_process( void )
{
	T_FSTAT	st;

	if ( fs_stat(SVCPROG, &st) < EX_OK ) KT_SKIP("no " SVCPROG);
	/* the administrator's: what needs x is given too */
	KT_ASSERT_EQ(prog_run(4, 0, NULL), 0);
	if ( !user_made() ) KT_SKIP("no store for a user object");
	/* a user's: the rest refused */
	KT_ASSERT_EQ(prog_run(3, 0, &user), 0);
	(void)ob_del_obj(&user);
	have_user = FALSE;
}

/* ---------------------------------------------------------------- the cost */

/*
 * The object path against the kernel function it took the place of,
 * each with a key opened once and kept, as the paths that use them do
 */
LOCAL void test_measure( void )
{
	T_HIDEV		e;
	T_DISPSPEC	spec;
	T_USBDEV	*d;
	UB		*b;
	SZ		asz;
	INT		x = 0, y = 0, i;
	UINT		btn = 0;
	UD		t0, t1;
	ID		k;

	b = (UB *)Kmalloc(4096);
	if ( b == NULL ) KT_SKIP("no memory");

	/* putting in a move to where the pointer is: nothing else changes */
	if ( ts_hid_pointer(&x, &y, &btn) >= E_OK ) {
		knl_memset(&e, 0, sizeof(e));
		e.type = HID_EV_MOVE;
		e.x = x;
		e.y = y;
		t0 = mono_ns();
		for ( i = 0; i < N_CALLS; i++ ) (void)knl_hid_inject(0, &e);
		t1 = mono_ns();
		tm_printf((UB *)"  measure inject direct %ld ns\n", ( t1 - t0 ) / N_CALLS);
		k = ob_opn_obj(&ob_uuid_input, OB_OP_WRITE | OB_OP_EXEC);
		KT_ASSERT(k > 0);
		t0 = mono_ns();
		for ( i = 0; i < N_CALLS; i++ ) (void)ob_wri_rec(k, OB_IN_EVENTS, 0, &e, sizeof(e), &asz);
		t1 = mono_ns();
		tm_printf((UB *)"  measure inject object %ld ns\n", ( t1 - t0 ) / N_CALLS);
		if ( k > 0 ) (void)ob_cls_obj(k);

		t0 = mono_ns();
		for ( i = 0; i < N_CALLS; i++ ) (void)ts_hid_pointer(&x, &y, &btn);
		t1 = mono_ns();
		tm_printf((UB *)"  measure pointer direct %ld ns\n", ( t1 - t0 ) / N_CALLS);
		k = ob_opn_obj(&ob_uuid_input, OB_OP_READ);
		t0 = mono_ns();
		for ( i = 0; i < N_CALLS; i++ ) (void)ob_rea_rec(k, OB_IN_STATE, 0, b, 4096, &asz);
		t1 = mono_ns();
		tm_printf((UB *)"  measure pointer object %ld ns\n", ( t1 - t0 ) / N_CALLS);
		if ( k > 0 ) (void)ob_cls_obj(k);
	}
	if ( ts_disp_ref(&spec) >= E_OK ) {
		t0 = mono_ns();
		for ( i = 0; i < N_CALLS; i++ ) (void)ts_disp_ref(&spec);
		t1 = mono_ns();
		tm_printf((UB *)"  measure display direct %ld ns\n", ( t1 - t0 ) / N_CALLS);
		k = ob_opn_obj(&ob_uuid_display, OB_OP_READ);
		t0 = mono_ns();
		for ( i = 0; i < N_CALLS; i++ ) (void)ob_rea_rec(k, OB_DSP_MODE, 0, b, 4096, &asz);
		t1 = mono_ns();
		tm_printf((UB *)"  measure display object %ld ns\n", ( t1 - t0 ) / N_CALLS);
		if ( k > 0 ) (void)ob_cls_obj(k);
	}
	d = (T_USBDEV *)Kmalloc(sizeof(T_USBDEV) * 16);
	if ( d != NULL ) {
		t0 = mono_ns();
		for ( i = 0; i < N_CALLS / 10; i++ ) (void)ts_usb_lst_dev(d, 16);
		t1 = mono_ns();
		tm_printf((UB *)"  measure usb list direct %ld ns\n", ( t1 - t0 ) / ( N_CALLS / 10 ));
		Kfree(d);
		k = ob_opn_obj(&ob_uuid_system, OB_OP_READ);
		t0 = mono_ns();
		for ( i = 0; i < N_CALLS / 10; i++ ) (void)ob_rea_rec(k, OB_SYS_USB, 0, b, 4096, &asz);
		t1 = mono_ns();
		tm_printf((UB *)"  measure usb list object %ld ns\n", ( t1 - t0 ) / ( N_CALLS / 10 ));
		if ( k > 0 ) (void)ob_cls_obj(k);
	}
	t0 = mono_ns();
	for ( i = 0; i < N_CALLS; i++ ) (void)ts_get_random(b, 32);
	t1 = mono_ns();
	tm_printf((UB *)"  measure random32 direct %ld ns\n", ( t1 - t0 ) / N_CALLS);
	k = ob_opn_obj(&ob_uuid_random, OB_OP_READ);
	t0 = mono_ns();
	for ( i = 0; i < N_CALLS; i++ ) (void)ob_rea_rec(k, OB_RND_DATA, 0, b, 32, &asz);
	t1 = mono_ns();
	tm_printf((UB *)"  measure random32 object %ld ns\n", ( t1 - t0 ) / N_CALLS);
	t0 = mono_ns();
	for ( i = 0; i < N_CALLS / 10; i++ ) (void)ts_get_random(b, 4096);
	t1 = mono_ns();
	tm_printf((UB *)"  measure random4k direct %ld ns\n", ( t1 - t0 ) / ( N_CALLS / 10 ));
	t0 = mono_ns();
	for ( i = 0; i < N_CALLS / 10; i++ ) (void)ob_rea_rec(k, OB_RND_DATA, 0, b, 4096, &asz);
	t1 = mono_ns();
	tm_printf((UB *)"  measure random4k object %ld ns\n", ( t1 - t0 ) / ( N_CALLS / 10 ));
	if ( k > 0 ) (void)ob_cls_obj(k);
	Kfree(b);

	/* a program: the library's kept key, and the old call the kernel keeps */
	tm_printf((UB *)"  measure random32 process object %d ns\n", prog_run(1, 32, NULL));
	tm_printf((UB *)"  measure random4k process object %d ns\n", prog_run(1, 4096, NULL));
	tm_printf((UB *)"  measure random32 process oldsvc %d ns\n", prog_run(2, 32, NULL));
	tm_printf((UB *)"  measure random4k process oldsvc %d ns\n", prog_run(2, 4096, NULL));
}

/* ---------------------------------------------------------------- lists of any length */

#define KT_NBSS		100

/* A scan's record as the Wi-Fi makes it, KT_NBSS networks long: each line about 90 bytes */
LOCAL INT bss_make( void *ctx, UB *t, INT max )
{
	INT	i, n = 0, k;

	for ( i = 0; i < *(INT *)ctx; i++ ) {
		n = knl_oj_put(t, n, max, "BSS 02:00:00:00:00:");
		n = knl_oj_put_num(t, n, max, 10 + i % 90);
		n = knl_oj_put(t, n, max, " 2412 -");
		n = knl_oj_put_num(t, n, max, 30 + i % 60);
		n = knl_oj_put(t, n, max, " wpa2 ");
		for ( k = 0; k < 24; k++ ) n = knl_oj_put(t, n, max, "41");	/* "AAAA..." in hexadecimal */
		n = knl_oj_put_num(t, n, max, 1000 + i);
		n = knl_oj_put(t, n, max, "\n");
	}
	return n;
}

/*
 * A text longer than any first buffer is made whole: the maker is asked
 * again with more room until it fits, and the last of a hundred
 * networks is there, as a reader finds it line by line
 */
LOCAL void test_text_grow( void )
{
	UB		*t = NULL;
	CONST UB	*v, *w;
	INT		n = KT_NBSS, len, vl, wl, k;
	char		last[16];

	len = knl_oj_make(bss_make, &n, &t);
	tm_printf((UB *)"  %d networks made %d bytes\n", n, len);
	KT_ASSERT(len > 4096);
	KT_ASSERT(t != NULL);
	if ( t == NULL ) return;
	for ( k = 0; cf_find(t, len, "BSS", k, &v, &vl); k++ ) ;
	KT_ASSERT_EQ(k, KT_NBSS);
	KT_ASSERT(cf_find(t, len, "BSS", KT_NBSS - 1, &v, &vl));
	KT_ASSERT(cf_wfind(v, vl, 4, &w, &wl));
	(void)cf_itoa(1000 + KT_NBSS - 1, last);
	KT_ASSERT(wl == 48 + 4 && same_bytes(w + 48, last, 4) == 0);
	KT_ASSERT_EQ((INT)knl_oj_make_len(bss_make, &n), len);
	Kfree(t);
}

/*
 * The processors' roles as the settings write them, for a machine of 64
 * processors and of as many as the system runs: every other one is a
 * list far longer than a value of CF_VAL_MAX, and it comes back the same
 */
LOCAL void prcset_round( INT k )
{
	T_PRCSET	s, b;
	UB		*t = (UB *)Kmalloc(8 * 1024);
	INT		i, n;

	KT_ASSERT(t != NULL);
	if ( t == NULL ) return;
	prcset_zero(&s);
	for ( i = 0; i < k; i += 2 ) prcset_add(&s, i);
	n = knl_prcset_text(t, 0, 8 * 1024, &s, k);
	KT_ASSERT(n > 0);
	tm_printf((UB *)"  %d processors, every other one: %d bytes\n", k, n);
	if ( k >= 128 ) KT_ASSERT(n > CF_VAL_MAX);
	knl_prcset_parse((CONST char *)t, n, k, &b);
	for ( i = 0; i < k; i++ ) KT_ASSERT_EQ(prcset_has(&b, i), prcset_has(&s, i));
	/* a run: "2-k" */
	prcset_zero(&s);
	for ( i = 1; i < k; i++ ) prcset_add(&s, i);
	n = knl_prcset_text(t, 0, 8 * 1024, &s, k);
	knl_prcset_parse((CONST char *)t, n, k, &b);
	for ( i = 0; i < k; i++ ) KT_ASSERT_EQ(prcset_has(&b, i), ( i >= 1 ));
	/* a number past the processors, or too large to be one, is left out */
	knl_prcset_parse("1,99999999999,3-999999", 22, k, &b);
	KT_ASSERT(prcset_has(&b, 0) && !prcset_has(&b, 1) && prcset_has(&b, 2) && prcset_has(&b, k - 1));
	Kfree(t);
}

LOCAL void test_prcset_text( void )
{
	prcset_round(( CNF_MAX_PRCID < 64 ) ? CNF_MAX_PRCID : 64);
	prcset_round(CNF_MAX_PRCID);
}

/*
 * The machine's settings with a list of processors longer than a value
 * of CF_VAL_MAX: the processor at its end is taken all the same
 */
LOCAL void test_roles_long( void )
{
	char		*l;
	INT		i, n = 0;
	T_PRCSET	c, r;

	if ( knl_num_prc < 3 ) KT_SKIP("fewer than three processors");
	l = (char *)Kmalloc(1024);
	KT_ASSERT(l != NULL);
	if ( l == NULL ) return;
	for ( i = 0; i < 200; i++ ) {
		l[n++] = '1';
		l[n++] = ',';
	}
	l[n++] = '3';
	l[n] = '\0';
	if ( wm_dev_conf_put("CPU_CHORE", l) < E_OK ) {
		Kfree(l);
		KT_SKIP("no machine settings object");
	}
	KT_ASSERT_ER(wm_cpu_conf_apply(), E_OK);
	knl_cpu_roles_get(&c, &r);
	tm_printf((UB *)"  CPU_CHORE of %d bytes: PRC1 %d, PRC3 %d\n", n, prcset_has(&c, 0), prcset_has(&c, 2));
	KT_ASSERT(prcset_has(&c, 0) && prcset_has(&c, 2));
	KT_ASSERT_ER(wm_dev_conf_put("CPU_CHORE", ""), E_OK);
	KT_ASSERT_ER(wm_cpu_conf_apply(), E_OK);
	knl_cpu_roles_get(&c, &r);
	KT_ASSERT(!prcset_has(&c, 2));
	Kfree(l);
}

/*
 * The system's record 1 read whole however long, as the record's list
 * says it is: every device whose interrupts are known has its line
 */
LOCAL void test_system_whole( void )
{
	T_OBREC	recs[OB_SYS_NREC];
	UB	*t = NULL, (*nm)[L_DEVNM + 1] = NULL;
	char	v[CF_VAL_MAX];
	ID	key;
	INT	len, cnt = 0, k, devs, i;

	key = ob_opn_obj(&ob_uuid_system, OB_OP_R);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	len = knl_ob_rea_all(key, OB_SYS_INFO, 0, &t, NULL);
	KT_ASSERT(len > 0);
	KT_ASSERT_ER(ob_lst_rec(key, recs, OB_SYS_NREC, &cnt), E_OK);
	ob_cls_obj(key);
	if ( len <= 0 ) return;
	KT_ASSERT_EQ((INT)recs[OB_SYS_INFO].size, len);
	/* the devices, asked again in more room until they all fit: the first room holds one */
	for ( k = 1; ; k = devs * 2 ) {
		nm = Kmalloc(sizeof(*nm) * (SZ)k);
		devs = ( nm != NULL ) ? knl_int_dev_names(nm, k) : 0;
		if ( devs <= k ) break;
		Kfree(nm);
	}
	k = lines(t, len, "IRQ\t");
	tm_printf((UB *)"  record 1: %d bytes, %d devices' interrupts, %d lines\n", len, devs, k);
	KT_ASSERT_EQ(k, devs);
	/* each line names one of the devices, as the system has it */
	for ( i = 0; i < k && nm != NULL; i++ ) {
		char	w[L_DEVNM + 2];
		INT	d;

		KT_ASSERT(cf_get(t, len, "IRQ", i, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)));
		for ( d = 0; d < devs && knl_strcmp(w, (CONST char *)nm[d]) != 0; d++ ) ;
		KT_ASSERT(d < devs);
	}
	if ( nm != NULL ) Kfree(nm);
	KT_ASSERT(lines(t, len, "CPU_CHORE\t") == 1 && lines(t, len, "CPU_RT\t") == 1);
	Kfree(t);
}

/* ---------------------------------------------------------------- edges of what comes in */

/*
 * The edges of ts/ob.h (T_OBEDGE): a notice for each time something
 * comes after a read found nothing, never one left untold, wherever what
 * came is said -- by a task, inside a critical section of the kernel's
 * holding one of its locks, or in an interrupt handler -- whether or not
 * anything else comes after it.
 */
#define EDGE_WAIT	2000		/* ms a notice may take, on a slow emulated machine */

/* The console's log read to its end: the edge armed */
LOCAL void log_drain( UD *p_pos, UB *buf, INT max, BOOL *p_two )
{
	INT	n, i;

	while ( ( n = tm_log_read(p_pos, buf, max) ) > 0 ) {
		for ( i = 0; p_two != NULL && i + 9 <= n; i++ ) {
			INT	j;

			for ( j = 0; j < 9 && buf[i + j] == (UB)"edge: two"[j]; j++ ) ;
			if ( j == 9 ) *p_two = TRUE;
		}
	}
}

/* what is written to the console after its log was read to its end is told, once until it is read again */
LOCAL void test_edge_log( void )
{
	UB	*buf;
	UD	pos = 0;
	BOOL	two = FALSE;
	ID	k;

	if ( port_make() < E_OK ) KT_SKIP("no channel");
	buf = (UB *)Kmalloc(512);
	k = watch(&ob_uuid_system, OB_E_CHANGE);
	KT_ASSERT(k > 0 && buf != NULL);
	if ( k <= 0 || buf == NULL ) {
		if ( buf != NULL ) Kfree(buf);
		port_gone();
		return;
	}
	log_drain(&pos, buf, 512, NULL);
	port_drain();

	tm_putstring((UB *)"  edge: one\n");
	KT_ASSERT(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_LOG, EDGE_WAIT) >= 1);

	/* told and not read yet: what comes meanwhile is not told again, and is read with the rest */
	tm_putstring((UB *)"  edge: two\n");
	KT_ASSERT_EQ(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_LOG, 300), 0);
	log_drain(&pos, buf, 512, &two);
	KT_ASSERT(two);

	/* read to its end again: what comes next is told */
	port_drain();
	tm_putstring((UB *)"  edge: three\n");
	KT_ASSERT(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_LOG, EDGE_WAIT) >= 1);
	log_drain(&pos, buf, 512, NULL);

	Kfree(buf);
	(void)ob_cls_obj(k);
	port_gone();
}

/*
 * Written inside a critical section of the kernel's, holding one of its
 * finer locks where the kernel has them (the wake-up owed, and given as
 * the last lock is let go), and nothing written after it: still told
 */
LOCAL void test_edge_locked( void )
{
	UB	*buf;
	UD	pos = 0;
	ID	k;

	if ( port_make() < E_OK ) KT_SKIP("no channel");
	buf = (UB *)Kmalloc(512);
	k = watch(&ob_uuid_system, OB_E_CHANGE);
	KT_ASSERT(k > 0 && buf != NULL);
	if ( k <= 0 || buf == NULL ) {
		if ( buf != NULL ) Kfree(buf);
		port_gone();
		return;
	}
	log_drain(&pos, buf, 512, NULL);
	port_drain();
	BEGIN_CRITICAL_SECTION;
	KLK_MISC();
	tm_putstring((UB *)"  edge: written in a critical section\n");
	KLK_UNMISC();
	END_CRITICAL_SECTION;
	KT_ASSERT(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_LOG, EDGE_WAIT) >= 1);
	log_drain(&pos, buf, 512, NULL);
	Kfree(buf);
	(void)ob_cls_obj(k);
	port_gone();
}

/*
 * Said to have come in an interrupt handler -- holding one of the
 * kernel's finer locks there where the kernel has them, so that the
 * wake-up is given as the handler lets go of its last lock -- and told
 */
LOCAL T_OBEDGE	edge_kt;
LOCAL BOOL	edge_kt_added = FALSE;
LOCAL volatile INT edge_kt_ran = 0;

LOCAL void edge_handler( void *exinf )
{
#if KLK_FINE
	UINT	_intsts_ = disint();

	KLK_MISC_DI();
	if ( knl_obedge_came(&edge_kt) ) knl_ob_edge_kick(&edge_kt);
	KLK_UNMISC();
	enaint(_intsts_);
#else
	if ( knl_obedge_came(&edge_kt) ) knl_ob_edge_kick(&edge_kt);
#endif
	(void)exinf;
	edge_kt_ran++;
}

LOCAL void test_edge_interrupt( void )
{
	T_CALM	ca;
	ID	k, alm;

	if ( !edge_kt_added ) {
		KT_ASSERT_ER(knl_ob_edge_add(&edge_kt, &ob_uuid_system, NULL, OB_SYS_HW, NULL), E_OK);
		edge_kt_added = TRUE;
	}
	if ( port_make() < E_OK ) KT_SKIP("no channel");
	k = watch(&ob_uuid_system, OB_E_CHANGE);
	KT_ASSERT(k > 0);
	if ( k <= 0 ) {
		port_gone();
		return;
	}
	port_drain();
	knl_obedge_arm(&edge_kt);
	ca.exinf = NULL;
	ca.almatr = TA_HLNG;
	ca.almhdr = (FP)edge_handler;
	alm = tk_cre_alm(&ca);
	KT_ASSERT(alm > 0);
	if ( alm > 0 ) {
		KT_ASSERT_ER(tk_sta_alm(alm, 10), E_OK);
		KT_ASSERT(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_HW, EDGE_WAIT) >= 1);
		KT_ASSERT_EQ(edge_kt_ran, 1);
		/* not armed again: a second time in the handler tells nothing */
		edge_kt_ran = 0;
		KT_ASSERT_ER(tk_sta_alm(alm, 10), E_OK);
		KT_ASSERT_EQ(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_HW, 300), 0);
		KT_ASSERT_EQ(edge_kt_ran, 1);
		(void)tk_del_alm(alm);
	}
	(void)ob_cls_obj(k);
	port_gone();
}

/*
 * What plays sound told as it changes (OB_SYS_SOUND), once the record
 * has been read: another output chosen, and "auto" again
 */
LOCAL BOOL sound_read( void )
{
	UB	*t = NULL;
	ID	key = ob_opn_obj(&ob_uuid_system, OB_OP_R);
	INT	len;

	if ( key <= 0 ) return FALSE;
	len = knl_ob_rea_all(key, OB_SYS_SOUND, 0, &t, NULL);
	ob_cls_obj(key);
	if ( t != NULL ) Kfree(t);
	return (BOOL)( len > 0 );
}

LOCAL void test_edge_sound( void )
{
	SDOut	o[8];
	INT	n, k;
	ID	key;

	n = knl_snd_outputs(o, 8);
	for ( k = 1; k < n && k < 8 && o[k].kind == SND_BE_NONE; k++ ) ;
	if ( k >= n || k >= 8 ) KT_SKIP("no output to choose");
	if ( port_make() < E_OK ) KT_SKIP("no channel");
	key = watch(&ob_uuid_system, OB_E_CHANGE);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) {
		port_gone();
		return;
	}
	KT_ASSERT(sound_read());
	port_drain();
	KT_ASSERT_ER(knl_snd_select(o[k].id), E_OK);
	KT_ASSERT(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_SOUND, 3000) >= 1);
	tm_printf((UB *)"  %s chosen: told\n", (CONST char *)o[k].id);
	KT_ASSERT(sound_read());
	port_drain();
	KT_ASSERT_ER(knl_snd_select((CONST UB *)SND_ID_AUTO), E_OK);
	KT_ASSERT(told(&ob_uuid_system, OB_E_CHANGE, OB_SYS_SOUND, 3000) >= 1);
	(void)ob_cls_obj(key);
	port_gone();
}

EXPORT void ktest_devob( void )
{
	KT_RUN(test_random);
	KT_RUN(test_display);
	KT_RUN(test_input);
	KT_RUN(test_hid_devices);
	KT_RUN(test_usb_devices);
	KT_RUN(test_hw_state);
	KT_RUN(test_cpu_limit);
	KT_RUN(test_wall_link);
	KT_RUN(test_sys_find);
	KT_RUN(test_clear_start);
	KT_RUN(test_text_grow);
	KT_RUN(test_prcset_text);
	KT_RUN(test_roles_long);
	KT_RUN(test_system_whole);
	KT_RUN_QEMU_DEVS(test_hotplug, "devices are taken out through the QEMU monitor");
	KT_RUN(test_process);
	KT_RUN(test_measure);
	KT_RUN(test_edge_log);
	KT_RUN(test_edge_locked);
	KT_RUN(test_edge_interrupt);
	KT_RUN(test_edge_sound);
}
