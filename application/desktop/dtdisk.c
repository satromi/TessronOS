/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtdisk.c
 *	ディスク操作: disks attached as volumes of objects (design 11.26)
 *
 *	A partition of TSFS is attached as a volume of objects with 接続,
 *	and the link to the object the volume opens with -- its top, made
 *	when it has none -- is put in the first window: the disk's link.
 *	切り離し takes a volume away again, unless an object of it is open
 *	in a window or by a program. The links to its objects stay, drawn
 *	pale (虚身), and come back as they were when it is attached again.
 *	状態表示 tells how full the disk of an object is. 区画作成 makes a
 *	TSFS partition in a free stretch of a disk, a volume on it, and
 *	attaches it; 拡張 makes the partition of a volume, and the volume,
 *	larger into the free stretch after it (gpt.h). Either on the disk
 *	the system volume is on asks for the administrator's password.
 *
 *	The volumes the person attached are remembered by their UUIDs in
 *	the device box (knl_obdev_vols_put) until they are taken away with
 *	切り離し or by deleting the disk's link in the first window
 *	(取り外し). Such a volume is attached again whenever its disk is
 *	found: at the start, and when a disk comes.
 *
 *	What happens without anyone asking is followed here as well. When a
 *	disk is taken out, its volumes are gone (the kernel's), the windows
 *	of their objects are closed, with a message saying so, and the
 *	links are drawn again. A window with changes not saved is not
 *	closed silently: the person is asked to save them to another
 *	volume, to have them saved when the disk is put back, or to throw
 *	them away.
 *
 *	The disk of the system volume taken out is followed the same way,
 *	except that the first window stays: the record it shows is taken
 *	from the store and kept as the window's own (dt_win_keep), the
 *	message line says the system's disk went, and what cannot be read
 *	without it fails as it would for any disk taken out. The same disk
 *	put back is attached again by itself, and the first window reads its
 *	record anew, writing what was changed in it meanwhile (dt_win_back).
 *
 *	Each step is said on the console, "disk: ...", for the tests.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/tad.h>
#include <ts/uuid.h>
#include <ts/gpt.h>
#include "desktop.h"

#define DK_WAIT		16		/* records waiting for their disk to be put back */
#define DK_PATH		64		/* bytes of a volume's path kept here */
#define DK_TO		8		/* other volumes offered to save to */

#define PN_DISK		10		/* the panel's choices, one per partition */
#define PN_TO		40		/* where changes of a window whose disk went are to go */
#define PN_SIZE		70		/* 区画作成 and 拡張: the size in megabytes */
#define PN_LABEL	71		/* 区画作成: the label of the volume made */

/* A record whose disk went while it had changes, kept until the disk is back */
typedef struct {
	BOOL	used;
	TS_UUID	vuuid;			/* the volume it is on */
	TS_UUID	id;
	INT	recno;
	UB	*xml;			/* the record as it was being changed */
	SZ	size;
	UB	name[TAD_NAME_MAX];
} DKWAIT;

LOCAL DKWAIT	dk_wait[DK_WAIT];
LOCAL UW	dk_volser = 0;		/* knl_obfile_serial as last followed */
LOCAL UW	dk_devser = 0;		/* knl_obdev_serial */
LOCAL BOOL	dk_started = FALSE;
LOCAL TS_UUID	dk_sysvu;		/* the system volume, the first attached at the start */
LOCAL BOOL	dk_has_sysvu = FALSE;
LOCAL BOOL	dk_sysgone = FALSE;	/* its disk is out */

/*
 * The windows, what each shows, whether that was on a volume and which,
 * as last looked at. A window is known by its number and its object
 * together: a window closed and another opened in its place between two
 * looks may be given the same number, and must not be taken for the one
 * before (and closed for a volume it never was on).
 */
LOCAL INT	dk_wid[DT_MAX_WIN];
LOCAL TS_UUID	dk_id[DT_MAX_WIN];
LOCAL BOOL	dk_onvol[DT_MAX_WIN];
LOCAL TS_UUID	dk_vu[DT_MAX_WIN];

/* ---------------------------------------------------------------- small things */

LOCAL INT put( UB *buf, INT at, INT max, CONST char *s )
{
	while ( *s != 0 && at < max - 1 ) {
		buf[at++] = (UB)*s++;
	}
	buf[at] = 0;
	return at;
}

LOCAL INT put_num( UB *buf, INT at, INT max, UD v )
{
	char	t[24];
	INT	n = 0;

	do {
		t[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && n < (INT)sizeof(t) );
	while ( n > 0 && at < max - 1 ) {
		buf[at++] = (UB)t[--n];
	}
	buf[at] = 0;
	return at;
}

/* A size the way a person reads it: 512KB, 64MB, 3GB */
LOCAL INT put_size( UB *buf, INT at, INT max, UD bytes )
{
	if ( bytes >= 1024ULL * 1024 * 1024 ) {
		at = put_num(buf, at, max, ( bytes + 512ULL * 1024 * 1024 ) / ( 1024ULL * 1024 * 1024 ));
		return put(buf, at, max, "GB");
	}
	if ( bytes >= 1024ULL * 1024 ) {
		at = put_num(buf, at, max, ( bytes + 512ULL * 1024 ) / ( 1024ULL * 1024 ));
		return put(buf, at, max, "MB");
	}
	at = put_num(buf, at, max, bytes / 1024);
	return put(buf, at, max, "KB");
}

LOCAL BOOL zero_uuid( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( u->b[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/* What a disk is called: its label, or its device's name */
LOCAL void disk_name( CONST UB *label, CONST char *dev, UB *out, INT max )
{
	INT	k;

	if ( label != NULL && label[0] != 0 ) {
		(void)put(out, 0, max, (CONST char *)label);
		return;
	}
	k = put(out, 0, max, "ディスク ");
	(void)put(out, k, max, dev);
}

/* ---------------------------------------------------------------- what is remembered as attached */

LOCAL void keep_vol( CONST TS_UUID *vuuid, CONST TS_UUID *top, BOOL keep )
{
	char	us[TS_UUID_STRLEN + 1];
	ER	er = knl_obdev_vols_put(vuuid, top, keep);

	(void)ts_uuid_to_str(vuuid, us, sizeof(us));
	tm_printf((UB *)"disk: %s %s (%d)\n", keep ? "remember" : "forget", us, (INT)er);
}

/*
 * The volumes remembered, every one: their UUIDs, and their top objects
 * in *p_top; the caller frees what this answers (*p_top is in it)
 */
LOCAL TS_UUID *kept_vols( INT *p_n, TS_UUID **p_top )
{
	INT	n = knl_obdev_vols_get(NULL, NULL, 0) + 4;
	TS_UUID	*vu = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n * 2);

	*p_top = ( vu != NULL ) ? vu + n : NULL;
	*p_n = ( vu != NULL ) ? knl_obdev_vols_get(vu, vu + n, n) : 0;
	if ( *p_n > n ) *p_n = n;		/* more came meanwhile: they are looked at the next time */
	return vu;
}

/*
 * The disks and partitions the device manager knows, every one, OB_DV_NAME
 * bytes each, in memory of their own (Kfree it): how many in *p_n
 */
LOCAL UB *disks_all( INT *p_n )
{
	INT	n = knl_obdev_disks(NULL, 0) + 8;
	UB	*names = (UB *)Kmalloc((SZ)OB_DV_NAME * (SZ)n);

	*p_n = ( names != NULL ) ? knl_obdev_disks(names, n) : 0;
	return names;
}

LOCAL INT kept_at( CONST TS_UUID *vu, INT n, CONST TS_UUID *vuuid )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( ts_uuid_cmp(&vu[i], vuuid) == 0 ) return i;
	}
	return -1;
}

/* ---------------------------------------------------------------- the first window's links */

/* Whether the first window holds a link to the object */
LOCAL BOOL on_desktop( CONST TS_UUID *u )
{
	DTWIN	*t = dt_first_win();
	T_VOBJ	*v;
	INT	i, n;
	BOOL	hit = FALSE;

	if ( t == NULL || t->rec == NULL || ( v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ)) ) == NULL ) {
		return FALSE;
	}
	n = tad_lnk_count(t->rec);
	for ( i = 0; i < n && !hit; i++ ) {
		hit = (BOOL)( tad_lnk_get(t->rec, i, v) >= E_OK && ts_uuid_cmp(&v->target, u) == 0 );
	}
	Kfree(v);
	return hit;
}

/* ---------------------------------------------------------------- attaching */

LOCAL void wait_done( void );

/*
 * A partition of TSFS attached by its device's name. The volume's top
 * object is made when it has none: a figure, named as the disk is, the
 * way a cabinet is. 'place' puts a link to it in the first window unless
 * one is there. Answers E_OBJ when the same volume is attached through
 * another device.
 */
LOCAL ER attach_dev( CONST char *dev, BOOL place, UB *p_name, INT max )
{
	T_OBFVREF	*ref;
	TS_UUID		top;
	UB		name[OB_VOL_LABEL + 16];
	ER		er;

	er = ob_att_vol(dev, TSFS_STORE_BLK);
	if ( er < E_OK ) {
		tm_printf((UB *)"disk: attach %s failed %d\n", dev, (INT)er);
		return er;
	}
	ref = (T_OBFVREF *)Kmalloc(sizeof(T_OBFVREF));
	if ( ref == NULL ) {
		return E_NOMEM;
	}
	er = knl_obfile_vol_ref(dev, ref);
	if ( er < E_OK ) {
		Kfree(ref);
		return er;
	}
	disk_name(ref->label, dev, name, sizeof(name));
	top = ref->top;
	if ( zero_uuid(&top) && !ref->rdonly ) {
		er = om_cre_obj(dev, name, OM_BODY_FIG, &top);
		if ( er >= E_OK ) {
			er = knl_obfile_set_top(dev, &top);
		}
		if ( er < E_OK ) {
			tm_printf((UB *)"disk: no top object on %s (%d)\n", dev, (INT)er);
			knl_memset(&top, 0, sizeof(top));
		}
	}
	if ( place ) {
		keep_vol(&ref->vuuid, &top, TRUE);	/* attached by the person: again at the next start */
	}
	Kfree(ref);
	om_store_recheck();
	if ( place && !zero_uuid(&top) && !on_desktop(&top) ) {
		dt_place_on_desktop(&top, name);
	}
	{
		char	us[TS_UUID_STRLEN + 1];

		(void)ts_uuid_to_str(&top, us, sizeof(us));
		tm_printf((UB *)"disk: attached %s \"%s\" top %s\n", dev, (CONST char *)name, us);
	}
	if ( p_name != NULL ) {
		(void)put(p_name, 0, max, (CONST char *)name);
	}
	wait_done();
	dt_draw_all();
	return E_OK;
}

/* ---------------------------------------------------------------- following what happens */

/*
 * The links of a record whose volume went made to say that volume: a
 * link without one meant an object of the same volume, which it no
 * longer is once the record is written somewhere else.
 */
LOCAL void links_name_vol( T_TAD *doc, CONST TS_UUID *vuuid )
{
	T_VOBJ	*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	INT	i, n;

	if ( v == NULL || doc == NULL ) {
		if ( v != NULL ) Kfree(v);
		return;
	}
	n = tad_lnk_count(doc);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(doc, i, v) < E_OK || !zero_uuid(&v->tvol) || !zero_uuid(&v->tmach) ) {
			continue;
		}
		if ( knl_obfile_vol_of(&v->target) > 0 ) {
			continue;		/* found elsewhere: the save says where */
		}
		v->tvol = *vuuid;
		if ( tad_lnk_set(doc, v) < E_OK ) {
			tm_printf((UB *)"desktop: the link \"%s\" was not told its volume\n", v->name);
		}
	}
	Kfree(v);
}

/*
 * A record written as a new object of the volume 'path', named as the
 * one it was, with a link to it in the volume's top (the first window
 * for the system's volume).
 */
LOCAL ER save_to( CONST char *path, CONST UB *name, UINT body, CONST UB *xml, SZ size, TS_UUID *p_new )
{
	TS_UUID	top;
	ER	er;

	er = om_cre_obj(path, name, body, p_new);
	if ( er < E_OK ) {
		return er;
	}
	if ( om_store_get(p_new, 0) == NULL || om_store_restore(p_new, 0, xml, size) == NULL ) {
		return E_IO;
	}
	er = om_store_save(p_new, 0);
	if ( er < E_OK ) {
		return er;
	}
	if ( !dt_disk_top(path, &top) || dt_place_in(&top, p_new, name) < E_OK ) {
		dt_place_on_desktop(p_new, name);
	}
	return E_OK;
}

/* A record kept until its disk is back */
LOCAL BOOL wait_put( CONST TS_UUID *vuuid, CONST TS_UUID *id, INT recno, UB *xml, SZ size, CONST UB *name )
{
	INT	i;

	for ( i = 0; i < DK_WAIT; i++ ) {
		if ( dk_wait[i].used && ts_uuid_cmp(&dk_wait[i].id, id) == 0 && dk_wait[i].recno == recno ) {
			Kfree(dk_wait[i].xml);		/* the newer changes stand */
			dk_wait[i].used = FALSE;
		}
	}
	for ( i = 0; i < DK_WAIT; i++ ) {
		if ( !dk_wait[i].used ) {
			dk_wait[i].used = TRUE;
			dk_wait[i].vuuid = *vuuid;
			dk_wait[i].id = *id;
			dk_wait[i].recno = recno;
			dk_wait[i].xml = xml;
			dk_wait[i].size = size;
			(void)put(dk_wait[i].name, 0, TAD_NAME_MAX, (CONST char *)name);
			return TRUE;
		}
	}
	return FALSE;
}

/* The records kept for a volume that is back, written to it */
LOCAL void wait_done( void )
{
	UB	msg[TAD_NAME_MAX + 64];
	INT	i, k;
	ER	er;

	for ( i = 0; i < DK_WAIT; i++ ) {
		DKWAIT	*w = &dk_wait[i];

		if ( !w->used || !knl_obfile_vuuid_on(&w->vuuid) ) {
			continue;
		}
		er = E_NOEXS;
		if ( knl_obfile_vol_of(&w->id) > 0 ) {
			om_store_forget(&w->id);
			if ( om_store_get(&w->id, w->recno) != NULL
			  && om_store_restore(&w->id, w->recno, w->xml, w->size) != NULL ) {
				er = om_store_save(&w->id, w->recno);
			}
		}
		tm_printf((UB *)"disk: kept changes of %s saved (%d)\n", (CONST char *)w->name, (INT)er);
		k = put(msg, 0, sizeof(msg), "「");
		k = put(msg, k, sizeof(msg), (CONST char *)w->name);
		(void)put(msg, k, sizeof(msg), ( er >= E_OK ) ? "」の変更を保存しました"
							 : "」の変更を保存できませんでした");
		(void)wm_msg_put((CONST char *)msg);
		Kfree(w->xml);
		w->used = FALSE;
	}
}

/*
 * A window whose disk went with changes not saved: the person says
 * where they go -- another volume attached, the disk when it is put
 * back, or nowhere. The window is closed after.
 */
LOCAL void rescue( DTWIN *d, CONST TS_UUID *vuuid, CONST UB *name )
{
	UB		(*to)[DK_PATH], vname[OB_VOL_LABEL + 16], t[WM_LABEL_MAX], msg[TAD_NAME_MAX + 96];
	T_OBFVREF	ref;
	T_WMPANEL	*def;
	T_WMPART	*pt;
	UB		*xml = NULL;
	SZ		size = 0;
	INT		n, m = 0, i, k, pid, w = 600, h, pick = -1;
	UINT		ans;
	DTWIN		*f = dt_first_win();
	TS_UUID		nid;
	ER		er;

	if ( d->xml_view ) {
		(void)dd_xml_commit(d);
	}
	links_name_vol((T_TAD *)d->rec, vuuid);
	if ( om_store_snapshot(&d->id, d->recno, &xml, &size) < E_OK ) {
		tm_printf((UB *)"disk: changes of %s cannot be kept\n", (CONST char *)name);
		return;
	}
	to = (UB (*)[DK_PATH])Kmalloc(sizeof(UB) * DK_PATH * DK_TO);
	n = ( to != NULL ) ? knl_obfile_vols((UB *)to, DK_PATH, DK_TO) : 0;
	for ( i = 0; i < n; i++ ) {
		if ( knl_obfile_vol_ref((CONST char *)to[i], &ref) >= E_OK && !ref.rdonly ) {
			if ( m != i ) knl_memcpy(to[m], to[i], DK_PATH);
			m++;
		}
	}
	tm_printf((UB *)"disk: %s has changes not saved; %d volumes to save to\n", (CONST char *)name, m);
	h = 120 + 26 * ( m + 2 );
	def = dt_pn_new(w, h);
	if ( def == NULL || f == NULL ) {
		if ( def != NULL ) Kfree(def);
		goto keep;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, w - 28, 22, "ディスクが取り外されました。");
	k = put(t, 0, sizeof(t), "「");
	k = put(t, k, sizeof(t), (CONST char *)name);
	(void)put(t, k, sizeof(t), "」の変更は保存されていません。どうしますか:");
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 30, w - 28, 22, (CONST char *)t);
	for ( i = 0; i < m + 2; i++ ) {
		if ( i < m ) {
			if ( knl_obfile_vol_ref((CONST char *)to[i], &ref) >= E_OK ) {
				disk_name(ref.label, (CONST char *)to[i], vname, sizeof(vname));
			} else {
				(void)put(vname, 0, sizeof(vname), (CONST char *)to[i]);
			}
			k = put(t, 0, sizeof(t), "「");
			k = put(t, k, sizeof(t), (CONST char *)vname);
			(void)put(t, k, sizeof(t), "」へ別の実身として保存");
		} else if ( i == m ) {
			(void)put(t, 0, sizeof(t), "ディスクを挿し直したときに保存");
		} else {
			(void)put(t, 0, sizeof(t), "変更を捨てる");
		}
		pt = dt_pn_add(def, WM_PT_CHOICE, PN_TO + i, 24, 58 + 26 * i, w - 48, 22, (CONST char *)t);
		if ( pt != NULL ) {
			pt->group = PN_TO;
			pt->value = ( i == 0 ) ? 1 : 0;
		}
	}
	dt_pn_buttons(def, "実行");
	ans = dt_pn_run(f, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		for ( i = 0; i < m + 2; i++ ) {
			if ( dt_pn_value(pid, PN_TO + i) != 0 ) pick = i;
		}
	}
	dt_pn_done(f, pid);
	wm_update();
	if ( pick == m + 1 ) {
		tm_printf((UB *)"disk: changes of %s thrown away\n", (CONST char *)name);
		Kfree(xml);
		goto out;
	}
	if ( pick >= 0 && pick < m ) {
		er = save_to((CONST char *)to[pick], name, ( d->fig != NULL ) ? OM_BODY_FIG : OM_BODY_DOC,
			     xml, size, &nid);
		tm_printf((UB *)"disk: changes of %s saved to %s (%d)\n", (CONST char *)name,
			  (CONST char *)to[pick], (INT)er);
		if ( er >= E_OK ) {
			k = put(msg, 0, sizeof(msg), "「");
			k = put(msg, k, sizeof(msg), (CONST char *)name);
			(void)put(msg, k, sizeof(msg), "」を別の実身として保存しました");
			(void)wm_msg_put((CONST char *)msg);
			Kfree(xml);
			goto out;
		}
		dt_tell(f, "保存できませんでした。", "ディスクを挿し直したときに保存します");
	}
    keep:
	/* cancelled, or not written: kept for the disk, rather than lost */
	if ( wait_put(vuuid, &d->id, d->recno, xml, size, name) ) {
		tm_printf((UB *)"disk: changes of %s kept for the disk\n", (CONST char *)name);
		k = put(msg, 0, sizeof(msg), "「");
		k = put(msg, k, sizeof(msg), (CONST char *)name);
		(void)put(msg, k, sizeof(msg), "」の変更は、ディスクを挿し直したときに保存します");
		(void)wm_msg_put((CONST char *)msg);
	} else {
		Kfree(xml);
	}
    out:
	if ( to != NULL ) Kfree(to);
}

#ifdef X64
/*
 * The console's last lines in a window of their own, when the disk of the
 * system volume went: on a PC that disk holds the records' files too
 * (bootlog.c), so what the console said as it went -- the USB port's
 * state among it -- is kept where it can be read off the screen until
 * the disk is back and the files are written.
 */
#define CL_LINES	40
#define CL_MAX		( 512 + CL_LINES * ( 200 * 6 + 8 ) )

IMPORT UD	knl_bootlog_head( void );
IMPORT void	knl_bootlog_tail( UD until, INT lines, void (*out)( CONST UB *s, INT n ) );

LOCAL UB	*cl_xml;
LOCAL INT	cl_at;
LOCAL UB	(*cl_text)[200];	/* the lines as the console said them, the oldest first */
LOCAL INT	cl_len[CL_LINES + 2];
LOCAL INT	cl_n;

LOCAL void cl_put( CONST char *s )
{
	while ( *s != 0 && cl_at < CL_MAX - 1 ) cl_xml[cl_at++] = (UB)*s++;
}

/*
 * One line of the console as a paragraph, what XML gives a meaning to
 * written out. A line is cut at a length, perhaps inside a letter of
 * several bytes, and a record must be UTF-8 throughout: a letter not
 * whole is a '?'.
 */
LOCAL void cl_para( CONST UB *s, INT n )
{
	INT	i, k, len;

	cl_put("<p>");
	for ( i = 0; i < n && cl_at < CL_MAX - 16; i++ ) {
		UB	c = s[i];

		if ( c >= 0x80 ) {
			len = ( c >= 0xf0 && c < 0xf8 ) ? 4 : ( c >= 0xe0 ) ? 3 : ( c >= 0xc2 && c < 0xe0 ) ? 2 : 0;
			for ( k = 1; len > 0 && k < len; k++ ) {
				if ( i + k >= n || ( s[i + k] & 0xc0 ) != 0x80 ) len = 0;
			}
			if ( len == 0 ) {
				cl_xml[cl_at++] = '?';
				continue;
			}
			for ( k = 0; k < len; k++ ) cl_xml[cl_at++] = s[i + k];
			i += len - 1;
		} else if ( c == '&' ) {
			cl_put("&amp;");
		} else if ( c == '<' ) {
			cl_put("&lt;");
		} else if ( c == '>' ) {
			cl_put("&gt;");
		} else if ( c < 0x20 || c == 0x7f ) {
			cl_xml[cl_at++] = ' ';		/* the console's colours and controls */
		} else {
			cl_xml[cl_at++] = c;
		}
	}
	cl_put("</p>");
}

/* A line of the console kept, to be written out the newest first */
LOCAL void cl_line( CONST UB *s, INT n )
{
	INT	i;

	if ( cl_n >= CL_LINES + 2 ) {
		return;
	}
	for ( i = 0; i < n && i < 200; i++ ) cl_text[cl_n][i] = s[i];
	cl_len[cl_n++] = i;
}

LOCAL void show_console( void )
{
	CONST char	*title = "システムのディスクが取り外されたときのコンソール（新しい行が上）";
	T_DPRECT	o;
	T_TAD		*rec = NULL;
	INT		i;

	cl_xml = (UB *)Kmalloc(CL_MAX);
	cl_text = Kmalloc(sizeof(*cl_text) * ( CL_LINES + 2 ));
	if ( cl_xml == NULL || cl_text == NULL ) {
		if ( cl_xml != NULL ) Kfree(cl_xml);
		if ( cl_text != NULL ) Kfree(cl_text);
		return;
	}
	cl_n = 0;
	cl_at = 0;
	cl_put("<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	cl_put(title);
	cl_put("\"><document>");
	knl_bootlog_tail(knl_bootlog_head(), CL_LINES, cl_line);
	for ( i = cl_n - 1; i >= 0; i-- ) {
		cl_para(cl_text[i], cl_len[i]);	/* the newest at the top, where a photograph sees it */
	}
	Kfree(cl_text);
	cl_text = NULL;
	cl_put("</document></tad>");
	if ( tad_parse(cl_xml, (SZ)cl_at, NULL, &rec) < E_OK ) {
		rec = NULL;
	}
	Kfree(cl_xml);
	cl_xml = NULL;
	if ( rec == NULL ) {
		tm_printf((UB *)"disk: the console's lines not shown\n");
		return;
	}
	o.left = 40;
	o.top = 240;
	o.right = 1000;
	o.bottom = 740;
	(void)dt_open_made(rec, title, DT_TOOL_NONE, &o);
}
#endif

/*
 * The first window, whose object's volume went: never closed, it keeps
 * the record it shows as its own. Answers what the message line is to
 * say after the windows closed have said theirs: that the disk went,
 * the system's own disk said as such.
 */
LOCAL CONST char *keep_first( DTWIN *d, CONST TS_UUID *vuuid )
{
	BOOL	sys = (BOOL)( dk_has_sysvu && ts_uuid_cmp(vuuid, &dk_sysvu) == 0 );

	if ( d->xml_view ) {
		(void)dd_xml_commit(d);
	}
	if ( !dt_win_keep(d) ) {
		tm_printf((UB *)"disk: the first window's record is not the store's\n");
	}
	if ( sys ) {
		dk_sysgone = TRUE;
		tm_printf((UB *)"disk: the system volume went\n");
		return "システムのディスクが取り外されました。同じディスクを挿し直すと元に戻ります";
	}
	tm_printf((UB *)"disk: the first window's volume went\n");
	return "初期ウィンドウのディスクが取り外されました。挿し直すと元に戻ります";
}

/*
 * The windows of the parts that hold a record of the store's past
 * drawing (a player of 実時間制御TAD, a slide show), closed when that
 * record's object can no longer be found: the store lets go of it next.
 */
LOCAL void close_lost_parts( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN	*d = dt_win_at(i);

		if ( d == NULL || !d->used || d->own == NULL || d->own->lost == NULL
		  || !d->own->lost(d) ) {
			continue;
		}
		tm_printf((UB *)"disk: closed window of %s\n", (CONST char *)d->name);
		dt_close(d);
		(void)wm_msg_put("ディスクが取り外されたため、ウインドウを閉じました");
	}
}

/* The windows of objects on a volume that went: closed, and said so */
LOCAL void close_gone( void )
{
	CONST char	*said = NULL;
	INT		i;

	close_lost_parts();

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN	*d = dt_win_at(i);
		UB	name[TAD_NAME_MAX], msg[TAD_NAME_MAX + 96];
		INT	k;

		if ( d == NULL || !d->used || d->wid != dk_wid[i] || !dk_onvol[i]
		  || ts_uuid_cmp(&d->id, &dk_id[i]) != 0 ) {
			continue;
		}
		if ( knl_obfile_vol_of(&d->id) > 0 ) {
			continue;
		}
		if ( d == dt_first_win() ) {
			said = keep_first(d, &dk_vu[i]);
			dk_wid[i] = 0;
			continue;
		}
		if ( om_store_name(&d->id, name, sizeof(name)) < 0 ) {
			(void)put(name, 0, sizeof(name), "実身");
		}
		k = put(msg, 0, sizeof(msg), "ディスクが取り外されたため、");
		k = put(msg, k, sizeof(msg), (CONST char *)name);
		(void)put(msg, k, sizeof(msg), "のウインドウを閉じました");
		if ( d->dirty && !d->sealed ) {
			rescue(d, &dk_vu[i], name);	/* says where the changes went */
			msg[0] = 0;
		}
		tm_printf((UB *)"disk: closed window of %s\n", (CONST char *)name);
		d->dirty = FALSE;		/* what changed went where the person said */
		dt_close(d);
		if ( msg[0] != 0 ) (void)wm_msg_put((CONST char *)msg);
		dk_wid[i] = 0;
	}
	if ( said != NULL ) {
#ifdef X64
		if ( dk_sysgone ) {
			show_console();		/* after the windows that went, in a place of the table free now */
		}
#endif
		(void)wm_msg_put(said);
	}
}

/* Which window shows what, and whether that is on a volume: new windows looked at once */
LOCAL void note_windows( BOOL all )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN	*d = dt_win_at(i);

		if ( d == NULL || !d->used || d->sealed || d->tool != DT_TOOL_NONE ) {
			dk_wid[i] = 0;
			continue;
		}
		if ( all || d->wid != dk_wid[i] || ts_uuid_cmp(&d->id, &dk_id[i]) != 0 ) {
			dk_wid[i] = d->wid;
			dk_id[i] = d->id;
			dk_onvol[i] = (BOOL)( knl_obfile_vol_of(&d->id) > 0 );
			if ( !dk_onvol[i] || knl_obfile_vuuid_of(&d->id, &dk_vu[i]) < E_OK ) {
				knl_memset(&dk_vu[i], 0, sizeof(dk_vu[i]));
			}
		}
	}
}

/*
 * The disks looked over after a device or a medium came or went, and at
 * the start: a partition of TSFS whose volume is remembered as attached
 * is attached again, and what was kept for it is written.
 */
LOCAL void look_disks( void )
{
	UB		*names;
	T_FSPROBE	*pr;
	TS_UUID		*kv, *kt;
	UB		path[OB_DV_NAME];
	INT		n = 0, i, k, nk = 0;

	pr = (T_FSPROBE *)Kmalloc(sizeof(T_FSPROBE));
	kv = kept_vols(&nk, &kt);
	names = ( nk > 0 || dk_sysgone ) ? disks_all(&n) : NULL;
	if ( pr == NULL || kv == NULL ) {
		goto done;
	}
	if ( names == NULL ) {
		n = 0;
	}
	for ( i = 0; i < n; i++ ) {
		CONST char	*dev = (CONST char *)( names + i * OB_DV_NAME );

		if ( knl_obfile_vol_path(dev) > 0 ) {
			continue;		/* attached already */
		}
		if ( knl_fs_probe(dev, pr) < E_OK || pr->kind != FS_KIND_TSFS
		  || knl_obfile_vuuid_on(&pr->vuuid) ) {
			continue;
		}
		if ( kept_at(kv, nk, &pr->vuuid) < 0
		  && !( dk_sysgone && ts_uuid_cmp(&pr->vuuid, &dk_sysvu) == 0 ) ) {
			continue;		/* neither remembered nor the system's */
		}
		for ( k = 0; dev[k] != 0 && k < OB_DV_NAME - 1; k++ ) path[k] = (UB)dev[k];
		path[k] = 0;
		tm_printf((UB *)"disk: %s is back\n", (CONST char *)path);
		(void)attach_dev((CONST char *)path, FALSE, NULL, 0);
	}
	wait_done();

    done:
	if ( names != NULL ) Kfree(names);
	if ( pr != NULL ) Kfree(pr);
	if ( kv != NULL ) Kfree(kv);
}

/* The first window kept while its disk was out: its object found again, read anew */
LOCAL void first_back( void )
{
	DTWIN	*f = dt_first_win();
	ER	er;

	if ( dk_sysgone && dk_has_sysvu && knl_obfile_vuuid_on(&dk_sysvu) ) {
		dk_sysgone = FALSE;
		tm_printf((UB *)"disk: the system volume is back\n");
	}
	if ( f == NULL || !f->used || !f->kept || knl_obfile_vol_of(&f->id) <= 0 ) {
		return;
	}
	er = dt_win_back(f);
	tm_printf((UB *)"disk: the first window read again (%d)\n", (INT)er);
	if ( er >= E_OK ) {
		(void)wm_msg_put("ディスクが挿し直され、元に戻りました");
	} else if ( er != E_NOEXS ) {
		(void)wm_msg_put("ディスクが挿し直されましたが、初期ウィンドウの変更を保存できませんでした");
	}
}

EXPORT void dt_disk_poll( void )
{
	UW	vs, ds;

	knl_obxref_drain();		/* links to other volumes given up by a collection, say */
	vs = knl_obfile_serial();
	ds = knl_obdev_serial();

	if ( !dk_started ) {
		dk_started = TRUE;
		dk_has_sysvu = (BOOL)( knl_obfile_machine(&dk_sysvu) >= E_OK && !zero_uuid(&dk_sysvu) );
		dk_volser = vs;
		dk_devser = ds;
		note_windows(TRUE);
		look_disks();			/* the disks of the links left on the desktop */
		return;
	}
	if ( ds != dk_devser ) {
		dk_devser = ds;
		look_disks();
		vs = knl_obfile_serial();
	}
	if ( vs != dk_volser ) {
		dk_volser = vs;
		close_gone();
		om_store_recheck();
		first_back();
		note_windows(TRUE);
		tm_printf((UB *)"disk: volumes changed\n");
		dt_draw_all();
		wm_update();
		return;
	}
	note_windows(FALSE);
}

/* ---------------------------------------------------------------- 接続 */

/* The partitions of TSFS that are not attached, and what is on the others, offered in a panel */
EXPORT void dt_disk_connect( DTWIN *d, CONST T_VOBJ *v )
{
	UB		*names, (*rows)[WM_LABEL_MAX];
	UINT		*kinds;
	T_FSPROBE	*pr;
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		n, i, k, m = 0, pid, w = 480, h, at = -1;
	UINT		ans;
	UB		name[OB_VOL_LABEL + 16];

	if ( d == NULL ) {
		d = dt_first_win();
	}
	if ( d == NULL ) {
		return;
	}
	/* a row for every disk and partition the device manager knows, as many as there are */
	names = disks_all(&n);
	rows = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * (SZ)( n + 1 ));
	kinds = (UINT *)Kmalloc(sizeof(UINT) * (SZ)( n + 1 ));
	pr = (T_FSPROBE *)Kmalloc(sizeof(T_FSPROBE));
	if ( names == NULL || rows == NULL || kinds == NULL || pr == NULL ) {
		goto done;
	}
	for ( i = 0; i < n; i++ ) {
		CONST char	*dev = (CONST char *)( names + i * OB_DV_NAME );

		if ( knl_obfile_vol_path(dev) > 0 || knl_fs_probe(dev, pr) < E_OK
		  || pr->kind == FS_KIND_PARTED || pr->kind == FS_KIND_NONE ) {
			continue;
		}
		k = put(rows[m], 0, WM_LABEL_MAX, dev);
		k = put(rows[m], k, WM_LABEL_MAX, "  ");
		k = put(rows[m], k, WM_LABEL_MAX, (CONST char *)pr->name);
		if ( pr->label[0] != 0 ) {
			k = put(rows[m], k, WM_LABEL_MAX, "「");
			k = put(rows[m], k, WM_LABEL_MAX, (CONST char *)pr->label);
			k = put(rows[m], k, WM_LABEL_MAX, "」");
		}
		k = put(rows[m], k, WM_LABEL_MAX, "  ");
		(void)put_size(rows[m], k, WM_LABEL_MAX, pr->bytes);
		kinds[m] = pr->kind;
		/* a 虚身 chosen: its disk, when it is in */
		if ( at < 0 && v != NULL && pr->kind == FS_KIND_TSFS && !zero_uuid(&pr->top)
		  && ts_uuid_cmp(&pr->top, &v->target) == 0 ) {
			at = m;
		}
		/* the names are kept beside the rows, for after the panel */
		knl_memcpy(names + m * OB_DV_NAME, dev, OB_DV_NAME);
		m++;
	}
	tm_printf((UB *)"disk: connect offers %d\n", m);
	if ( m == 0 ) {
		dt_tell(d, "接続できるディスクがありません", NULL);
		goto done;
	}
	if ( at < 0 ) {
		for ( i = 0; i < m && at < 0; i++ ) {
			if ( kinds[i] == FS_KIND_TSFS ) at = i;
		}
	}
	h = 96 + 26 * m;
	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		goto done;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, w - 28, 22, "接続するディスク:");
	for ( i = 0; i < m; i++ ) {
		pt = dt_pn_add(def, WM_PT_CHOICE, PN_DISK + i, 24, 30 + 26 * i, w - 48, 22,
			       (CONST char *)rows[i]);
		if ( pt != NULL ) {
			pt->group = PN_DISK;
			pt->value = ( i == ( at < 0 ? 0 : at ) ) ? 1 : 0;
		}
	}
	dt_pn_buttons(def, "接続");
	ans = dt_pn_run(d, def, &pid);
	k = -1;
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		for ( i = 0; i < m; i++ ) {
			if ( dt_pn_value(pid, PN_DISK + i) != 0 ) k = i;
		}
	}
	dt_pn_done(d, pid);
	wm_update();
	if ( k < 0 ) {
		tm_printf((UB *)"disk: connect cancelled\n");
		goto done;
	}
	{
		CONST char	*dev = (CONST char *)( names + k * OB_DV_NAME );
		ER		er;

		tm_printf((UB *)"disk: connect %s\n", dev);
		if ( kinds[k] == FS_KIND_FAT12 || kinds[k] == FS_KIND_FAT16 || kinds[k] == FS_KIND_FAT32
		  || kinds[k] == FS_KIND_EXFAT || kinds[k] == FS_KIND_NTFS || kinds[k] == FS_KIND_BTRON ) {
			/* files, not objects: ファイル変換 reads them */
			tm_printf((UB *)"disk: %s is files, not objects\n", dev);
			dt_tell(d, "実身のディスクではありません。", "ファイル変換で開いてください");
			goto done;
		}
		if ( kinds[k] != FS_KIND_TSFS ) {
			tm_printf((UB *)"disk: %s is of a kind not handled\n", dev);
			dt_tell(d, "この形式は扱えません", NULL);
			goto done;
		}
		er = attach_dev(dev, TRUE, name, sizeof(name));
		if ( er == E_OBJ ) {
			tm_printf((UB *)"disk: %s is attached already\n", dev);
			dt_tell(d, "同じディスクがすでに接続されています", NULL);
		} else if ( er < E_OK ) {
			dt_tell(d, "ディスクを接続できませんでした", NULL);
		} else {
			UB	msg[OB_VOL_LABEL + 48];

			k = put(msg, 0, sizeof(msg), "ディスク「");
			k = put(msg, k, sizeof(msg), (CONST char *)name);
			(void)put(msg, k, sizeof(msg), "」を接続しました");
			(void)wm_msg_put((CONST char *)msg);
		}
	}

    done:
	if ( names != NULL ) Kfree(names);
	if ( rows != NULL ) Kfree(rows);
	if ( kinds != NULL ) Kfree(kinds);
	if ( pr != NULL ) Kfree(pr);
	wm_update();
}

/* ---------------------------------------------------------------- 切り離し */

/* Whether the volume of a link's object is one that may be taken away: grey in the menu otherwise */
EXPORT BOOL dt_disk_detachable( CONST T_VOBJ *v )
{
	UB		path[DK_PATH];
	T_OBFVREF	ref;

	return (BOOL)( v != NULL && knl_obfile_path_of(&v->target, path, sizeof(path)) >= E_OK
		       && knl_obfile_vol_ref((CONST char *)path, &ref) >= E_OK && !ref.system );
}

/*
 * A volume taken away: not while an object of it is shown in a window
 * or used by a program, nor the system's. 'what' is the word the
 * messages use (切り離し, 取り外し), 'fail' what is said when it
 * cannot go for another reason. Answers E_OK when it went.
 */
LOCAL ER detach_vol( DTWIN *d, CONST char *path, CONST UB *name, CONST char *what,
		      CONST char *fail )
{
	UB	msg[OB_VOL_LABEL + 96], how[64];
	INT	i, k;
	ER	er;

	/* an object of it shown in a window keeps it */
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN	*w = dt_win_at(i);

		if ( w != NULL && w->used && !w->sealed && w->tool == DT_TOOL_NONE
		  && knl_obfile_on(&w->id, path) ) {
			k = put(msg, 0, sizeof(msg), "ディスク「");
			k = put(msg, k, sizeof(msg), (CONST char *)name);
			(void)put(msg, k, sizeof(msg), "」の実身が開いています。");
			k = put(how, 0, sizeof(how), "閉じてから");
			k = put(how, k, sizeof(how), what);
			(void)put(how, k, sizeof(how), "てください");
			tm_printf((UB *)"disk: detach %s refused: a window is open\n", path);
			dt_tell(d, (CONST char *)msg, (CONST char *)how);
			return E_BUSY;
		}
	}
	er = ob_det_vol(path);
	if ( er == E_BUSY ) {
		k = put(msg, 0, sizeof(msg), "ディスク「");
		k = put(msg, k, sizeof(msg), (CONST char *)name);
		(void)put(msg, k, sizeof(msg), "」の実身を使っているプログラムがあります。");
		k = put(how, 0, sizeof(how), "終わってから");
		k = put(how, k, sizeof(how), what);
		(void)put(how, k, sizeof(how), "てください");
		tm_printf((UB *)"disk: detach %s refused: in use\n", path);
		dt_tell(d, (CONST char *)msg, (CONST char *)how);
		return er;
	}
	if ( er < E_OK ) {
		tm_printf((UB *)"disk: detach %s failed %d\n", path, (INT)er);
		dt_tell(d, fail, NULL);
		return er;
	}
	tm_printf((UB *)"disk: detached %s\n", path);
	om_store_recheck();
	dk_volser = knl_obfile_serial();
	note_windows(TRUE);
	dt_draw_all();
	return E_OK;
}

EXPORT void dt_disk_detach( DTWIN *d, CONST T_VOBJ *v )
{
	UB		path[DK_PATH], name[OB_VOL_LABEL + 16], msg[OB_VOL_LABEL + 96];
	T_OBFVREF	ref;
	INT		k;

	if ( v == NULL || knl_obfile_path_of(&v->target, path, sizeof(path)) < E_OK
	  || knl_obfile_vol_ref((CONST char *)path, &ref) < E_OK ) {
		dt_tell(d, "そのディスクは接続されていません", NULL);
		return;
	}
	disk_name(ref.label, (CONST char *)path, name, sizeof(name));
	if ( ref.system ) {
		dt_tell(d, "システムのディスクは切り離せません", NULL);
		return;
	}
	if ( detach_vol(d, (CONST char *)path, name, "切り離し", "ディスクを切り離せませんでした") < E_OK ) {
		return;
	}
	if ( ref.native ) {
		keep_vol(&ref.vuuid, &ref.top, FALSE);	/* not attached again by itself */
	}
	k = put(msg, 0, sizeof(msg), "ディスク「");
	k = put(msg, k, sizeof(msg), (CONST char *)name);
	(void)put(msg, k, sizeof(msg), "」を切り離しました。取り出せます");
	(void)wm_msg_put((CONST char *)msg);
}

/* ---------------------------------------------------------------- 取り外し */

/*
 * A link about to be deleted from the first window. When it is a
 * disk's link -- the top object of a volume attached, or of one
 * remembered as attached -- deleting it takes the disk away (取り外し):
 * the volume is detached and forgotten, so that it is not attached again
 * at the next start. FALSE when the volume cannot be taken away now; the
 * link is then not deleted.
 */
EXPORT BOOL dt_disk_on_delete( DTWIN *d, CONST T_VOBJ *v )
{
	UB		path[DK_PATH], name[OB_VOL_LABEL + 16], msg[OB_VOL_LABEL + 96];
	T_OBFVREF	ref;
	TS_UUID		*kv, *kt;
	INT		nk = 0, i, k;

	if ( v == NULL || d == NULL || d != dt_first_win() ) {
		return TRUE;
	}
	if ( knl_obfile_path_of(&v->target, path, sizeof(path)) >= E_OK ) {
		if ( knl_obfile_vol_ref((CONST char *)path, &ref) < E_OK || ref.system || !ref.native
		  || ts_uuid_cmp(&ref.top, &v->target) != 0 ) {
			return TRUE;		/* an object's link, not a disk's */
		}
		disk_name(ref.label, (CONST char *)path, name, sizeof(name));
		if ( detach_vol(d, (CONST char *)path, name, "取り外し", "ディスクを取り外せませんでした") < E_OK ) {
			return FALSE;
		}
		keep_vol(&ref.vuuid, &ref.top, FALSE);
		k = put(msg, 0, sizeof(msg), "ディスク「");
		k = put(msg, k, sizeof(msg), (CONST char *)name);
		(void)put(msg, k, sizeof(msg), "」を取り外しました。取り出せます");
		(void)wm_msg_put((CONST char *)msg);
		tm_printf((UB *)"disk: removed %s\n", (CONST char *)path);
		return TRUE;
	}
	/* the link of a disk not attached now: forgotten as well */
	kv = kept_vols(&nk, &kt);
	for ( i = 0; kv != NULL && i < nk; i++ ) {
		if ( ts_uuid_cmp(&kt[i], &v->target) == 0
		  || ( !zero_uuid(&v->tvol) && ts_uuid_cmp(&kv[i], &v->tvol) == 0 ) ) {
			keep_vol(&kv[i], NULL, FALSE);
			tm_printf((UB *)"disk: removed a disk not attached\n");
			break;
		}
	}
	if ( kv != NULL ) Kfree(kv);
	return TRUE;
}

/* ---------------------------------------------------------------- 状態表示 */

/* How full the disk of a link's object is (the system's, away from a link) */
EXPORT void dt_disk_status( DTWIN *d, CONST T_VOBJ *v )
{
	T_OBVOL		vol;
	T_OBFVREF	ref;
	T_WMPANEL	*def;
	UB		path[DK_PATH], name[OB_VOL_LABEL + 16], t[WM_LABEL_MAX];
	CONST char	*lab[6] = { "全体容量:", "使用容量:", "空き容量:", "使用比率:",
				    "使用実身数:", "くず実身数:" };
	UD		total, used;
	INT		gc = 0, i, k, pid, w = 360, h = 230;
	ID		tv;

	if ( d == NULL ) {
		return;
	}
	if ( ob_ref_vol(( v != NULL ) ? &v->target : NULL, &vol) < E_OK ) {
		dt_tell(d, "その実身のディスクは接続されていません", NULL);
		return;
	}
	tv = ( v != NULL ) ? knl_obfile_vol_of(&v->target) : knl_obfile_first_vol();
	if ( tv > 0 ) {
		TS_UUID	buf[32], from;
		INT	got;

		/* the garbage, counted a piece at a time */
		for (;;) {
			got = 0;
			if ( ts_lst_gc(tv, ( gc == 0 ) ? NULL : &from, buf, 32, &got) < E_OK || got <= 0 ) {
				break;
			}
			gc += got;
			from = buf[got - 1];
			if ( got < 32 ) break;
		}
	}
	name[0] = 0;
	if ( ( v != NULL && knl_obfile_path_of(&v->target, path, sizeof(path)) >= E_OK )
	  || ( v == NULL && knl_obfile_vols(path, sizeof(path), 1) == 1 ) ) {
		if ( knl_obfile_vol_ref((CONST char *)path, &ref) >= E_OK ) {
			disk_name(ref.label, (CONST char *)path, name, sizeof(name));
		}
	}
	total = vol.blocks * vol.bsize;
	used = ( vol.blocks - vol.bfree ) * vol.bsize;
	tm_printf((UB *)"disk: status \"%s\" blocks %d free %d objects %d garbage %d\n",
		  (CONST char *)name, (INT)vol.blocks, (INT)vol.bfree, vol.nobj, gc);
	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return;
	}
	k = put(t, 0, sizeof(t), "ディスク状態: ");
	(void)put(t, k, sizeof(t), (CONST char *)name);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, w - 28, 22, (CONST char *)t);
	for ( i = 0; i < 6; i++ ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 24, 32 + 24 * i, 120, 22, lab[i]);
		switch ( i ) {
		case 0:	(void)put_size(t, 0, sizeof(t), total); break;
		case 1:	(void)put_size(t, 0, sizeof(t), used); break;
		case 2:	(void)put_size(t, 0, sizeof(t), total - used); break;
		case 3:	k = put_num(t, 0, sizeof(t), ( total > 0 ) ? used * 100 / total : 0);
			(void)put(t, k, sizeof(t), "%"); break;
		case 4:	(void)put_num(t, 0, sizeof(t), (UD)vol.nobj); break;
		default: (void)put_num(t, 0, sizeof(t), (UD)gc); break;
		}
		dt_pn_add(def, WM_PT_LABEL, 0, 150, 32 + 24 * i, w - 170, 22, (CONST char *)t);
	}
	{
		T_WMPART	*pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, w - 116, h - 36, 100, 26, "終了");

		if ( pt != NULL ) pt->answer = WM_ANS_OK;
	}
	(void)dt_pn_run(d, def, &pid);
	dt_pn_done(d, pid);
	wm_update();
}

/* ---------------------------------------------------------------- 区画作成 */

/* The disk the system volume is on ("sda" of "sda1"), or "" */
LOCAL void sys_disk( UB *out, INT max )
{
	INT	n = 0;

	out[0] = 0;
#ifdef CNF_OB_SYSVOL
	{
		CONST char	*s = CNF_OB_SYSVOL;
		INT		k;

		while ( s[n] != '\0' && n < max - 1 ) n++;
		for ( k = n; k > 0 && s[k - 1] >= '0' && s[k - 1] <= '9'; k-- ) ;
		knl_memcpy(out, s, k);
		out[k] = 0;
	}
#endif
}

LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	return (BOOL)( knl_strcmp((CONST char *)a, (CONST char *)b) == 0 );
}

/* The number in a number box, in megabytes; -1 when it cannot be read */
LOCAL D box_mb( INT pid, INT num )
{
	INT	v = 0;

	return ( wm_panel_get(pid, num, &v) >= E_OK ) ? (D)v : -1;
}

/* A box of a number, set to `v` */
LOCAL T_WMPART *num_box( T_WMPANEL *def, INT num, INT l, INT t, INT w, UD v )
{
	T_WMPART	*pt = dt_pn_add(def, WM_PT_NUMBOX, num, l, t, w, 26, NULL);
	INT		k;

	if ( pt != NULL ) {
		k = put_num(pt->text, 0, WM_LABEL_MAX, v);
		pt->caret = k;
		pt->value = (INT)v;
	}
	return pt;
}

/* A free stretch of a disk, as the panel of 区画作成 offers it */
typedef struct {
	UB	disk[OB_DV_NAME];
	UD	first, last;
	BOOL	fresh;			/* the disk has no table yet */
	BOOL	system;			/* the disk the system volume is on */
} DKFREE;

#define DK_MIN_MB	8		/* a stretch smaller than this is not offered */

/*
 * 区画作成: the free stretches of the disks, a size and a label, and a
 * TSFS partition made in the stretch chosen, a volume made on it and
 * attached, and remembered as attached (as 接続 does).
 */
EXPORT void dt_disk_create( DTWIN *d )
{
	UB		*names, (*rows)[WM_LABEL_MAX] = NULL, sd[OB_DV_NAME], label[OB_VOL_LABEL];
	UB		part[OB_DV_NAME], name[OB_VOL_LABEL + 16], msg[OB_VOL_LABEL + 96];
	DKFREE		*fr = NULL;
	T_GPTFREE	*g = NULL;
	INT		groom = 0, rroom = 0;
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		n, i, j, k, m = 0, pid, w = 560, h, top, pick = -1;
	UINT		ans;
	D		mb = 0;
	ER		er;

	if ( d == NULL ) {
		d = dt_first_win();
	}
	if ( d == NULL ) {
		return;
	}
	names = disks_all(&n);
	if ( names == NULL ) {
		goto done;
	}
	sys_disk(sd, sizeof(sd));
	for ( i = 0; i < n; i++ ) {
		CONST char	*dev = (CONST char *)( names + i * OB_DV_NAME );
		UD		nsect = 0;
		BOOL		fresh = FALSE;
		INT		c;

		k = (INT)knl_strlen(dev);
		if ( k == 0 || ( dev[k - 1] >= '0' && dev[k - 1] <= '9' ) ) {
			continue;		/* a partition: the disks are looked at whole */
		}
		/* every free stretch of the disk: a list that came back full is asked for again in more room */
		for ( c = groom; ; ) {
			if ( g == NULL || c >= groom ) {
				T_GPTFREE	*ng;

				groom = ( groom > 0 ) ? groom * 2 : 8;
				ng = (T_GPTFREE *)Kmalloc(sizeof(T_GPTFREE) * (SZ)groom);
				if ( ng == NULL ) {
					c = 0;
					break;
				}
				if ( g != NULL ) Kfree(g);
				g = ng;
			}
			c = knl_gpt_free(dev, g, groom, &nsect, &fresh);
			if ( c < groom ) break;
		}
		/* a row for each, the rows grown to hold them */
		if ( c > 0 && m + c > rroom ) {
			INT		nr = ( rroom > 0 ) ? rroom : 16;
			UB		(*nrw)[WM_LABEL_MAX];
			DKFREE		*nfr;

			while ( nr < m + c ) nr *= 2;
			nrw = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * (SZ)nr);
			nfr = (DKFREE *)Kmalloc(sizeof(DKFREE) * (SZ)nr);
			if ( nrw == NULL || nfr == NULL ) {
				if ( nrw != NULL ) Kfree(nrw);
				if ( nfr != NULL ) Kfree(nfr);
				c = 0;
			} else {
				if ( rows != NULL ) {
					knl_memcpy(nrw, rows, (INT)( sizeof(UB) * WM_LABEL_MAX * (SZ)m ));
					Kfree(rows);
				}
				if ( fr != NULL ) {
					knl_memcpy(nfr, fr, (INT)( sizeof(DKFREE) * (SZ)m ));
					Kfree(fr);
				}
				rows = nrw;
				fr = nfr;
				rroom = nr;
			}
		}
		for ( j = 0; j < c; j++ ) {
			UD	bytes = ( g[j].last - g[j].first + 1 ) * 512;

			if ( bytes < (UD)DK_MIN_MB << 20 ) continue;
			knl_memcpy(fr[m].disk, dev, OB_DV_NAME);
			fr[m].first = g[j].first;
			fr[m].last = g[j].last;
			fr[m].fresh = fresh;
			fr[m].system = same_name((CONST UB *)dev, sd);
			k = put(rows[m], 0, WM_LABEL_MAX, dev);
			k = put(rows[m], k, WM_LABEL_MAX, "  空き ");
			k = put_size(rows[m], k, WM_LABEL_MAX, bytes);
			k = put(rows[m], k, WM_LABEL_MAX, "(");
			k = put_size(rows[m], k, WM_LABEL_MAX, g[j].first * 512);
			k = put(rows[m], k, WM_LABEL_MAX, "の位置から)");
			if ( fresh ) {
				k = put(rows[m], k, WM_LABEL_MAX, " 区画表なし");
			}
			if ( fr[m].system ) {
				(void)put(rows[m], k, WM_LABEL_MAX, " システムのディスク");
			}
			m++;
		}
	}
	tm_printf((UB *)"disk: create offers %d\n", m);
	if ( m == 0 ) {
		dt_tell(d, "区画を作れる空き領域がありません", NULL);
		goto done;
	}
	h = 176 + 26 * m;
	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		goto done;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, w - 28, 22, "区画を作る空き領域:");
	for ( i = 0; i < m; i++ ) {
		pt = dt_pn_add(def, WM_PT_CHOICE, PN_DISK + i, 24, 30 + 26 * i, w - 48, 22,
			       (CONST char *)rows[i]);
		if ( pt != NULL ) {
			pt->group = PN_DISK;
			pt->value = ( i == 0 ) ? 1 : 0;
		}
	}
	top = 38 + 26 * m;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, top + 2, 90, 22, "大きさ:");
	(void)num_box(def, PN_SIZE, 104, top, 110, 0);
	dt_pn_add(def, WM_PT_LABEL, 0, 222, top + 2, w - 236, 22, "MB(0は空き領域の全体)");
	top += 34;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, top + 2, 90, 22, "ラベル:");
	pt = dt_pn_add(def, WM_PT_BOX | P_JP, PN_LABEL, 104, top, w - 118, 26, NULL);
	if ( pt != NULL ) {
		k = put(pt->text, 0, WM_LABEL_MAX, "ディスク");
		pt->caret = k;
	}
	dt_pn_buttons(def, "作成");
	ans = dt_pn_run(d, def, &pid);
	label[0] = 0;
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		for ( i = 0; i < m; i++ ) {
			if ( dt_pn_value(pid, PN_DISK + i) != 0 ) pick = i;
		}
		mb = box_mb(pid, PN_SIZE);
		if ( wm_panel_text(pid, PN_LABEL, label, sizeof(label)) < E_OK ) label[0] = 0;
	}
	dt_pn_done(d, pid);
	wm_update();
	if ( pick < 0 ) {
		tm_printf((UB *)"disk: create cancelled\n");
		goto done;
	}
	if ( mb < 0 || (UD)mb * 2048 > fr[pick].last - fr[pick].first + 1 || ( mb > 0 && mb < DK_MIN_MB ) ) {
		tm_printf((UB *)"disk: create %s refused: %d MB\n", fr[pick].disk, (INT)mb);
		dt_tell(d, "その大きさの区画は作れません", "空き領域に収まる、8MB以上の大きさにしてください");
		goto done;
	}
	if ( label[0] == 0 ) {
		(void)put(label, 0, sizeof(label), "ディスク");
	}
	if ( fr[pick].system && dt_admin_self("ディスク操作", (CONST UB *)"システムのディスクに区画を作る") < E_OK ) {
		tm_printf((UB *)"disk: create %s refused: not the administrator\n", fr[pick].disk);
		goto done;
	}
	er = knl_tsfs_part_make((CONST char *)fr[pick].disk, fr[pick].first, (UD)mb << 20,
				(CONST char *)label, part, sizeof(part));
	tm_printf((UB *)"disk: created %s %d MB \"%s\" (%d)\n", part,
		  (INT)( ( mb > 0 ) ? mb : (D)( ( fr[pick].last - fr[pick].first + 1 ) / 2048 ) ),
		  (CONST char *)label, (INT)er);
	if ( er < E_OK ) {
		dt_tell(d, "区画を作れませんでした", NULL);
		goto done;
	}
	/* the volume made is the person's, as one 接続 attached */
	er = attach_dev((CONST char *)part, TRUE, name, sizeof(name));
	if ( er < E_OK ) {
		dt_tell(d, "区画は作りましたが、接続できませんでした", NULL);
		goto done;
	}
	k = put(msg, 0, sizeof(msg), "区画「");
	k = put(msg, k, sizeof(msg), (CONST char *)name);
	(void)put(msg, k, sizeof(msg), "」を作り、接続しました");
	(void)wm_msg_put((CONST char *)msg);

    done:
	if ( names != NULL ) Kfree(names);
	if ( rows != NULL ) Kfree(rows);
	if ( fr != NULL ) Kfree(fr);
	if ( g != NULL ) Kfree(g);
	wm_update();
}

/* ---------------------------------------------------------------- 拡張 */

/*
 * The volume 拡張 works on: that of the link's object, the system's away
 * from a link. FALSE when it is not a native volume.
 */
LOCAL BOOL grow_target( CONST T_VOBJ *v, UB *path, INT max, T_OBFVREF *ref )
{
	if ( ( v != NULL && knl_obfile_path_of(&v->target, path, max) < E_OK )
	  || ( v == NULL && knl_obfile_vols(path, max, 1) != 1 ) ) {
		return FALSE;
	}
	return (BOOL)( knl_obfile_vol_ref((CONST char *)path, ref) >= E_OK && ref->native );
}

/* Whether 拡張 can be chosen on a link: grey in the menu otherwise */
EXPORT BOOL dt_disk_growable( CONST T_VOBJ *v )
{
	UB		path[DK_PATH];
	T_OBFVREF	ref;

	return grow_target(v, path, sizeof(path), &ref);
}

/*
 * 拡張: the partition of the volume of the link's object (the system's
 * away from a link) and the volume on it made larger, into the free
 * stretch right after the partition, to the size typed (the whole
 * stretch at first).
 */
EXPORT void dt_disk_grow( DTWIN *d, CONST T_VOBJ *v )
{
	UB		path[DK_PATH], name[OB_VOL_LABEL + 16], t[WM_LABEL_MAX], msg[OB_VOL_LABEL + 96];
	T_OBFVREF	ref;
	T_GPTPART	gp;
	T_OBVOL		vol;
	T_WMPANEL	*def;
	UD		cur, most, from = 0, to = 0;
	INT		k, pid, w = 480, h = 206;
	UINT		ans;
	D		mb = -1;
	ER		er;

	if ( d == NULL ) {
		d = dt_first_win();
	}
	if ( d == NULL ) {
		return;
	}
	if ( !grow_target(v, path, sizeof(path), &ref) ) {
		dt_tell(d, "このディスクは広げられません", NULL);
		return;
	}
	disk_name(ref.label, (CONST char *)path, name, sizeof(name));
	if ( knl_gpt_part((CONST char *)path, &gp) < E_OK || !gp.tsfs ) {
		tm_printf((UB *)"disk: grow %s: not a partition of a GPT\n", path);
		dt_tell(d, "このディスクは広げられません", "区画表(GPT)のTSFSの区画ではありません");
		return;
	}
	cur = ( gp.last - gp.first + 1 ) * 512;
	most = ( gp.grow_last - gp.first + 1 ) * 512;
	if ( ob_ref_vol(( v != NULL ) ? &v->target : NULL, &vol) >= E_OK && vol.blocks * vol.bsize < cur ) {
		cur = vol.blocks * vol.bsize;	/* a partition larger than its volume: that much to take in */
	}
	tm_printf((UB *)"disk: grow %s offers %d MB to %d MB\n", path, (INT)( cur >> 20 ), (INT)( most >> 20 ));
	if ( most <= cur ) {
		dt_tell(d, "広げられる空き領域がありません", "区画のすぐ後ろに空き領域が要ります");
		return;
	}
	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return;
	}
	k = put(t, 0, sizeof(t), "ディスク「");
	k = put(t, k, sizeof(t), (CONST char *)name);
	k = put(t, k, sizeof(t), "」(");
	k = put(t, k, sizeof(t), (CONST char *)path);
	(void)put(t, k, sizeof(t), ")を広げます");
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, w - 28, 22, (CONST char *)t);
	dt_pn_add(def, WM_PT_LABEL, 0, 24, 32, 150, 22, "今の大きさ:");
	k = put_num(t, 0, sizeof(t), cur >> 20);
	(void)put(t, k, sizeof(t), "MB");
	dt_pn_add(def, WM_PT_LABEL, 0, 180, 32, w - 194, 22, (CONST char *)t);
	dt_pn_add(def, WM_PT_LABEL, 0, 24, 58, 150, 22, "広げられる大きさ:");
	k = put(t, 0, sizeof(t), "最大 ");
	k = put_num(t, k, sizeof(t), most >> 20);
	(void)put(t, k, sizeof(t), "MB");
	dt_pn_add(def, WM_PT_LABEL, 0, 180, 58, w - 194, 22, (CONST char *)t);
	dt_pn_add(def, WM_PT_LABEL, 0, 24, 94, 150, 22, "新しい大きさ:");
	(void)num_box(def, PN_SIZE, 180, 92, 120, most >> 20);
	dt_pn_add(def, WM_PT_LABEL, 0, 308, 94, 60, 22, "MB");
	dt_pn_buttons(def, "拡張");
	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		mb = box_mb(pid, PN_SIZE);
	}
	dt_pn_done(d, pid);
	wm_update();
	if ( ans != WM_ANS_OK ) {
		tm_printf((UB *)"disk: grow cancelled\n");
		return;
	}
	if ( mb < 0 || ( (UD)mb << 20 ) <= cur || ( (UD)mb << 20 ) > most ) {
		tm_printf((UB *)"disk: grow %s refused: %d MB\n", path, (INT)mb);
		dt_tell(d, "その大きさには広げられません", "今の大きさより大きく、最大までの大きさにしてください");
		return;
	}
	if ( ref.system && dt_admin_self("ディスク操作", (CONST UB *)"システムのディスクを広げる") < E_OK ) {
		tm_printf((UB *)"disk: grow %s refused: not the administrator\n", path);
		return;
	}
	er = knl_tsfs_part_grow((CONST char *)path, (UD)mb << 20, &from, &to);
	tm_printf((UB *)"disk: grown %s %d MB -> %d MB (%d)\n", path, (INT)( from >> 20 ), (INT)( to >> 20 ), (INT)er);
	if ( er < E_OK ) {
		dt_tell(d, "ディスクを広げられませんでした", NULL);
		return;
	}
	k = put(msg, 0, sizeof(msg), "ディスク「");
	k = put(msg, k, sizeof(msg), (CONST char *)name);
	k = put(msg, k, sizeof(msg), "」を");
	k = put_num(msg, k, sizeof(msg), from >> 20);
	k = put(msg, k, sizeof(msg), "MBから");
	k = put_num(msg, k, sizeof(msg), to >> 20);
	(void)put(msg, k, sizeof(msg), "MBに広げました");
	(void)wm_msg_put((CONST char *)msg);
	dt_draw_all();
}

/* ---------------------------------------------------------------- the top of a volume */

/*
 * The object a link to something saved on a volume goes in: the
 * volume's top object, or the first window for the system's volume and
 * one that has none.
 */
EXPORT BOOL dt_disk_top( CONST char *path, TS_UUID *p_top )
{
	T_OBFVREF	ref;

	if ( path == NULL || knl_obfile_vol_ref(path, &ref) < E_OK || ref.system
	  || zero_uuid(&ref.top) || knl_obfile_vol_of(&ref.top) <= 0 ) {
		return FALSE;
	}
	*p_top = ref.top;
	return TRUE;
}

/* ---------------------------------------------------------------- a link whose object is not there */

/*
 * A link opened whose object cannot be found (a 虚身): which volume it is
 * on, as its id says (design 8.3), is told, and TRUE answered; FALSE for
 * a link whose object is there, to be opened as ever. A link to an object
 * of another machine is not opened from here.
 */
EXPORT BOOL dt_disk_absent( DTWIN *d, CONST T_VOBJ *v )
{
	UB	name[OB_VOL_LABEL + 16], msg[OB_VOL_LABEL + 96];
	TS_UUID	mine;
	INT	k;

	if ( v == NULL ) {
		return FALSE;
	}
	if ( d == NULL ) {
		d = dt_first_win();
	}
	if ( !zero_uuid(&v->tmach) && ( knl_obfile_machine(&mine) < E_OK || ts_uuid_cmp(&mine, &v->tmach) != 0 ) ) {
		tm_printf((UB *)"disk: absent: another machine's\n");
		dt_tell(d, "ほかのマシンの実身です。", "このマシンからは開けません");
		return TRUE;
	}
	if ( dt_reachable(v) ) {
		return FALSE;
	}
	if ( !zero_uuid(&v->tvol) && !knl_obfile_vuuid_on(&v->tvol) ) {
		/* the name the volume had when it was attached; else what the link calls it */
		if ( knl_obfile_vol_name(&v->tvol, name, sizeof(name)) < E_OK ) {
			char	us[TS_UUID_STRLEN + 1];

			(void)ts_uuid_to_str(&v->tvol, us, sizeof(us));
			us[8] = 0;
			k = put(name, 0, sizeof(name), us);
			if ( v->name[0] != 0 ) {
				k = put(name, k, sizeof(name), "、");
				(void)put(name, k, sizeof(name), (CONST char *)v->name);
			}
		}
		k = put(msg, 0, sizeof(msg), "ボリューム「");
		k = put(msg, k, sizeof(msg), (CONST char *)name);
		(void)put(msg, k, sizeof(msg), "」が接続されていません");
		tm_printf((UB *)"disk: absent: volume \"%s\" not attached\n", (CONST char *)name);
		dt_tell(d, (CONST char *)msg, "ディスク操作の接続で接続してください");
		return TRUE;
	}
	tm_printf((UB *)"disk: absent: not found\n");
	dt_tell(d, "実身が見つかりません。", "そのボリュームが接続されていないか、実身が削除されています");
	return TRUE;
}
