/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obinput.c
 *	The keyboards and pointers as objects (design 18.7)
 *
 *	Each keyboard or pointer interface on the USB bus is a device
 *	object while it is plugged in, named after the port it is on
 *	("hid0-4.3", with ".i1" and so on for another interface of the same
 *	device), and kept in the device box under that name. Beside them is
 *	入力 (ob_uuid_input), the input they all feed, which is there without
 *	any of them: what is put in without a device goes there.
 *
 *	Record 1 is the events. Read, it gives those that came since the
 *	key last read, copied (device/usb/hid.c knl_hid_tap): the window
 *	manager, which takes the events and hands them to the windows, still
 *	gets every one. Written, T_HIDEV are put in as if the device had
 *	reported them; this is the way in for the tests and the desktop's
 *	own trials (application/desktop/dtuitest.c). Both need a key that
 *	may control the device (x): the record's own limit says so, and by
 *	default only an administrator's key has it. Record 2 is "key value"
 *	text of what the device is and what it has done. 入力 has record 3,
 *	T_HIDATTR, how the input is taken (ユーザ環境設定), which the window
 *	manager writes.
 *
 *	The window manager keeps taking the events with ts_hid_read, and the
 *	desktop keeps asking where the pointer is with ts_hid_pointer: that
 *	is done on every movement, and through an object it would cost the
 *	name manager's lookups each time (measured in ktest_devob).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/hid.h>
#include <ts/usb.h>
#include "obj.h"

IMPORT INT knl_obusb_path( CONST T_USBDEV *d, UB *out, INT max );	/* obusb.c: "0-4.3" */

#define IN_MAX		16		/* interfaces shown at once */
#define IN_TEXT_MAX	1024
#define IN_EV_MAX	64		/* events copied in one call */

typedef struct {
	BOOL		used;
	T_HIDINFO	info;
	UB		name[OB_DV_NAME];
	TS_UUID		uuid;
	UH		vendor, product;
	UB		port[24];		/* "0-4.3" */
} INDEV;

LOCAL INDEV	in_tbl[IN_MAX];
LOCAL ID	in_mtx = 0;
LOCAL UINT	in_nkbd = 0, in_nptr = 0;	/* as last told on 入力 */

#define LOCK()		tk_loc_mtx(in_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(in_mtx)

/* ---------------------------------------------------------------- text */

LOCAL INT put_line( UB *t, INT n, INT max, CONST char *key, CONST char *val )
{
	n = knl_oj_put(t, n, max, key);
	n = knl_oj_put(t, n, max, "\t");
	n = knl_oj_put(t, n, max, val);
	return knl_oj_put(t, n, max, "\n");
}

LOCAL INT put_num( UB *t, INT n, INT max, CONST char *key, D v )
{
	n = knl_oj_put(t, n, max, key);
	n = knl_oj_put(t, n, max, "\t");
	n = knl_oj_put_num(t, n, max, v);
	return knl_oj_put(t, n, max, "\n");
}

LOCAL INT put_hex4( UB *t, INT n, INT max, UINT v )
{
	char	s[5];
	INT	i;

	for ( i = 0; i < 4; i++ ) {
		UINT	d = ( v >> ( ( 3 - i ) * 4 ) ) & 0xF;

		s[i] = (char)( d < 10 ? '0' + d : 'a' + d - 10 );
	}
	s[4] = 0;
	return knl_oj_put(t, n, max, s);
}

/* A copy of an entry, while it is there */
LOCAL BOOL entry_of( INDEV *ctx, INDEV *out )
{
	BOOL	ok;

	LOCK();
	ok = ctx->used;
	if ( ok ) *out = *ctx;
	UNLOCK();
	return ok;
}

/* Record 2 of a keyboard or pointer */
LOCAL INT dev_state( INDEV *d, UB *t, INT n, INT max )
{
	n = put_line(t, n, max, "KIND", d->info.keyboard ? "keyboard" : "pointer");
	if ( !d->info.keyboard ) {
		n = put_line(t, n, max, "POINTING", d->info.absolute ? "absolute" : "relative");
		n = put_num(t, n, max, "BUTTONS", d->info.buttons);
		n = put_line(t, n, max, "WHEEL", d->info.wheel ? "yes" : "no");
	}
	n = knl_oj_put(t, n, max, "VENDOR\t");
	n = put_hex4(t, n, max, d->vendor);
	n = knl_oj_put(t, n, max, "\nPRODUCT\t");
	n = put_hex4(t, n, max, d->product);
	n = knl_oj_put(t, n, max, "\n");
	n = put_line(t, n, max, "PORT", (CONST char *)d->port);
	n = put_num(t, n, max, "INTERFACE", d->info.ifno);
	return put_num(t, n, max, "REPORTS", (D)d->info.reports);
}

/* Record 2 of 入力: where the pointer is and what came */
LOCAL INT all_state( UB *t, INT n, INT max )
{
	T_HIDSTAT	st;
	INT		x = 0, y = 0;
	UINT		b = 0;

	if ( ts_hid_pointer(&x, &y, &b) >= E_OK ) {
		n = put_num(t, n, max, "X", x);
		n = put_num(t, n, max, "Y", y);
		n = put_num(t, n, max, "BUTTONS", b);
	} else {
		n = put_line(t, n, max, "POINTER", "none");
	}
	if ( ts_hid_stat(&st) >= E_OK ) {
		n = put_num(t, n, max, "KEYBOARDS", st.keyboards);
		n = put_num(t, n, max, "POINTERS", st.pointers);
		n = put_num(t, n, max, "EVENTS", (D)st.events);
		n = put_num(t, n, max, "DROPPED", (D)st.dropped);
	}
	return n;
}

LOCAL INT state_text( INDEV *ctx, UB *t, INT max )
{
	INDEV	d;

	if ( ctx == NULL ) {
		return all_state(t, 0, max);
	}
	return entry_of(ctx, &d) ? dev_state(&d, t, 0, max) : -1;
}

/* ---------------------------------------------------------------- the records */

LOCAL CONST char *in_about =
	"<p>Record 1: the events (T_HIDEV), read as a copy of what came since the key last "
	"read, written to be put in as if the device had reported them; both with a key "
	"that may control the device. Record 2: what it is and what it has done, as key "
	"and value lines.";

LOCAL INT in_text( void *ctx, UB *t, INT n, INT max )
{
	INDEV	d;
	TS_UUID	u;
	INT	i;

	n = knl_oj_put(t, n, max, in_about);
	if ( ctx == NULL ) {
		n = knl_oj_put(t, n, max, " Record 3: how the input is taken (T_HIDATTR).</p>"
				  "<p>The keyboards and pointers that feed it:</p>");
		for ( i = 0; i < IN_MAX; i++ ) {
			if ( entry_of(&in_tbl[i], &d) ) {
				n = knl_obdev_link(t, n, max, &d.uuid, d.name);
			}
		}
		return n;
	}
	n = knl_oj_put(t, n, max, "</p>");
	if ( entry_of((INDEV *)ctx, &d) ) {
		if ( knl_obusb_uuid(d.info.usbdev, &u) >= E_OK ) {
			n = knl_obdev_link(t, n, max, &u, (CONST UB *)"the USB device it is on");
		}
	}
	return knl_obdev_link(t, n, max, &ob_uuid_input, (CONST UB *)"入力");
}

LOCAL INT in_attr( void *ctx, UB *j, INT n, INT max )
{
	INDEV	d;

	if ( ctx == NULL ) {
		T_HIDSTAT	st;

		n = knl_oj_put(j, n, max, ",\"virtual\":true");
		if ( ts_hid_stat(&st) >= E_OK ) {
			n = knl_oj_put(j, n, max, ",\"keyboards\":");
			n = knl_oj_put_num(j, n, max, st.keyboards);
			n = knl_oj_put(j, n, max, ",\"pointers\":");
			n = knl_oj_put_num(j, n, max, st.pointers);
		}
		return n;
	}
	if ( !entry_of((INDEV *)ctx, &d) ) {
		return n;
	}
	n = knl_oj_put(j, n, max, ",\"virtual\":false,\"vid\":\"");
	n = put_hex4(j, n, max, d.vendor);
	n = knl_oj_put(j, n, max, "\",\"pid\":\"");
	n = put_hex4(j, n, max, d.product);
	n = knl_oj_put(j, n, max, "\",\"port\":");
	n = knl_oj_put_str(j, n, max, d.port);
	n = knl_oj_put(j, n, max, ",\"interface\":");
	n = knl_oj_put_num(j, n, max, d.info.ifno);
	if ( !d.info.keyboard ) {
		n = knl_oj_put(j, n, max, d.info.absolute ? ",\"pointing\":\"absolute\""
							   : ",\"pointing\":\"relative\"");
	}
	return n;
}

LOCAL UINT src_of( void *ctx )
{
	INDEV	d;

	if ( ctx == NULL ) {
		return 0;
	}
	return entry_of((INDEV *)ctx, &d) ? d.info.id : (UINT)-1;
}

LOCAL ER in_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	*t;
	INT	len;
	SZ	n = 0;

	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	if ( recno == OB_IN_EVENTS ) {
		T_HIDEV	*ev = (T_HIDEV *)buf;
		INT	want = (INT)( size / (SZ)sizeof(T_HIDEV) ), got = 0, k;
		UINT	src = src_of(ctx);

		if ( src == (UINT)-1 ) {
			return E_NOEXS;
		}
		while ( got < want ) {
			k = knl_hid_tap(pos, src, ev + got, ( want - got > IN_EV_MAX ) ? IN_EV_MAX : want - got,
					NULL);
			if ( k <= 0 ) break;
			got += k;
		}
		if ( p_asize != NULL ) *p_asize = (SZ)got * (SZ)sizeof(T_HIDEV);
		return E_OK;
	}
	if ( recno == OB_IN_ATTR && ctx == NULL ) {
		T_HIDATTR	a;

		if ( off != 0 || size < (SZ)sizeof(a) ) {
			return E_PAR;			/* read whole */
		}
		(void)ts_hid_getattr(&a);
		knl_memcpy(buf, &a, sizeof(a));
		if ( p_asize != NULL ) *p_asize = sizeof(a);
		return E_OK;
	}
	if ( recno != OB_IN_STATE ) {
		return E_NOEXS;
	}
	t = (UB *)Kmalloc(IN_TEXT_MAX);
	if ( t == NULL ) {
		return E_NOMEM;
	}
	len = state_text((INDEV *)ctx, t, IN_TEXT_MAX);
	if ( len >= 0 && off < len ) {
		n = (SZ)( len - off );
		if ( n > size ) n = size;
		knl_memcpy(buf, t + off, (INT)n);
	}
	Kfree(t);
	if ( len < 0 ) {
		return E_NOEXS;
	}
	if ( p_asize != NULL ) *p_asize = n;
	return E_OK;
}

LOCAL ER in_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	CONST T_HIDEV	*ev = (CONST T_HIDEV *)buf;
	INT		k, cnt;
	UINT		src;
	ER		er;

	(void)pos;
	if ( recno == OB_IN_ATTR && ctx == NULL ) {
		if ( off != 0 || size != (SZ)sizeof(T_HIDATTR) ) {
			return E_PAR;
		}
		er = ts_hid_setattr((CONST T_HIDATTR *)buf);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
		return er;
	}
	if ( recno == OB_IN_STATE ) {
		return E_RONLY;
	}
	if ( recno != OB_IN_EVENTS ) {
		return E_NOEXS;
	}
	if ( size <= 0 || size % (SZ)sizeof(T_HIDEV) != 0 ) {
		return E_PAR;			/* whole events only */
	}
	src = src_of(ctx);
	if ( src == (UINT)-1 ) {
		return E_NOEXS;
	}
	cnt = (INT)( size / (SZ)sizeof(T_HIDEV) );
	for ( k = 0; k < cnt; k++ ) {
		er = knl_hid_inject(src, &ev[k]);
		if ( er < E_OK ) {
			if ( k == 0 ) return er;
			break;
		}
	}
	if ( p_asize != NULL ) *p_asize = (SZ)k * (SZ)sizeof(T_HIDEV);
	return E_OK;
}

LOCAL UD in_size( void *ctx, INT recno )
{
	UB	*t;
	INT	len;

	if ( recno == OB_IN_ATTR ) {
		return sizeof(T_HIDATTR);
	}
	if ( recno != OB_IN_STATE ) {
		return 0;			/* the events are a stream */
	}
	t = (UB *)Kmalloc(IN_TEXT_MAX);
	if ( t == NULL ) {
		return 0;
	}
	len = state_text((INDEV *)ctx, t, IN_TEXT_MAX);
	Kfree(t);
	return ( len > 0 ) ? (UD)len : 0;
}

LOCAL void in_opened( void *ctx, UD *pos )
{
	(void)ctx;
	*pos = knl_hid_tap_now();		/* a key reads what comes after it was opened */
}

/* The input they all share; a keyboard; a pointer: rw-r--r--, the events x only */
LOCAL CONST T_OBDVOPS all_ops = {
	OB_S_INPUT, "input", OB_IN_NREC + 1, 0644, OB_IN_EVENTS, OB_OP_READ | OB_OP_WRITE,
	in_text, in_attr, in_rea, in_wri, in_size, in_opened
};
LOCAL CONST T_OBDVOPS kbd_ops = {
	OB_S_INPUT, "keyboard", OB_IN_NREC, 0644, OB_IN_EVENTS, OB_OP_READ | OB_OP_WRITE,
	in_text, in_attr, in_rea, in_wri, in_size, in_opened
};
LOCAL CONST T_OBDVOPS ptr_ops = {
	OB_S_INPUT, "pointer", OB_IN_NREC, 0644, OB_IN_EVENTS, OB_OP_READ | OB_OP_WRITE,
	in_text, in_attr, in_rea, in_wri, in_size, in_opened
};

/* ---------------------------------------------------------------- what is there */

EXPORT ER knl_obinput_uuid( UINT hid, TS_UUID *p_uuid )
{
	INT	i;
	ER	er = E_NOEXS;

	if ( in_mtx <= 0 ) {
		return E_NOEXS;
	}
	LOCK();
	for ( i = 0; i < IN_MAX; i++ ) {
		if ( in_tbl[i].used && in_tbl[i].info.id == hid ) {
			*p_uuid = in_tbl[i].uuid;
			er = E_OK;
			break;
		}
	}
	UNLOCK();
	return er;
}

/*
 * The name of an interface: the port its device is on, and, past the
 * first, which interface of it -- the same whichever the driver lists
 * first
 */
LOCAL void name_of( CONST T_HIDINFO *h, CONST T_USBDEV *d, UB *name, UB *port )
{
	INT	n = 0;

	(void)knl_obusb_path(d, port, 24);
	n = knl_oj_put(name, 0, OB_DV_NAME, "hid");
	n = knl_oj_put(name, n, OB_DV_NAME, (CONST char *)port);
	if ( h->ifno > 0 ) {
		n = knl_oj_put(name, n, OB_DV_NAME, ".i");
		n = knl_oj_put_num(name, n, OB_DV_NAME, h->ifno);
	}
	if ( n < 0 ) name[OB_DV_NAME - 1] = 0;
}

/*
 * The keyboards and pointers now, as the driver lists them, against
 * those shown: the new ones added, the gone ones taken away, and 入力
 * told when how many there are changed
 */
EXPORT void knl_obinput_sync( void )
{
	T_HIDINFO	*h;
	T_USBDEV	d;
	T_HIDSTAT	st;
	UB		gone[IN_MAX][OB_DV_NAME];
	INT		n, i, k, ng = 0;
	BOOL		changed = FALSE;

	if ( in_mtx <= 0 ) {
		return;
	}
	h = (T_HIDINFO *)Kmalloc(sizeof(T_HIDINFO) * IN_MAX);
	if ( h == NULL ) {
		return;
	}
	n = knl_hid_list(h, IN_MAX);
	if ( n > IN_MAX ) n = IN_MAX;

	/* the gone ones */
	LOCK();
	for ( i = 0; i < IN_MAX; i++ ) {
		if ( !in_tbl[i].used ) continue;
		for ( k = 0; k < n && h[k].id != in_tbl[i].info.id; k++ ) ;
		if ( k < n ) {
			in_tbl[i].info.reports = h[k].reports;
			continue;
		}
		knl_memcpy(gone[ng++], in_tbl[i].name, OB_DV_NAME);
		in_tbl[i].used = FALSE;
	}
	UNLOCK();
	for ( i = 0; i < ng; i++ ) {
		(void)knl_obdev_del(gone[i]);
		changed = TRUE;
	}

	/* the new ones */
	for ( k = 0; k < n; k++ ) {
		INDEV	e;
		INT	slot = -1;
		ER	er;

		LOCK();
		for ( i = 0; i < IN_MAX; i++ ) {
			if ( in_tbl[i].used && in_tbl[i].info.id == h[k].id ) break;
			if ( !in_tbl[i].used && slot < 0 ) slot = i;
		}
		UNLOCK();
		if ( i < IN_MAX || slot < 0 ) continue;		/* there already, or no room */
		knl_memset(&e, 0, sizeof(e));
		e.info = h[k];
		if ( ts_usb_ref_dev(h[k].usbdev, &d) >= E_OK ) {
			e.vendor = d.vendor;
			e.product = d.product;
			name_of(&h[k], &d, e.name, e.port);
		} else {
			(void)knl_oj_put(e.name, 0, OB_DV_NAME, "hid?");
		}
		e.used = TRUE;
		LOCK();
		in_tbl[slot] = e;
		UNLOCK();
		er = knl_obdev_add(e.name, NULL, h[k].keyboard ? &kbd_ops : &ptr_ops, &in_tbl[slot], &e.uuid);
		LOCK();
		if ( er >= E_OK ) {
			in_tbl[slot].uuid = e.uuid;
		} else {
			in_tbl[slot].used = FALSE;
		}
		UNLOCK();
		changed = TRUE;
	}
	Kfree(h);

	if ( changed && ts_hid_stat(&st) >= E_OK
	  && ( st.keyboards != in_nkbd || st.pointers != in_nptr ) ) {
		in_nkbd = st.keyboards;
		in_nptr = st.pointers;
		knl_obdev_post(&ob_uuid_input, OB_IN_STATE, OB_E_CHANGE, NULL);
	}
}

EXPORT void knl_obinput_start( void )
{
	T_CMTX	cmtx;
	T_HIDSTAT st;
	ER	er;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	in_mtx = tk_cre_mtx(&cmtx);
	if ( in_mtx <= 0 ) {
		return;
	}
	if ( ts_hid_stat(&st) >= E_OK ) {
		in_nkbd = st.keyboards;
		in_nptr = st.pointers;
	}
	er = knl_obdev_add((CONST UB *)"入力", &ob_uuid_input, &all_ops, NULL, NULL);
	if ( er < E_OK ) {
		tm_printf((UB *)"ob: no input object (%d)\n", (INT)er);
	}
}
