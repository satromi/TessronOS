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
#include <tk/tkernel.h>

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
	T_OBPRT	prt;
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
	KT_ASSERT_ER(ob_get_prt(&ob_uuid_random, &prt), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&prt.owner, &ob_user_system), 0);
	KT_ASSERT_EQ(prt.mode, 0644);
}

/* ---------------------------------------------------------------- the screen */

LOCAL void test_display( void )
{
	T_DISPSPEC	s, s2;
	T_OBREF		r;
	T_OBPRT		prt;
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
	KT_ASSERT(has(txt, n, "MODES\t") && has(txt, n, "DRIVER\tbochs-display"));
	n = atr(&ob_uuid_display);
	tm_sprintf((UB *)want, (UB *)"\"width\":%d", (INT)s.width);
	KT_ASSERT(n > 0 && has(txt, n, want) && has(txt, n, "\"modes\":[\"640x480\""));

	/* the system object's record 3 is a view of it */
	n = rec(&ob_uuid_system, OB_OP_R, OB_SYS_DISPLAY);
	(void)ts_uuid_to_str(&ob_uuid_display, us, sizeof(us));
	KT_ASSERT(n > 0 && has(txt, n, us) && has(txt, n, "SIZE\t"));

	/* the pixels: only with x */
	KT_ASSERT_ER(ob_get_prt(&ob_uuid_display, &prt), E_OK);
	KT_ASSERT_EQ(prt.mode, 0664);
	KT_ASSERT(prt.nrmask == 1 && prt.rmask[0].recno == OB_DSP_PIXELS
		  && ( prt.rmask[0].ops & OB_OP_EXEC ) != 0);
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
	KT_ASSERT_ER(ob_wri_rec(k, OB_DSP_MODE, 0, "800x600", 7, &asz), E_OK);
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
	T_OBPRT		prt;
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
	KT_ASSERT_ER(ob_get_prt(&ob_uuid_input, &prt), E_OK);
	KT_ASSERT(prt.nrmask == 1 && prt.rmask[0].recno == OB_IN_EVENTS
		  && ( prt.rmask[0].ops & OB_OP_EXEC ) != 0);
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
#ifndef RPI5
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
#ifdef RPI5
	KT_ASSERT(has(txt, len, "ARM_HZ\t"));
#else
	/* QEMU: nothing to ask, and said so */
	KT_ASSERT(has(txt, len, "TEMPERATURE\tunknown\n") && has(txt, len, "ARM_HZ\tunknown\n")
		  && has(txt, len, "V3D_HZ\tunknown\n"));
#endif
	KT_ASSERT(rec(&ob_uuid_system, OB_OP_R, 0) > 0 && has(txt, TEXT_MAX, "Records 1 to 9"));
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

EXPORT void ktest_devob( void )
{
	KT_RUN(test_random);
	KT_RUN(test_display);
	KT_RUN(test_input);
	KT_RUN(test_hid_devices);
	KT_RUN(test_usb_devices);
	KT_RUN(test_hw_state);
	KT_RUN_EXCEPT_RPI5(test_hotplug, "devices are taken out through the QEMU monitor");
	KT_RUN(test_process);
	KT_RUN(test_measure);
}
