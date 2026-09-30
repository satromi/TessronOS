/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obusb.c
 *	The devices on the USB buses as objects (design 18.7)
 *
 *	Every device the USB stack has brought up, hubs included, is a
 *	device object while it is plugged in, named after where it is:
 *	"usb", the controller, the root port and the hub ports on the way
 *	("usb0-4.3" is port 3 of the hub on root port 4 of controller 0), so
 *	that a device put back where it was is the same object, its UUID
 *	kept in the device box under that name. Record 1 is "key value" text
 *	of what it is; record 0 links to the hub it hangs off and to the
 *	objects of what it provides -- its disk, the sound device, its
 *	keyboards and pointers -- and a hub's to the devices behind it.
 *	Coming and going are told as of any device, on the object and on
 *	the list of the devices.
 *
 *	The USB manager calls knl_obusb_changed from its task after a device
 *	came or went, holding none of its locks; the list is then read
 *	again and the objects follow it. The system object's record 4 is
 *	made from these objects.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/usb.h>
#include <ts/snd.h>
#include <ts/hid.h>
#include "obj.h"

#define UO_MAX		32		/* devices shown at once */
#define UO_TEXT_MAX	1024

typedef struct {
	BOOL		used;
	T_USBDEV	d;
	UB		name[OB_DV_NAME];
	UB		port[24];
	TS_UUID		uuid;
} UODEV;

LOCAL UODEV	uo_tbl[UO_MAX];
LOCAL ID	uo_mtx = 0;
LOCAL ID	uo_sync_mtx = 0;
LOCAL BOOL	uo_started = FALSE;

#define LOCK()		tk_loc_mtx(uo_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(uo_mtx)

/* "0-4.3": the controller, the root port from 1, the hub ports on the way */
EXPORT INT knl_obusb_path( CONST T_USBDEV *d, UB *out, INT max )
{
	INT	n, t;

	n = knl_oj_put_num(out, 0, max, d->hc);
	n = knl_oj_put(out, n, max, "-");
	n = knl_oj_put_num(out, n, max, d->root_port + 1);
	for ( t = 0; t < (INT)d->depth && t < 5; t++ ) {
		n = knl_oj_put(out, n, max, ".");
		n = knl_oj_put_num(out, n, max, ( d->route >> ( t * 4 ) ) & 0xF);
	}
	return n;
}

LOCAL INT hex( UB *t, INT n, INT max, UW v, INT digits )
{
	char	s[12];
	INT	i;

	for ( i = 0; i < digits; i++ ) {
		UW	x = ( v >> ( ( digits - 1 - i ) * 4 ) ) & 0xF;

		s[i] = (char)( x < 10 ? '0' + x : 'a' + x - 10 );
	}
	s[i] = 0;
	return knl_oj_put(t, n, max, s);
}

LOCAL CONST char *speed_name( UINT s )
{
	switch ( s ) {
	case USB_SPEED_LOW:		return "low";
	case USB_SPEED_FULL:		return "full";
	case USB_SPEED_HIGH:		return "high";
	case USB_SPEED_SUPER:		return "super";
	case USB_SPEED_SUPER_PLUS:	return "super+";
	default:			return "unknown";
	}
}

/* What it is, in a word, as the system's record 4 has always said */
LOCAL CONST char *kind_of( CONST T_USBDEV *d )
{
	switch ( d->role ) {
	case USB_ROLE_HUB:	return "HUB";
	case USB_ROLE_HID:	return ( d->protocol == 1 ) ? "KB" : ( d->protocol == 2 ) ? "MOUSE" : "HID";
	case USB_ROLE_STORAGE:	return "DISK";
	case USB_ROLE_AUDIO:	return "AUDIO";
	default:		break;
	}
	switch ( d->dev_class ) {
	case 0x01:	return "AUDIO";
	case 0x02:	return "COMM";
	case 0x07:	return "PRT";
	case 0x08:	return "DISK";
	case 0x09:	return "HUB";
	default:	return "(未サポート)";
	}
}

LOCAL BOOL entry_of( UODEV *ctx, UODEV *out )
{
	BOOL	ok;

	LOCK();
	ok = ctx->used;
	if ( ok ) *out = *ctx;
	UNLOCK();
	return ok;
}

/* ---------------------------------------------------------------- the records */

LOCAL INT line( UB *t, INT n, INT max, CONST char *key )
{
	n = knl_oj_put(t, n, max, key);
	return knl_oj_put(t, n, max, "\t");
}

/* Record 1 */
LOCAL INT info_text( CONST UODEV *u, UB *t, INT max )
{
	CONST T_USBDEV	*d = &u->d;
	INT		n = 0;

	n = line(t, n, max, "VID");
	n = hex(t, n, max, d->vendor, 4);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "PID");
	n = hex(t, n, max, d->product, 4);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "CLASS");
	n = hex(t, n, max, d->dev_class, 2);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "SUBCLASS");
	n = hex(t, n, max, d->subclass, 2);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "PROTOCOL");
	n = hex(t, n, max, d->protocol, 2);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "SPEED");
	n = knl_oj_put(t, n, max, speed_name(d->speed));
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "PORT");
	n = knl_oj_put(t, n, max, (CONST char *)u->port);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "DEPTH");
	n = knl_oj_put_num(t, n, max, d->depth);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "ADDRESS");
	n = knl_oj_put_num(t, n, max, d->addr);
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "KIND");
	n = knl_oj_put(t, n, max, kind_of(d));
	n = line(t, knl_oj_put(t, n, max, "\n"), max, "PRODUCT");
	n = knl_oj_put(t, n, max, ( d->name[0] != 0 ) ? (CONST char *)d->name : "(no name)");
	n = knl_oj_put(t, n, max, "\n");
	if ( d->role == USB_ROLE_HUB ) {
		n = line(t, n, max, "HUB_PORTS");
		n = knl_oj_put_num(t, n, max, d->hub_ports);
		n = knl_oj_put(t, n, max, "\n");
	}
	if ( d->devnm[0] != 0 ) {
		n = line(t, n, max, "DEVICE");
		n = knl_oj_put(t, n, max, (CONST char *)d->devnm);
		n = knl_oj_put(t, n, max, "\n");
	}
	return n;
}

LOCAL INT usb_text( void *ctx, UB *t, INT n, INT max )
{
	UODEV		u, o;
	TS_UUID		f;
	T_HIDINFO	*h;
	INT		i, k;

	n = knl_oj_put(t, n, max, "<p>Record 1: what the device is, as key and value lines "
			  "(VID, PID, CLASS, SPEED, PORT, KIND, PRODUCT). It hangs off, and "
			  "provides:</p>");
	if ( !entry_of((UODEV *)ctx, &u) ) {
		return n;
	}
	for ( i = 0; i < UO_MAX; i++ ) {
		if ( entry_of(&uo_tbl[i], &o) && o.d.dev == u.d.parent ) {
			n = knl_obdev_link(t, n, max, &o.uuid, o.name);
		}
	}
	if ( u.d.devnm[0] != 0 && knl_obdev_uuid(u.d.devnm, &f) >= E_OK ) {
		n = knl_obdev_link(t, n, max, &f, u.d.devnm);
	}
	if ( ( u.d.role == USB_ROLE_AUDIO || u.d.dev_class == 0x01 )
	  && knl_obdev_uuid((CONST UB *)SND_DEVNM, &f) >= E_OK ) {
		n = knl_obdev_link(t, n, max, &f, (CONST UB *)SND_DEVNM);
	}
	h = (T_HIDINFO *)Kmalloc(sizeof(T_HIDINFO) * 16);
	if ( h != NULL ) {
		INT	nh = knl_hid_list(h, 16);

		for ( k = 0; k < nh && k < 16; k++ ) {
			if ( h[k].usbdev == u.d.dev && knl_obinput_uuid(h[k].id, &f) >= E_OK ) {
				n = knl_obdev_link(t, n, max, &f, (CONST UB *)( h[k].keyboard ? "keyboard"
											       : "pointer" ));
			}
		}
		Kfree(h);
	}
	if ( u.d.role == USB_ROLE_HUB ) {
		for ( i = 0; i < UO_MAX; i++ ) {
			if ( entry_of(&uo_tbl[i], &o) && o.d.parent == u.d.dev ) {
				n = knl_obdev_link(t, n, max, &o.uuid, o.name);
			}
		}
	}
	return n;
}

LOCAL INT usb_attr( void *ctx, UB *j, INT n, INT max )
{
	UODEV	u;

	if ( !entry_of((UODEV *)ctx, &u) ) {
		return n;
	}
	n = knl_oj_put(j, n, max, ",\"virtual\":false,\"vid\":\"");
	n = hex(j, n, max, u.d.vendor, 4);
	n = knl_oj_put(j, n, max, "\",\"pid\":\"");
	n = hex(j, n, max, u.d.product, 4);
	n = knl_oj_put(j, n, max, "\",\"class\":");
	n = knl_oj_put_num(j, n, max, u.d.dev_class);
	n = knl_oj_put(j, n, max, ",\"speed\":\"");
	n = knl_oj_put(j, n, max, speed_name(u.d.speed));
	n = knl_oj_put(j, n, max, "\",\"port\":");
	n = knl_oj_put_str(j, n, max, u.port);
	n = knl_oj_put(j, n, max, ",\"product\":");
	n = knl_oj_put_str(j, n, max, ( u.d.name[0] != 0 ) ? u.d.name : (CONST UB *)"");
	return knl_oj_put(j, n, max, ( u.d.role == USB_ROLE_HUB ) ? ",\"hub\":true" : ",\"hub\":false");
}

LOCAL ER usb_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	UODEV	u;
	UB	*t;
	INT	len;
	SZ	n = 0;

	(void)pos;
	if ( recno != OB_USB_INFO ) {
		return E_NOEXS;
	}
	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	if ( !entry_of((UODEV *)ctx, &u) ) {
		return E_NOEXS;
	}
	t = (UB *)Kmalloc(UO_TEXT_MAX);
	if ( t == NULL ) {
		return E_NOMEM;
	}
	len = info_text(&u, t, UO_TEXT_MAX);
	if ( len >= 0 && off < len ) {
		n = (SZ)( len - off );
		if ( n > size ) n = size;
		knl_memcpy(buf, t + off, (INT)n);
	}
	Kfree(t);
	if ( p_asize != NULL ) *p_asize = n;
	return E_OK;
}

LOCAL UD usb_size( void *ctx, INT recno )
{
	UODEV	u;
	UB	*t;
	INT	len = 0;

	if ( recno != OB_USB_INFO || !entry_of((UODEV *)ctx, &u) ) {
		return 0;
	}
	t = (UB *)Kmalloc(UO_TEXT_MAX);
	if ( t != NULL ) {
		len = info_text(&u, t, UO_TEXT_MAX);
		Kfree(t);
	}
	return ( len > 0 ) ? (UD)len : 0;
}

/* Everyone reads what it is; there is nothing to write */
LOCAL CONST T_OBDVOPS usb_ops = {
	OB_S_USB, "usb", OB_USB_NREC, 0444, 0, 0,
	usb_text, usb_attr, usb_rea, NULL, usb_size, NULL
};

/* ---------------------------------------------------------------- what is there */

EXPORT ER knl_obusb_uuid( INT usbdev, TS_UUID *p_uuid )
{
	INT	i;
	ER	er = E_NOEXS;

	if ( uo_mtx <= 0 ) {
		return E_NOEXS;
	}
	LOCK();
	for ( i = 0; i < UO_MAX; i++ ) {
		if ( uo_tbl[i].used && uo_tbl[i].d.dev == usbdev ) {
			*p_uuid = uo_tbl[i].uuid;
			er = E_OK;
			break;
		}
	}
	UNLOCK();
	return er;
}

/* The USB devices now, against those shown, and then the keyboards and pointers on them */
LOCAL void sync( void )
{
	T_USBDEV	*d;
	UB		gone[UO_MAX][OB_DV_NAME];
	INT		n, i, k, ng = 0;

	d = (T_USBDEV *)Kmalloc(sizeof(T_USBDEV) * UO_MAX);
	if ( d == NULL ) {
		return;
	}
	n = ts_usb_lst_dev(d, UO_MAX);
	if ( n > UO_MAX ) n = UO_MAX;

	LOCK();
	for ( i = 0; i < UO_MAX; i++ ) {
		if ( !uo_tbl[i].used ) continue;
		for ( k = 0; k < n && d[k].dev != uo_tbl[i].d.dev; k++ ) ;
		if ( k < n ) {
			uo_tbl[i].d = d[k];		/* a role or a disk's name that came since */
			continue;
		}
		knl_memcpy(gone[ng++], uo_tbl[i].name, OB_DV_NAME);
		uo_tbl[i].used = FALSE;
	}
	UNLOCK();
	for ( i = 0; i < ng; i++ ) {
		(void)knl_obdev_del(gone[i]);
	}

	/* parents first: the list is in that order */
	for ( k = 0; k < n; k++ ) {
		UODEV	e;
		INT	slot = -1;
		ER	er;

		LOCK();
		for ( i = 0; i < UO_MAX; i++ ) {
			if ( uo_tbl[i].used && uo_tbl[i].d.dev == d[k].dev ) break;
			if ( !uo_tbl[i].used && slot < 0 ) slot = i;
		}
		UNLOCK();
		if ( i < UO_MAX || slot < 0 ) continue;
		knl_memset(&e, 0, sizeof(e));
		e.d = d[k];
		(void)knl_obusb_path(&d[k], e.port, sizeof(e.port));
		i = knl_oj_put(e.name, 0, OB_DV_NAME, "usb");
		(void)knl_oj_put(e.name, i, OB_DV_NAME, (CONST char *)e.port);
		e.used = TRUE;
		LOCK();
		uo_tbl[slot] = e;
		UNLOCK();
		er = knl_obdev_add(e.name, NULL, &usb_ops, &uo_tbl[slot], &e.uuid);
		LOCK();
		if ( er >= E_OK ) {
			uo_tbl[slot].uuid = e.uuid;
		} else {
			uo_tbl[slot].used = FALSE;
		}
		UNLOCK();
	}
	Kfree(d);
	knl_obinput_sync();
}

EXPORT void knl_obusb_changed( void )
{
	if ( !uo_started ) {
		return;				/* before the object layer: knl_obusb_start does it */
	}
	tk_loc_mtx(uo_sync_mtx, TMO_FEVR);
	sync();
	tk_unl_mtx(uo_sync_mtx);
}

/* Once the object layer is up (knl_obdev_start): the devices there are now */
EXPORT void knl_obusb_start( void )
{
	T_CMTX	cmtx;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	uo_mtx = tk_cre_mtx(&cmtx);
	uo_sync_mtx = tk_cre_mtx(&cmtx);
	if ( uo_mtx <= 0 || uo_sync_mtx <= 0 ) {
		return;
	}
	uo_started = TRUE;
	knl_obusb_changed();
}

/*
 * The system object's record 4: a line for each USB device, made from
 * its object's record 1 read through the object, and the object named
 */
LOCAL BOOL field( CONST UB *t, INT len, CONST char *key, UB *out, INT max )
{
	INT	i = 0, k, m;

	while ( i < len ) {
		for ( k = 0; key[k] != 0 && i + k < len && t[i + k] == (UB)key[k]; k++ ) ;
		if ( key[k] == 0 && i + k < len && t[i + k] == '\t' ) {
			for ( m = 0, i += k + 1; i < len && t[i] != '\n' && m < max - 1; ) out[m++] = t[i++];
			out[m] = 0;
			return TRUE;
		}
		while ( i < len && t[i] != '\n' ) i++;
		i++;
	}
	out[0] = 0;
	return FALSE;
}

EXPORT INT knl_obusb_text( UB *t, INT max )
{
	UB	*r, v[48];
	char	us[TS_UUID_STRLEN + 1];
	TS_UUID	*ids;
	INT	n = 0, i, cnt = 0, depth;
	SZ	asz;
	ID	key;

	if ( !uo_started ) {
		return 0;
	}
	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * UO_MAX);
	r = (UB *)Kmalloc(UO_TEXT_MAX);
	if ( ids == NULL || r == NULL ) {
		if ( ids != NULL ) Kfree(ids);
		if ( r != NULL ) Kfree(r);
		return -1;
	}
	/* the objects, parents before their children */
	LOCK();
	for ( depth = 0; depth <= 6; depth++ ) {
		for ( i = 0; i < UO_MAX; i++ ) {
			if ( uo_tbl[i].used && (INT)uo_tbl[i].d.depth == depth && cnt < UO_MAX ) {
				ids[cnt++] = uo_tbl[i].uuid;
			}
		}
	}
	UNLOCK();
	for ( i = 0; i < cnt; i++ ) {
		asz = 0;
		key = ob_opn_obj(&ids[i], OB_OP_READ);
		if ( key <= 0 ) continue;
		if ( ob_rea_rec(key, OB_USB_INFO, 0, r, UO_TEXT_MAX, &asz) < E_OK ) asz = 0;
		(void)ob_cls_obj(key);
		if ( asz <= 0 ) continue;
		/* DEVICE depth port vid:pid kind name, as before */
		n = knl_oj_put(t, n, max, "DEVICE\t");
		(void)field(r, (INT)asz, "DEPTH", v, sizeof(v));
		n = knl_oj_put(t, n, max, (CONST char *)v);
		n = knl_oj_put(t, n, max, " ");
		(void)field(r, (INT)asz, "PORT", v, sizeof(v));
		n = knl_oj_put(t, n, max, (CONST char *)v);
		n = knl_oj_put(t, n, max, " ");
		(void)field(r, (INT)asz, "VID", v, sizeof(v));
		n = knl_oj_put(t, n, max, (CONST char *)v);
		n = knl_oj_put(t, n, max, ":");
		(void)field(r, (INT)asz, "PID", v, sizeof(v));
		n = knl_oj_put(t, n, max, (CONST char *)v);
		n = knl_oj_put(t, n, max, " ");
		(void)field(r, (INT)asz, "KIND", v, sizeof(v));
		n = knl_oj_put(t, n, max, (CONST char *)v);
		n = knl_oj_put(t, n, max, " ");
		(void)field(r, (INT)asz, "PRODUCT", v, sizeof(v));
		n = knl_oj_put(t, n, max, (CONST char *)v);
		n = knl_oj_put(t, n, max, "\nOBJECT\t");
		(void)ts_uuid_to_str(&ids[i], us, sizeof(us));
		n = knl_oj_put(t, n, max, us);
		n = knl_oj_put(t, n, max, "\n");
	}
	Kfree(ids);
	Kfree(r);
	return n;
}
