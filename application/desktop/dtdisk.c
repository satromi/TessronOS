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
 *	What the menu ディスク操作 offers is done by the program ディスク操作
 *	(application/diskop), started with the item chosen and the link it
 *	was chosen on (dt_disk_menu): its panels are its own, and it asks
 *	the desktop, which alone knows the disks, the volumes and the windows
 *	open on them, for each step (DT_RQ_DISK, dt_disk_req). A partition of
 *	TSFS is attached as a volume of objects with 接続, and the link to
 *	the object the volume opens with -- its top, made when it has none --
 *	is put in the first window: the disk's link. 切り離し takes a volume
 *	away again, unless an object of it is open in a window or by a
 *	program, or it is being checked (ディスク修復). The links to its
 *	objects stay, drawn pale (虚身), and come back as they were when it
 *	is attached again. 状態表示 tells how full the disk of an object is.
 *	区画作成 makes a TSFS partition in a free stretch of a disk, a volume
 *	on it, and attaches it; 拡張 makes the partition of a volume, and the
 *	volume, larger into the free stretch after it (gpt.h). Making and
 *	making larger are an administrator's (design 8.7), and so are
 *	attaching and taking away but on a USB medium the system uses
 *	nothing of (knl_obdev_medium_person), which is the person's: a
 *	program that is not one is asked the administrator's password for.
 *	Another machine's system volume (a TessronOS disk put in, knl_obfile_
 *	foreign) gets a top holding a link to that machine's first window,
 *	by its alias (design 8.3).
 *
 *	The volumes the person attached are remembered by their UUIDs in
 *	the device box (knl_obdev_vols_put) until they are taken away with
 *	切り離し or by deleting the disk's link in the first window
 *	(取り外し). Such a volume is attached again whenever its disk is
 *	found: at the start, and when a disk comes.
 *
 *	What happens without anyone asking is followed here as well. When a
 *	disk is taken out, its volumes are gone (the kernel's), the windows
 *	of their objects -- the programs' -- are asked to close, each closed
 *	said with a message, and the links are drawn again. A window with
 *	changes not saved is not closed silently: its program hands what it
 *	holds to the desktop (DT_DISK_RESCUE), and the person is asked to
 *	save it to another volume, to have it saved when the disk is put
 *	back, or to throw it away.
 *
 *	The disk of the system volume taken out is followed the same way,
 *	except that the first window stays: its program keeps the record it
 *	shows, the message line says the system's disk went, and what cannot
 *	be read without it fails as it would for any disk taken out. The
 *	same disk put back is attached again by itself.
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
#include <ts/dtreq.h>
#include <ts/tsfsck.h>
#include <ts/fquit.h>
#include <ts/sysdef.h>
#include <ts/proc.h>
#include "desktop.h"

#define DK_WAIT		16		/* records waiting for their disk to be put back */
#define DK_PATH		64		/* bytes of a volume's path kept here */
#define DK_TO		8		/* other volumes offered to save to */

#define PN_TO		40		/* where changes of a window whose disk went are to go */

/* A record whose disk went while it had changes, kept until the disk is back */
typedef struct {
	BOOL	used;
	TS_UUID	vuuid;			/* the volume it is on */
	TS_UUID	id;
	INT	recno;
	UB	*xml;			/* the record as it was being changed */
	SZ	size;
	UB	*name;			/* its object's, whole (Kmalloc); NULL for want of memory */
} DKWAIT;

LOCAL DKWAIT	dk_wait[DK_WAIT];
LOCAL UW	dk_volser = 0;		/* knl_obfile_serial as last followed */
LOCAL UW	dk_devser = 0;		/* knl_obdev_serial */
LOCAL BOOL	dk_started = FALSE;
LOCAL TS_UUID	dk_sysvu;		/* the system volume, the first attached at the start */
LOCAL BOOL	dk_has_sysvu = FALSE;
LOCAL BOOL	dk_sysgone = FALSE;	/* its disk is out */

/*
 * The programs' windows, what each shows, whether that was on a volume
 * and which, as last looked at. A window is known by its number and its
 * object together: a window closed and another opened in its place
 * between two looks may be given the same number, and must not be taken
 * for the one before (and closed for a volume it never was on). One
 * asked to close because its disk went is said closed once it is gone.
 */
#define DK_WINS		64

typedef struct {
	INT	wid;
	TS_UUID	id;
	BOOL	onvol;
	TS_UUID	vu;
	BOOL	asked;			/* asked to close: its disk went */
	UB	*name;			/* what it shows, whole, as named when first seen (Kmalloc) */
} DKWIN;

/* A name kept for when its object can no longer be read: "実身" when it has none or there is no memory */
#define dk_name(p)	( ( (p) != NULL ) ? (CONST char *)(p) : "実身" )

LOCAL DKWIN	dk_win[DK_WINS];
LOCAL INT	dk_nwin = 0;

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

/* a, b and c one after another, in memory of their own (NULL for want of it) */
LOCAL UB *text_cat3( CONST char *a, CONST char *b, CONST char *c )
{
	SZ	na = (SZ)knl_strlen(a), nb = (SZ)knl_strlen(b), nc = (SZ)knl_strlen(c);
	UB	*t = (UB *)Kmalloc(na + nb + nc + 1);

	if ( t == NULL ) {
		tm_printf((UB *)"disk: a text of %d bytes not made: no memory\n", (INT)( na + nb + nc ));
		return NULL;
	}
	knl_memcpy(t, a, (INT)na);
	knl_memcpy(t + na, b, (INT)nb);
	knl_memcpy(t + na + nb, c, (INT)nc);
	t[na + nb + nc] = 0;
	return t;
}

/*
 * What a disk is called, whole, in memory of its own: its label (the
 * name of its volume's top object, design 7.6), or its device's name.
 * NULL for want of memory, which is said on the console.
 */
LOCAL UB *disk_name( CONST char *dev )
{
	UB	*lab = NULL, *t;
	SZ	size = 0, asz = 0;

	for ( ;; ) {
		if ( knl_obfile_vol_label(dev, lab, size, &asz) < E_OK ) {
			if ( lab != NULL ) Kfree(lab);
			lab = NULL;
			break;
		}
		if ( lab != NULL && asz <= size ) {
			break;
		}
		if ( lab != NULL ) Kfree(lab);
		size = asz;
		lab = (UB *)Kmalloc(size);
		if ( lab == NULL ) {
			break;
		}
	}
	if ( lab != NULL && lab[0] != 0 ) {
		return lab;
	}
	if ( lab != NULL ) Kfree(lab);
	t = text_cat3("ディスク ", dev, "");
	return t;
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

/* Whether the first window holds a link to the object, as its record is saved */
LOCAL BOOL on_desktop( CONST TS_UUID *u )
{
	TS_UUID	cab;
	T_TAD	*rec = NULL;
	T_VOBJ	*v;
	UB	*x;
	SZ	size = 0;
	INT	i, n;
	BOOL	hit = FALSE;

	/* as stored: the record is the first window's program's, which writes it itself */
	if ( ts_str_to_uuid(SYSDEF_CABINET, &cab) < E_OK || ( x = om_obj_record(&cab, 0, &size) ) == NULL ) {
		return FALSE;
	}
	if ( tad_parse(x, size, NULL, &rec) < E_OK ) rec = NULL;
	Kfree(x);
	if ( rec == NULL || ( v = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ)) ) == NULL ) {
		if ( rec != NULL ) tad_free(rec);
		return FALSE;
	}
	n = tad_lnk_count(rec);
	for ( i = 0; i < n && !hit; i++ ) {
		hit = (BOOL)( tad_lnk_get(rec, i, v) >= E_OK && ts_uuid_cmp(&v->target, u) == 0 );
	}
	tad_vobj_clear(v);
	Kfree(v);
	tad_free(rec);
	return hit;
}

/* ---------------------------------------------------------------- attaching */

LOCAL void wait_done( void );

/*
 * The top just made on another machine's system volume (design 8.3): a
 * link to that machine's first window, which is on the same volume, so
 * that the disk opens on what that system shows. The link says no
 * volume, as a link to an object of the record's own volume.
 */
LOCAL void foreign_top( CONST char *dev, CONST TS_UUID *top )
{
	TS_UUID	cab, a;
	UB	*nm;
	ER	er = E_NOEXS;

	/* that machine's first window, by its alias: this system's first window has its UUID */
	if ( ts_str_to_uuid(SYSDEF_CABINET, &cab) >= E_OK && knl_obfile_alias(dev, &cab, &a) >= E_OK ) {
		nm = knl_ob_name(&a, NULL, NULL);
		er = dt_place_in(top, &a, ( nm != NULL ) ? nm : (CONST UB *)"初期ウインドウ");
		if ( nm != NULL ) Kfree(nm);
	}
	tm_printf((UB *)"disk: another machine's system: its first window put in the top (%d)\n", (INT)er);
}

/*
 * A partition of TSFS attached by its device's name. The volume's top
 * object is made when it has none: a figure, named as the disk is, the
 * way a cabinet is. 'place' puts a link to it in the first window unless
 * one is there. Answers E_OBJ when the same volume is attached through
 * another device. What the disk is called, whole, goes to *p_name in
 * memory of its own for the caller to free (NULL for want of it).
 * 'made': the volume was just made (区画作成), its top below the object
 * layer, and the top is given the person's protection, as one the
 * desktop makes gets it (ob_cre_obj, knl_ob_make_for_person).
 */
LOCAL ER attach_dev( CONST char *dev, BOOL place, BOOL made, UB **p_name )
{
	T_OBFVREF	*ref;
	TS_UUID		top;
	UB		*name;
	ER		er;

	if ( p_name != NULL ) {
		*p_name = NULL;
	}

	er = ob_att_vol(dev, TSFS_STORE_BLK);
	if ( er < E_OK ) {
		tm_printf((UB *)"disk: attach %s failed %d\n", dev, (INT)er);
		return er;
	}
	if ( made ) {
		er = knl_obfile_top_prot(dev, knl_ob_session_crd());
		if ( er < E_OK ) {
			tm_printf((UB *)"disk: the top of %s not given the person's protection (%d)\n", dev, (INT)er);
		}
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
	name = disk_name(dev);
	if ( name == NULL ) {
		Kfree(ref);
		return E_NOMEM;
	}
	top = ref->top;
	if ( zero_uuid(&top) && !ref->rdonly ) {
		er = om_cre_obj(dev, name, OM_BODY_FIG, &top);
		if ( er >= E_OK ) {
			er = knl_obfile_set_top(dev, &top);
		}
		if ( er < E_OK ) {
			tm_printf((UB *)"disk: no top object on %s (%d)\n", dev, (INT)er);
			knl_memset(&top, 0, sizeof(top));
		} else if ( ref->foreign ) {
			foreign_top(dev, &top);
		}
	}
	if ( place ) {
		keep_vol(&ref->vuuid, &top, TRUE);	/* attached by the person: again at the next start */
	}
	Kfree(ref);
	om_store_recheck();
	if ( place && !zero_uuid(&top) ) {
		/*
		 * The first window's program holds its record, and puts the link
		 * unless that record has one now (DT_HOW_LINK_ONCE): a link deleted
		 * from it stays in the record as last saved until it saves. The
		 * record as saved says only while the program is not up.
		 */
		if ( !dt_prog_tell_link(DT_TELL_FIRST, DT_HOW_LINK_ONCE, &top, name, NULL) && !on_desktop(&top) ) {
			dt_place_on_desktop(&top, name);
		}
	}
	{
		char	us[TS_UUID_STRLEN + 1];

		(void)ts_uuid_to_str(&top, us, sizeof(us));
		tm_printf((UB *)"disk: attached %s \"%s\" top %s\n", dev, (CONST char *)name, us);
	}
	if ( p_name != NULL ) {
		*p_name = name;
	} else {
		Kfree(name);
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
	T_VOBJ	*v = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ));
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
			tm_printf((UB *)"desktop: the link \"%s\" was not told its volume\n", TAD_VS(v->vname));
		}
	}
	tad_vobj_clear(v);
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
			if ( dk_wait[i].name != NULL ) Kfree(dk_wait[i].name);
			dk_wait[i].name = NULL;
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
			dk_wait[i].name = dt_cat(dk_name(name), NULL);
			return TRUE;
		}
	}
	return FALSE;
}

/* The records kept for a volume that is back, written to it */
LOCAL void wait_done( void )
{
	UB	*msg;
	INT	i;
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
		tm_printf((UB *)"disk: kept changes of %s saved (%d)\n", dk_name(w->name), (INT)er);
		msg = dt_cat("「", dk_name(w->name), ( er >= E_OK ) ? "」の変更を保存しました"
							 : "」の変更を保存できませんでした", NULL);
		(void)wm_msg_put(( msg != NULL ) ? (CONST char *)msg
						 : ( er >= E_OK ) ? "変更を保存しました" : "変更を保存できませんでした");
		if ( msg != NULL ) Kfree(msg);
		Kfree(w->xml);
		if ( w->name != NULL ) Kfree(w->name);
		w->name = NULL;
		w->used = FALSE;
	}
}

/*
 * A window whose disk went with changes not saved: the person says
 * where they go -- another volume attached, the disk when it is put
 * back, or nowhere -- in a panel over host. xml (Kmalloc, size bytes)
 * is the record as the window's program held it, its links made to say
 * the volume (links_name_vol); it is this function's.
 */
LOCAL void rescue( DTWIN *host, CONST TS_UUID *id, UINT body, CONST TS_UUID *vuuid, CONST UB *name,
		   UB *xml, SZ size )
{
	UB		(*to)[DK_PATH], t[WM_LABEL_MAX], *msg;
	T_OBFVREF	ref;
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		n, m = 0, i, pid, w = 600, h, pick = -1;
	UINT		ans;
	DTWIN		*f = host;
	TS_UUID		nid;
	ER		er;

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
		wm_panel_free(def);
		goto keep;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, w - 28, 22, "ディスクが取り外されました。");
	msg = dt_cat("「", dk_name(name), "」の変更は保存されていません。どうしますか:", NULL);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 30, w - 28, 22,		/* the label keeps what fits */
		  ( msg != NULL ) ? (CONST char *)msg : "変更は保存されていません。どうしますか:");
	if ( msg != NULL ) Kfree(msg);
	for ( i = 0; i < m + 2; i++ ) {
		UB	*line = NULL;

		if ( i < m ) {
			UB	*vname = disk_name((CONST char *)to[i]);

			/* the label whole: the choice keeps what fits of the line */
			line = dt_cat("「", ( vname != NULL ) ? (CONST char *)vname : (CONST char *)to[i],
				      "」へ別の実身として保存", NULL);
			if ( vname != NULL ) Kfree(vname);
			if ( line == NULL ) {
				(void)put(t, 0, sizeof(t), "別の実身として保存");
			}
		} else if ( i == m ) {
			(void)put(t, 0, sizeof(t), "ディスクを挿し直したときに保存");
		} else {
			(void)put(t, 0, sizeof(t), "変更を捨てる");
		}
		pt = dt_pn_add(def, WM_PT_CHOICE, PN_TO + i, 24, 58 + 26 * i, w - 48, 22,
			       (CONST char *)( ( line != NULL ) ? line : t ));
		if ( line != NULL ) Kfree(line);
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
		er = save_to((CONST char *)to[pick], name, body, xml, size, &nid);
		tm_printf((UB *)"disk: changes of %s saved to %s (%d)\n", (CONST char *)name,
			  (CONST char *)to[pick], (INT)er);
		if ( er >= E_OK ) {
			msg = dt_cat("「", dk_name(name), "」を別の実身として保存しました", NULL);
			(void)wm_msg_put(( msg != NULL ) ? (CONST char *)msg : "別の実身として保存しました");
			if ( msg != NULL ) Kfree(msg);
			Kfree(xml);
			goto out;
		}
		dt_tell(f, "保存できませんでした。", "ディスクを挿し直したときに保存します");
	}
    keep:
	/* cancelled, or not written: kept for the disk, rather than lost */
	if ( wait_put(vuuid, id, 0, xml, size, name) ) {
		tm_printf((UB *)"disk: changes of %s kept for the disk\n", dk_name(name));
		msg = dt_cat("「", dk_name(name), "」の変更は、ディスクを挿し直したときに保存します", NULL);
		(void)wm_msg_put(( msg != NULL ) ? (CONST char *)msg : "変更は、ディスクを挿し直したときに保存します");
		if ( msg != NULL ) Kfree(msg);
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
#define CL_MAX		( 1024 + CL_LINES * ( 200 * 6 + 8 ) )
#define CL_LINE_H	20		/* a line of the 14 dot letters, in the page */
#define CL_W		1800		/* the box of the text: the longest line, unwrapped */

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

LOCAL void cl_num( INT v )
{
	char	t[12];
	INT	k = 0;

	do {
		t[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && k < 11 );
	while ( k > 0 && cl_at < CL_MAX - 1 ) cl_xml[cl_at++] = (UB)t[--k];
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

/* The window of the console's lines: the page made of them, drawn as a figure's */
LOCAL void cl_draw( DTWIN *d )
{
	T_TAD		*rec = (T_TAD *)d->self;
	T_TVFIG		*fig = NULL;
	T_DPRECT	r;
	INT		gid = wm_gid(d->wid);

	dt_own_ground(d);
	dt_work_rect(d, &r);
	if ( rec != NULL && gid >= 0 && tv_fig(rec, &fig) >= E_OK && fig != NULL ) {
		(void)tv_fig_draw(gid, fig, &r, 0, 0, rec, TV_PAPER_LOOK);
		tv_fig_free(fig);
	}
}

LOCAL BOOL cl_input( DTWIN *d, CONST T_WMEV *ev )
{
	(void)d;
	(void)ev;
	return FALSE;
}

LOCAL void cl_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	(void)d;
	(void)sel;
}

LOCAL void cl_close( DTWIN *d )
{
	if ( d->self != NULL ) {
		tad_free((T_TAD *)d->self);
	}
}

LOCAL CONST DTOWN	cl_own = {
	SYSDEF_MENU_NOTE,
	cl_draw,
	cl_input,
	NULL,
	NULL,
	cl_menu_do,
	NULL,
	cl_close,
	NULL,
	NULL
};

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
	knl_bootlog_tail(knl_bootlog_head(), CL_LINES, cl_line);
	/*
	 * A page of the desktop's is a figure: the lines are a piece of text
	 * in it, in a box as wide as the longest line and as tall as the lines
	 */
	cl_put("<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	cl_put(title);
	cl_put("\"><figure><document><docView viewleft=\"8\" viewtop=\"8\" viewright=\"");
	cl_num(8 + CL_W);
	cl_put("\" viewbottom=\"");
	cl_num(8 + ( cl_n + 1 ) * CL_LINE_H);
	cl_put("\"/><docDraw drawleft=\"8\" drawtop=\"8\" drawright=\"");
	cl_num(8 + CL_W);
	cl_put("\" drawbottom=\"");
	cl_num(8 + ( cl_n + 1 ) * CL_LINE_H);
	cl_put("\"/><docScale hunit=\"-72\" vunit=\"-72\"/><text lang=\"0\" bpat=\"0\" zIndex=\"0\"/>"
	       "<font size=\"14\" face=\"sans-serif\" color=\"#000000\"/>");
	for ( i = cl_n - 1; i >= 0; i-- ) {
		cl_para(cl_text[i], cl_len[i]);	/* the newest at the top, where a photograph sees it */
	}
	Kfree(cl_text);
	cl_text = NULL;
	cl_put("</document></figure></tad>");
	if ( tad_parse(cl_xml, (SZ)cl_at, NULL, &rec) < E_OK ) {
		rec = NULL;
	}
	Kfree(cl_xml);
	cl_xml = NULL;
	if ( rec == NULL ) {
		tm_printf((UB *)"disk: the console's lines not shown\n");
		return;
	}
	o.left = wm_new_x(40);		/* from the corner of the screen new windows open on */
	o.top = wm_new_y(240);
	o.right = o.left + 960;
	o.bottom = o.top + 500;
	if ( dt_own_open(&cl_own, rec, &o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE, title, NULL) == NULL ) {
		tad_free(rec);
	}
}
#endif

/*
 * The first window, whose object's volume went: never closed, its
 * program keeps the record it shows. Answers what the message line is
 * to say after the windows closed have said theirs: that the disk went,
 * the system's own disk said as such.
 */
LOCAL CONST char *keep_first( CONST TS_UUID *vuuid )
{
	BOOL	sys = (BOOL)( dk_has_sysvu && ts_uuid_cmp(vuuid, &dk_sysvu) == 0 );

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
		tm_printf((UB *)"disk: closed window of %s\n", ( d->name != NULL ) ? (CONST char *)d->name : "");
		dt_close(d);
		(void)wm_msg_put("ディスクが取り外されたため、ウインドウを閉じました");
	}
}

/* The window of a program, closed: said so when its disk went */
LOCAL void win_gone( DKWIN *e )
{
	UB	*msg;

	if ( !e->asked ) {
		return;
	}
	tm_printf((UB *)"disk: closed window of %s\n", dk_name(e->name));
	msg = dt_cat("ディスクが取り外されたため、", dk_name(e->name), "のウインドウを閉じました", NULL);
	(void)wm_msg_put(( msg != NULL ) ? (CONST char *)msg : "ディスクが取り外されたため、ウインドウを閉じました");
	if ( msg != NULL ) Kfree(msg);
}

/*
 * The windows of objects on a volume that went: their programs asked to
 * close them, as the person closes a window. One with changes not saved
 * hands them over first (DT_DISK_RESCUE); one closed is said closed as
 * it goes (note_windows). The first window stays.
 */
LOCAL void close_gone( void )
{
	CONST char	*said = NULL;
	INT		i, fw = 0;
	DTWIN		*f = dt_first_win();

	close_lost_parts();
	if ( f != NULL ) {
		fw = f->wid;
	}
	for ( i = 0; i < dk_nwin; i++ ) {
		DKWIN	*e = &dk_win[i];
		ID	pid;

		if ( !e->onvol || e->asked || knl_obfile_vol_of(&e->id) > 0 ) {
			continue;
		}
		if ( e->wid == fw ) {
			said = keep_first(&e->vu);
			e->onvol = FALSE;
			continue;
		}
		pid = wm_obj_maker(e->wid);
		if ( pid <= 0 ) {
			continue;
		}
		e->asked = TRUE;
		tm_printf((UB *)"disk: window of %s asked to close\n", dk_name(e->name));
		(void)wm_obj_close_window(e->wid);
	}
	if ( said != NULL ) {
#ifdef X64
		if ( dk_sysgone ) {
			show_console();		/* after the windows that went */
		}
#endif
		(void)wm_msg_put(said);
	}
}

/*
 * Which program's window shows what, and whether that is on a volume:
 * new windows looked at once (all: every one again). A window gone that
 * was asked to close for its disk is said closed.
 */
LOCAL void note_windows( BOOL all )
{
	DKWIN	*was;
	INT	*wids, n, i, k, nw = 0;
	TS_UUID	id;
	BOOL	seen[DK_WINS];

	knl_memset(seen, 0, sizeof(seen));
	wids = (INT *)Kmalloc(sizeof(INT) * DK_WINS);
	was = (DKWIN *)Kmalloc(sizeof(DKWIN) * DK_WINS);
	if ( wids == NULL || was == NULL ) {
		if ( wids != NULL ) Kfree(wids);
		if ( was != NULL ) Kfree(was);
		return;
	}
	n = wm_list(wids, DK_WINS);
	if ( n > DK_WINS ) n = DK_WINS;
	knl_memcpy(was, dk_win, sizeof(DKWIN) * (SZ)dk_nwin);
	for ( i = 0; i < n; i++ ) {
		DKWIN	*e = &dk_win[nw];
		T_WMWIN	w;

		if ( dt_win_of(wids[i]) != NULL || wm_obj_maker(wids[i]) <= 0 || !wm_obj_shown(wids[i], &id)
		  || wm_ref(wids[i], &w) < E_OK || ( w.attr & WM_ATTR_POPUP ) != 0 ) {
			continue;
		}
		for ( k = 0; k < dk_nwin; k++ ) {
			if ( was[k].wid == wids[i] && ts_uuid_cmp(&was[k].id, &id) == 0 ) break;
		}
		if ( k < dk_nwin && !all ) {
			*e = was[k];
		} else {
			knl_memset(e, 0, sizeof(*e));
			e->wid = wids[i];
			e->id = id;
			e->asked = ( k < dk_nwin ) ? was[k].asked : FALSE;
			e->onvol = (BOOL)( knl_obfile_vol_of(&id) > 0 );
			if ( !e->onvol || knl_obfile_vuuid_of(&id, &e->vu) < E_OK ) {
				knl_memset(&e->vu, 0, sizeof(e->vu));
			}
			if ( !e->onvol && k < dk_nwin ) {
				/*
				 * its volume gone since it was looked at: which one it
				 * was is kept, for what its program hands over
				 * (DT_DISK_RESCUE) to be written when that disk is back
				 */
				e->vu = was[k].vu;
			}
			if ( k < dk_nwin ) {
				e->name = was[k].name;		/* the entry's now */
			} else {
				e->name = om_store_name_dup(&id, NULL);	/* NULL: said as 実身 */
			}
		}
		if ( k < dk_nwin ) seen[k] = TRUE;
		nw++;
	}
	for ( k = 0; k < dk_nwin; k++ ) {
		if ( !seen[k] ) {
			win_gone(&was[k]);	/* not there any more */
			if ( was[k].name != NULL ) Kfree(was[k].name);
		}
	}
	dk_nwin = nw;
	Kfree(was);
	Kfree(wids);
}

/*
 * DT_DISK_RESCUE: what a program holds of an object whose disk went,
 * handed over before its window closes (grab_x 0: an object of the
 * system's made for it, given in into, which the program writes its
 * record into; grab_x 1: that one, named in text, read and the person
 * asked where the changes go, then thrown away)
 */
LOCAL ER rescue_do( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	TS_UUID	tmp;
	T_TAD	*rec = NULL;
	UB	*xml, *out;
	SZ	size = 0, osz = 0;
	INT	i;
	DKWIN	*e = NULL;
	ER	er;

	for ( i = 0; i < dk_nwin; i++ ) {
		if ( ts_uuid_cmp(&dk_win[i].id, &rq->target) == 0 && dk_win[i].asked ) e = &dk_win[i];
	}
	if ( e == NULL || pid <= 0 || wm_obj_maker(e->wid) != pid ) {
		return E_PAR;			/* only the program of a window asked to close for its disk */
	}
	if ( rq->grab_x == 0 ) {
		er = om_cre_obj(NULL, (CONST UB *)"退避", ( rq->grab_y != 0 ) ? OM_BODY_FIG : OM_BODY_DOC, &tmp);
		if ( er < E_OK ) {
			return er;
		}
		knl_ob_grant(pid, &tmp, OB_OP_R | OB_OP_W);
		an->into = tmp;
		return E_OK;
	}
	if ( ts_str_to_uuid((CONST char *)dt_rq_word(rq, DT_TAIL_TEXT, NULL), &tmp) < E_OK ) {
		return E_PAR;
	}
	om_store_forget(&tmp);
	xml = om_obj_record(&tmp, 0, &size);
	(void)om_store_delete(&tmp);
	if ( xml == NULL || tad_parse(xml, size, NULL, &rec) < E_OK ) {
		if ( xml != NULL ) Kfree(xml);
		tm_printf((UB *)"disk: changes of %s cannot be kept\n", dk_name(e->name));
		return E_IO;
	}
	Kfree(xml);
	links_name_vol(rec, &e->vu);
	out = NULL;
	if ( tad_write_mem(rec, NULL, 0, &osz) >= E_OK && ( out = (UB *)Kmalloc(osz + 1) ) != NULL
	  && tad_write_mem(rec, out, osz + 1, &osz) < E_OK ) {
		Kfree(out);
		out = NULL;
	}
	tad_free(rec);
	if ( out == NULL ) {
		return E_NOMEM;
	}
	rescue(dt_host_of(&rq->win), &e->id, ( rq->grab_y != 0 ) ? OM_BODY_FIG : OM_BODY_DOC, &e->vu,
	       (CONST UB *)dk_name(e->name), out, osz);
	return E_OK;
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
		(void)attach_dev((CONST char *)path, FALSE, FALSE, NULL);
	}
	wait_done();

    done:
	if ( names != NULL ) Kfree(names);
	if ( pr != NULL ) Kfree(pr);
	if ( kv != NULL ) Kfree(kv);
}

/* The system's disk put back: said once (the first window's program kept what it shows meanwhile) */
LOCAL void first_back( void )
{
	if ( dk_sysgone && dk_has_sysvu && knl_obfile_vuuid_on(&dk_sysvu) ) {
		dk_sysgone = FALSE;
		tm_printf((UB *)"disk: the system volume is back\n");
		(void)wm_msg_put("ディスクが挿し直され、元に戻りました");
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

/* ---------------------------------------------------------------- what ディスク操作 asks (DT_RQ_DISK) */

/* A tab between the fields of an answer's text */
LOCAL INT put_tab( UB *buf, INT at, INT max )
{
	return put(buf, at, max, "\t");
}

/* A number of a text, from *p on, past the tab after it; -1 when there is none */
LOCAL D text_num( CONST UB *t, INT *p )
{
	D	v = 0;
	BOOL	any = FALSE;

	while ( t[*p] >= '0' && t[*p] <= '9' ) {
		v = v * 10 + ( t[*p] - '0' );
		(*p)++;
		any = TRUE;
	}
	if ( t[*p] == '\t' ) (*p)++;
	return any ? v : -1;
}

/* The link a request names: its object, as a link of the desktop's would point at it; NULL for none */
LOCAL CONST T_VOBJ *req_link( CONST T_DTREQ *rq, T_VOBJ *v )
{
	tad_vobj_init(v);		/* no words: nothing to let go */
	if ( zero_uuid(&rq->target) ) {
		return NULL;
	}
	v->target = rq->target;
	return v;
}

/*
 * The rights of an administrator for what only one may do: a process
 * that is not one is asked the password for, in a panel of the
 * desktop's (dtadmin.c), and given the rights for a while when it is
 * typed right, as DT_RQ_ADMIN gives them.
 */
LOCAL ER as_admin( CONST T_DTREQ *rq, ID pid, CONST char *what )
{
	ER	er;

	if ( pid <= 0 || knl_ob_pid_admin(pid) ) {
		return E_OK;
	}
	er = dt_admin_ask(pid, (CONST UB *)what, &rq->win);
	if ( er >= E_OK ) {
		er = knl_ob_adm_give(pid, 0);
	}
	tm_printf((UB *)"disk: the administrator for %s (%d)\n", what, (INT)er);
	return er;
}

/*
 * 接続's list, made when its first row is asked for and kept for the
 * rest: the partitions not attached that hold something, by device
 * name, the row 接続 shows, and their kind.
 */
LOCAL UB	*cl_names = NULL;		/* OB_DV_NAME bytes each */
LOCAL UB	**cl_rows = NULL;		/* each whole (Kmalloc), cl_nrow of them */
LOCAL UINT	*cl_kinds = NULL;
LOCAL INT	cl_n = 0, cl_pick = -1, cl_nrow = 0;

LOCAL void connect_forget( void )
{
	INT	i;

	if ( cl_names != NULL ) Kfree(cl_names);
	if ( cl_rows != NULL ) {
		for ( i = 0; i < cl_nrow; i++ ) {
			if ( cl_rows[i] != NULL ) Kfree(cl_rows[i]);
		}
		Kfree(cl_rows);
	}
	cl_nrow = 0;
	if ( cl_kinds != NULL ) Kfree(cl_kinds);
	cl_names = NULL;
	cl_rows = NULL;
	cl_kinds = NULL;
	cl_n = 0;
	cl_pick = -1;
}

/*
 * A TSFS volume's label, whole, of a disk not attached: read with the
 * volume opened for it alone (ob_chk_label), the probe's copy of 64 bytes
 * being cut. NULL when there is none or for want of memory.
 */
LOCAL UB *probe_label( CONST char *dev, CONST T_FSPROBE *pr )
{
	UB	*lab = NULL;
	SZ	size = 0, asz = 0;

	if ( pr->kind != FS_KIND_TSFS ) {
		return ( pr->label[0] != 0 ) ? dt_cat((CONST char *)pr->label, NULL) : NULL;
	}
	for ( ;; ) {
		if ( ob_chk_label(dev, lab, size, &asz) < E_OK ) {
			break;
		}
		if ( lab != NULL && asz <= size ) {
			if ( lab[0] != 0 ) return lab;
			break;
		}
		if ( lab != NULL ) Kfree(lab);
		size = asz;
		lab = (UB *)Kmalloc((size_t)( ( size > 0 ) ? size : 1 ));
		if ( lab == NULL ) {
			break;
		}
	}
	if ( lab != NULL ) Kfree(lab);
	return ( pr->label[0] != 0 ) ? dt_cat((CONST char *)pr->label, NULL) : NULL;
}

LOCAL void connect_list( CONST T_VOBJ *v )
{
	T_FSPROBE	*pr;
	UB		sz[32], *lab;
	INT		n, i, m = 0, at = -1;

	connect_forget();
	cl_names = disks_all(&n);
	cl_rows = (UB **)Kcalloc((SZ)( n + 1 ), sizeof(UB *));
	cl_nrow = ( cl_rows != NULL ) ? n + 1 : 0;
	cl_kinds = (UINT *)Kmalloc(sizeof(UINT) * (SZ)( n + 1 ));
	pr = (T_FSPROBE *)Kmalloc(sizeof(T_FSPROBE));
	if ( cl_names == NULL || cl_rows == NULL || cl_kinds == NULL || pr == NULL ) {
		if ( pr != NULL ) Kfree(pr);
		connect_forget();
		return;
	}
	for ( i = 0; i < n; i++ ) {
		CONST char	*dev = (CONST char *)( cl_names + i * OB_DV_NAME );

		if ( knl_obfile_vol_path(dev) > 0 || knl_fs_probe(dev, pr) < E_OK
		  || pr->kind == FS_KIND_PARTED || pr->kind == FS_KIND_NONE ) {
			continue;
		}
		/* the row whole: the program showing it keeps what fits */
		(void)put_size(sz, 0, sizeof(sz), pr->bytes);
		lab = probe_label(dev, pr);
		cl_rows[m] = dt_cat(dev, "  ", (CONST char *)pr->name, ( lab != NULL ) ? "「" : "",
				    ( lab != NULL ) ? (CONST char *)lab : "", ( lab != NULL ) ? "」" : "",
				    "  ", (CONST char *)sz, NULL);
		if ( lab != NULL ) Kfree(lab);
		if ( cl_rows[m] == NULL ) {
			continue;		/* said on the console: the disk is not offered */
		}
		cl_kinds[m] = pr->kind;
		/* a 虚身 chosen: its disk, when it is in */
		if ( at < 0 && v != NULL && pr->kind == FS_KIND_TSFS && !zero_uuid(&pr->top)
		  && ts_uuid_cmp(&pr->top, &v->target) == 0 ) {
			at = m;
		}
		/* the names are kept beside the rows */
		knl_memcpy(cl_names + m * OB_DV_NAME, dev, OB_DV_NAME);
		m++;
	}
	Kfree(pr);
	for ( i = 0; i < m && at < 0; i++ ) {
		if ( cl_kinds[i] == FS_KIND_TSFS ) at = i;
	}
	cl_n = m;
	cl_pick = ( at < 0 ) ? 0 : at;
	tm_printf((UB *)"disk: connect offers %d\n", m);
}

LOCAL ER connect_row( CONST T_DTREQ *rq, T_DTANS *an )
{
	T_VOBJ	v = { .recno = 0 };
	INT	i = rq->grab_x;

	if ( i == 0 ) {
		connect_list(req_link(rq, &v));
	}
	an->how = (UINT)cl_n;
	if ( i < 0 || i >= cl_n ) {
		return E_NOEXS;
	}
	(void)dt_ans_cat(an, (CONST UB *)(CONST char *)( cl_names + i * OB_DV_NAME ), -1);
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_cat(an, cl_rows[i], -1);
	if ( i == cl_pick ) an->flags |= DT_F_DISK_PICK;
	return E_OK;
}

/* 接続 of the partition named: one of objects attached, and remembered as attached */
LOCAL ER connect_do( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	UB		dev[OB_DV_NAME], *name = NULL;
	T_FSPROBE	*pr;
	UINT		kind;
	INT		i;
	ER		er;

	SZ		nl;
	CONST UB	*rn = dt_rq_word(rq, DT_TAIL_NAME, &nl);

	if ( nl >= OB_DV_NAME ) {
		return E_PAR;			/* longer than the name of any device */
	}
	for ( i = 0; i <= (INT)nl; i++ ) dev[i] = rn[i];
	pr = (T_FSPROBE *)Kmalloc(sizeof(T_FSPROBE));
	if ( pr == NULL ) {
		return E_NOMEM;
	}
	er = knl_fs_probe((CONST char *)dev, pr);
	kind = pr->kind;
	Kfree(pr);
	tm_printf((UB *)"disk: connect %s\n", dev);
	if ( er < E_OK ) {
		return E_NOEXS;
	}
	if ( kind == FS_KIND_FAT12 || kind == FS_KIND_FAT16 || kind == FS_KIND_FAT32
	  || kind == FS_KIND_EXFAT || kind == FS_KIND_NTFS || kind == FS_KIND_BTRON ) {
		/* files, not objects: ファイル変換 reads them */
		tm_printf((UB *)"disk: %s is files, not objects\n", dev);
		return E_NOSPT;
	}
	if ( kind != FS_KIND_TSFS ) {
		tm_printf((UB *)"disk: %s is of a kind not handled\n", dev);
		return E_PAR;
	}
	/* a USB medium the system uses nothing of is the person's to attach (design 8.7.1) */
	if ( !knl_obdev_medium_person(dev) ) {
		er = as_admin(rq, pid, "ディスクの接続");
		if ( er < E_OK ) {
			return er;
		}
	} else {
		tm_printf((UB *)"disk: %s is the person's medium\n", dev);
	}
	er = attach_dev((CONST char *)dev, TRUE, FALSE, &name);
	if ( er == E_OBJ ) {
		tm_printf((UB *)"disk: %s is attached already\n", dev);
	}
	if ( er >= E_OK && name != NULL ) {
		(void)dt_ans_cat(an, (CONST UB *)(CONST char *)name, -1);
	}
	if ( name != NULL ) Kfree(name);
	return er;
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
 * Why a volume cannot be taken away now, found before anything is asked:
 * an object of it shown in a window (*p_how DT_HOW_DISK_WIN), or its
 * check under way (DT_HOW_DISK_CHECK), each E_BUSY; else E_OK
 */
LOCAL ER detach_held( CONST char *path, UINT *p_how )
{
	*p_how = 0;
	/* an object of it shown in a program's window (a process's, which says what it shows) keeps it */	/* and one shown in a program's window (a process's, which says what it shows) */
	{
		INT	n = knl_wm_nwin(), k;
		INT	*wids = (INT *)Kmalloc(sizeof(INT) * (SZ)( n + 1 ));
		TS_UUID	u;
		BOOL	shown = FALSE;

		n = ( wids != NULL ) ? wm_list(wids, n + 1) : 0;
		for ( k = 0; k < n && !shown; k++ ) {
			shown = (BOOL)( dt_win_of(wids[k]) == NULL && wm_obj_shown(wids[k], &u)
					&& knl_obfile_on(&u, path) );
		}
		if ( wids != NULL ) Kfree(wids);
		if ( shown ) {
			tm_printf((UB *)"disk: detach %s refused: a window is open\n", path);
			*p_how = DT_HOW_DISK_WIN;
			return E_BUSY;
		}
	}
	if ( knl_ob_chk_busy(path) ) {
		tm_printf((UB *)"disk: detach %s refused: being checked\n", path);
		*p_how = DT_HOW_DISK_CHECK;
		return E_BUSY;
	}
	return E_OK;
}

/*
 * A volume taken away: not while detach_held says so, nor while it is
 * used by a program (*p_how DT_HOW_DISK_PROG), each E_BUSY. E_OK when it
 * went.
 */
LOCAL ER detach_vol( CONST char *path, UINT *p_how )
{
	ER	er = detach_held(path, p_how);

	if ( er < E_OK ) {
		return er;
	}
	er = ob_det_vol(path);
	if ( er == E_BUSY ) {
		tm_printf((UB *)"disk: detach %s refused: in use\n", path);
		*p_how = knl_ob_chk_busy(path) ? DT_HOW_DISK_CHECK : DT_HOW_DISK_PROG;
		return er;
	}
	if ( er < E_OK ) {
		tm_printf((UB *)"disk: detach %s failed %d\n", path, (INT)er);
		return er;
	}
	tm_printf((UB *)"disk: detached %s\n", path);
	om_store_recheck();
	dk_volser = knl_obfile_serial();
	note_windows(TRUE);
	dt_draw_all();
	return E_OK;
}

/* Why a volume could not be taken away, told over d ('what' 切り離し or 取り外し) */
LOCAL void tell_detach( DTWIN *d, CONST UB *name, CONST char *what, ER er, UINT how, CONST char *fail )
{
	UB	*msg, hint[64];
	INT	k;

	if ( er != E_BUSY || name == NULL ) {
		dt_tell(d, fail, NULL);
		return;
	}
	msg = text_cat3("ディスク「", (CONST char *)name,
			( how == DT_HOW_DISK_WIN ) ? "」の実身が開いています。"
			: ( how == DT_HOW_DISK_CHECK ) ? "」はディスク修復で検査中です。"
			: "」の実身を使っているプログラムがあります。");
	if ( msg == NULL ) {
		dt_tell(d, fail, NULL);
		return;
	}
	k = put(hint, 0, sizeof(hint), ( how == DT_HOW_DISK_WIN ) ? "閉じてから" : "終わってから");
	k = put(hint, k, sizeof(hint), what);
	(void)put(hint, k, sizeof(hint), "てください");
	dt_tell(d, (CONST char *)msg, (CONST char *)hint);
	Kfree(msg);
}

/* 切り離し of the volume of the link's object */
LOCAL ER detach_do( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	UB		path[DK_PATH], *name;
	T_OBFVREF	ref;
	ER		er;

	if ( zero_uuid(&rq->target) || knl_obfile_path_of(&rq->target, path, sizeof(path)) < E_OK
	  || knl_obfile_vol_ref((CONST char *)path, &ref) < E_OK ) {
		return E_NOEXS;
	}
	name = disk_name((CONST char *)path);
	if ( name == NULL ) {
		return E_NOMEM;
	}
	(void)dt_ans_cat(an, (CONST UB *)(CONST char *)name, -1);
	Kfree(name);
	if ( ref.system ) {
		return E_RONLY;
	}
	/* what keeps it is said before the password is asked for */
	er = detach_held((CONST char *)path, &an->how);
	if ( er >= E_OK && !knl_obdev_medium_person(path) ) {
		er = as_admin(rq, pid, "ディスクの切り離し");
	}
	if ( er >= E_OK ) {
		er = detach_vol((CONST char *)path, &an->how);
	}
	if ( er >= E_OK && ref.native ) {
		keep_vol(&ref.vuuid, &ref.top, FALSE);	/* not attached again by itself */
	}
	return er;
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
	UB		path[DK_PATH], *name, *msg;
	T_OBFVREF	ref;
	TS_UUID		*kv, *kt;
	INT		nk = 0, i;
	UINT		how;
	ER		er;

	if ( v == NULL || d == NULL ) {
		return TRUE;
	}
	if ( knl_obfile_path_of(&v->target, path, sizeof(path)) >= E_OK ) {
		if ( knl_obfile_vol_ref((CONST char *)path, &ref) < E_OK || ref.system || !ref.native
		  || ts_uuid_cmp(&ref.top, &v->target) != 0 ) {
			return TRUE;		/* an object's link, not a disk's */
		}
		name = disk_name((CONST char *)path);	/* while it is attached: the label is its top's name */
		er = detach_vol((CONST char *)path, &how);
		if ( er < E_OK ) {
			tell_detach(d, name, "取り外し", er, how, "ディスクを取り外せませんでした");
			if ( name != NULL ) Kfree(name);
			return FALSE;
		}
		keep_vol(&ref.vuuid, &ref.top, FALSE);
		msg = text_cat3("ディスク「", ( name != NULL ) ? (CONST char *)name : (CONST char *)path,
				"」を取り外しました。取り出せます");
		if ( msg != NULL ) {
			(void)wm_msg_put((CONST char *)msg);
			Kfree(msg);
		}
		if ( name != NULL ) Kfree(name);
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
LOCAL ER status_do( CONST T_DTREQ *rq, T_DTANS *an )
{
	T_OBVOL		vol;
	T_OBFVREF	ref;
	T_VOBJ		lv = { .recno = 0 };
	CONST T_VOBJ	*v = req_link(rq, &lv);
	UB		path[DK_PATH], *name = NULL;
	UD		total, used;
	INT		gc = 0;
	ID		tv;

	if ( ob_ref_vol(( v != NULL ) ? &v->target : NULL, &vol) < E_OK ) {
		return E_NOEXS;
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
	if ( ( v != NULL && knl_obfile_path_of(&v->target, path, sizeof(path)) >= E_OK )
	  || ( v == NULL && knl_obfile_vols(path, sizeof(path), 1) == 1 ) ) {
		if ( knl_obfile_vol_ref((CONST char *)path, &ref) >= E_OK ) {
			name = disk_name((CONST char *)path);
		}
	}
	total = vol.blocks * vol.bsize;
	used = ( vol.blocks - vol.bfree ) * vol.bsize;
	tm_printf((UB *)"disk: status \"%s\" blocks %d free %d objects %d garbage %d\n",
		  ( name != NULL ) ? (CONST char *)name : "", (INT)vol.blocks, (INT)vol.bfree, vol.nobj, gc);
	if ( name != NULL ) {
		(void)dt_ans_cat(an, (CONST UB *)(CONST char *)name, -1);
		Kfree(name);
	}
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( total ));
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( used ));
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( (UD)vol.nobj ));
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( (UD)gc ));
	return E_OK;
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

/* A free stretch of a disk, as 区画作成 offers it */
typedef struct {
	UB	disk[OB_DV_NAME];
	UD	first, last;
	BOOL	fresh;			/* the disk has no table yet */
	BOOL	system;			/* the disk the system volume is on */
} DKFREE;

#define DK_MIN_MB	8		/* a stretch smaller than this is not offered */

/*
 * The free stretches of one disk, every one: a list that came back full
 * is asked for again in more room. How many, in *p_g (Kfree it).
 */
LOCAL INT disk_free( CONST char *dev, T_GPTFREE **p_g, BOOL *p_fresh )
{
	T_GPTFREE	*g = NULL;
	INT		room = 8, c = 0;
	UD		nsect = 0;

	for ( ;; ) {
		g = (T_GPTFREE *)Kmalloc(sizeof(T_GPTFREE) * (SZ)room);
		if ( g == NULL ) {
			c = 0;
			break;
		}
		c = knl_gpt_free(dev, g, room, &nsect, p_fresh);
		if ( c < room ) break;
		Kfree(g);
		g = NULL;
		room *= 2;
	}
	*p_g = g;
	return ( c > 0 ) ? c : 0;
}

/* 区画作成's list, made when its first row is asked for and kept for the rest */
LOCAL DKFREE	*fl_fr = NULL;
LOCAL UB	(*fl_rows)[WM_LABEL_MAX] = NULL;
LOCAL INT	fl_n = 0;

LOCAL void free_list( void )
{
	UB		*names, sd[OB_DV_NAME];
	T_GPTFREE	*g;
	INT		n, i, j, k, c, room = 0;
	BOOL		fresh;

	if ( fl_fr != NULL ) Kfree(fl_fr);
	if ( fl_rows != NULL ) Kfree(fl_rows);
	fl_fr = NULL;
	fl_rows = NULL;
	fl_n = 0;
	names = disks_all(&n);
	if ( names == NULL ) {
		return;
	}
	sys_disk(sd, sizeof(sd));
	for ( i = 0; i < n; i++ ) {
		CONST char	*dev = (CONST char *)( names + i * OB_DV_NAME );

		k = (INT)knl_strlen(dev);
		if ( k == 0 || ( dev[k - 1] >= '0' && dev[k - 1] <= '9' ) ) {
			continue;		/* a partition: the disks are looked at whole */
		}
		fresh = FALSE;
		c = disk_free(dev, &g, &fresh);
		/* a row for each, the rows grown to hold them */
		if ( c > 0 && fl_n + c > room ) {
			INT	nr = ( room > 0 ) ? room : 16;
			UB	(*nrw)[WM_LABEL_MAX];
			DKFREE	*nfr;

			while ( nr < fl_n + c ) nr *= 2;
			nrw = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * (SZ)nr);
			nfr = (DKFREE *)Kmalloc(sizeof(DKFREE) * (SZ)nr);
			if ( nrw == NULL || nfr == NULL ) {
				if ( nrw != NULL ) Kfree(nrw);
				if ( nfr != NULL ) Kfree(nfr);
				c = 0;
			} else {
				if ( fl_rows != NULL ) {
					knl_memcpy(nrw, fl_rows, (INT)( sizeof(UB) * WM_LABEL_MAX * (SZ)fl_n ));
					Kfree(fl_rows);
				}
				if ( fl_fr != NULL ) {
					knl_memcpy(nfr, fl_fr, (INT)( sizeof(DKFREE) * (SZ)fl_n ));
					Kfree(fl_fr);
				}
				fl_rows = nrw;
				fl_fr = nfr;
				room = nr;
			}
		}
		for ( j = 0; j < c; j++ ) {
			DKFREE	*f = &fl_fr[fl_n];
			UB	*r = fl_rows[fl_n];
			UD	bytes = ( g[j].last - g[j].first + 1 ) * 512;

			if ( bytes < (UD)DK_MIN_MB << 20 ) continue;
			knl_memcpy(f->disk, dev, OB_DV_NAME);
			f->first = g[j].first;
			f->last = g[j].last;
			f->fresh = fresh;
			f->system = same_name((CONST UB *)dev, sd);
			k = put(r, 0, WM_LABEL_MAX, dev);
			k = put(r, k, WM_LABEL_MAX, "  空き ");
			k = put_size(r, k, WM_LABEL_MAX, bytes);
			k = put(r, k, WM_LABEL_MAX, "(");
			k = put_size(r, k, WM_LABEL_MAX, g[j].first * 512);
			k = put(r, k, WM_LABEL_MAX, "の位置から)");
			if ( fresh ) {
				k = put(r, k, WM_LABEL_MAX, " 区画表なし");
			}
			if ( f->system ) {
				(void)put(r, k, WM_LABEL_MAX, " システムのディスク");
			}
			fl_n++;
		}
		if ( g != NULL ) Kfree(g);
	}
	Kfree(names);
	tm_printf((UB *)"disk: create offers %d\n", fl_n);
}

LOCAL ER free_row( CONST T_DTREQ *rq, T_DTANS *an )
{
	DKFREE	*f;
	INT	i = rq->grab_x;

	if ( i == 0 ) {
		free_list();
	}
	an->how = (UINT)fl_n;
	if ( i < 0 || i >= fl_n ) {
		return E_NOEXS;
	}
	f = &fl_fr[i];
	(void)dt_ans_cat(an, (CONST UB *)(CONST char *)f->disk, -1);
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( f->first ));
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( f->last ));
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_cat(an, (CONST UB *)(CONST char *)fl_rows[i], -1);
	if ( f->system ) an->flags |= DT_F_DISK_SYS;
	return E_OK;
}

/*
 * 区画作成: a TSFS partition made in the free stretch of the disk named
 * that begins where the text says, a volume made on it and attached, and
 * remembered as attached (as 接続 does). The stretch is looked for again
 * on the disk as it is now, not taken from the list.
 */
LOCAL ER create_do( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	UB		disk[OB_DV_NAME], *label, part[OB_DV_NAME], *name = NULL;
	T_GPTFREE	*g = NULL;
	UD		first, last = 0;
	D		mb = rq->grab_x, at;
	INT		i, c, p = 0, ln;
	BOOL		fresh = FALSE, found = FALSE;
	ER		er;

	SZ		nl;
	CONST UB	*rn = dt_rq_word(rq, DT_TAIL_NAME, &nl), *t = dt_rq_word(rq, DT_TAIL_TEXT, NULL);

	if ( nl >= OB_DV_NAME ) {
		return E_PAR;			/* longer than the name of any device */
	}
	for ( i = 0; i <= (INT)nl; i++ ) disk[i] = rn[i];
	at = text_num(t, &p);
	for ( ln = 0; t[p + ln] != 0 && t[p + ln] != '\t'; ln++ ) ;	/* the label, of any length (design 7.6) */
	first = (UD)at;
	c = ( at >= 0 ) ? disk_free((CONST char *)disk, &g, &fresh) : 0;
	for ( i = 0; i < c && !found; i++ ) {
		if ( g[i].first == first ) {
			last = g[i].last;
			found = TRUE;
		}
	}
	if ( g != NULL ) Kfree(g);
	if ( !found ) {
		tm_printf((UB *)"disk: create %s: no such free stretch\n", disk);
		return E_NOEXS;
	}
	if ( mb < 0 || (UD)mb * 2048 > last - first + 1 || ( mb > 0 && mb < DK_MIN_MB ) ) {
		tm_printf((UB *)"disk: create %s refused: %d MB\n", disk, (INT)mb);
		return E_PAR;
	}
	er = as_admin(rq, pid, "区画の作成");
	if ( er < E_OK ) {
		tm_printf((UB *)"disk: create %s refused: not the administrator\n", disk);
		return er;
	}
	label = ( ln > 0 ) ? (UB *)Kmalloc((SZ)ln + 1) : text_cat3("ディスク", "", "");
	if ( label == NULL ) {
		tm_printf((UB *)"disk: create %s: no memory for the label of %d bytes\n", disk, ln);
		return E_NOMEM;
	}
	if ( ln > 0 ) {
		knl_memcpy(label, t + p, ln);
		label[ln] = 0;
	}
	er = knl_tsfs_part_make((CONST char *)disk, first, (UD)mb << 20, (CONST char *)label, part, sizeof(part));
	tm_printf((UB *)"disk: created %s %d MB \"%s\" (%d)\n", part,
		  (INT)( ( mb > 0 ) ? mb : (D)( ( last - first + 1 ) / 2048 ) ), (CONST char *)label, (INT)er);
	Kfree(label);
	if ( er < E_OK ) {
		return er;
	}
	/* the volume made is the person's, as one 接続 attached */
	er = attach_dev((CONST char *)part, TRUE, TRUE, &name);
	if ( er < E_OK ) {
		an->how = DT_HOW_DISK_MADE;
		if ( name != NULL ) Kfree(name);
		return er;
	}
	if ( name != NULL ) {
		(void)dt_ans_cat(an, (CONST UB *)(CONST char *)name, -1);
		Kfree(name);
	}
	return E_OK;
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
 * What 拡張 can do to the volume of the link's object (the system's
 * away from a link): its size now and the most it can be, into the
 * free stretch right after its partition.
 */
LOCAL ER grow_room( CONST T_VOBJ *v, UB *path, T_OBFVREF *ref, UD *p_cur, UD *p_most )
{
	T_GPTPART	gp;
	T_OBVOL		vol;

	if ( !grow_target(v, path, DK_PATH, ref) ) {
		return E_NOSPT;
	}
	if ( knl_gpt_part((CONST char *)path, &gp) < E_OK || !gp.tsfs ) {
		tm_printf((UB *)"disk: grow %s: not a partition of a GPT\n", path);
		return E_OBJ;
	}
	*p_cur = ( gp.last - gp.first + 1 ) * 512;
	*p_most = ( gp.grow_last - gp.first + 1 ) * 512;
	if ( ob_ref_vol(( v != NULL ) ? &v->target : NULL, &vol) >= E_OK && vol.blocks * vol.bsize < *p_cur ) {
		*p_cur = vol.blocks * vol.bsize;	/* a partition larger than its volume: that much to take in */
	}
	return ( *p_most <= *p_cur ) ? E_LIMIT : E_OK;
}

LOCAL ER grow_ask( CONST T_DTREQ *rq, T_DTANS *an )
{
	UB		path[DK_PATH], *name;
	T_OBFVREF	ref;
	T_VOBJ		lv = { .recno = 0 };
	UD		cur = 0, most = 0;
	ER		er;

	er = grow_room(req_link(rq, &lv), path, &ref, &cur, &most);
	if ( er == E_NOSPT ) {
		return er;
	}
	name = disk_name((CONST char *)path);
	if ( name == NULL ) {
		return E_NOMEM;
	}
	if ( er == E_OK || er == E_LIMIT ) {
		tm_printf((UB *)"disk: grow %s offers %d MB to %d MB\n", path, (INT)( cur >> 20 ), (INT)( most >> 20 ));
	}
	(void)dt_ans_cat(an, (CONST UB *)(CONST char *)name, -1);
	Kfree(name);
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_cat(an, (CONST UB *)(CONST char *)path, -1);
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( cur >> 20 ));
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( most >> 20 ));
	if ( ref.system ) an->flags |= DT_F_DISK_SYS;
	return er;
}

/*
 * 拡張: the partition of the volume and the volume on it made larger, to
 * the size asked (grab_x megabytes), the room looked at again first.
 * Not while the volume is being checked.
 */
LOCAL ER grow_do( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	UB		path[DK_PATH];
	T_OBFVREF	ref;
	T_VOBJ		lv = { .recno = 0 };
	UD		cur = 0, most = 0, from = 0, to = 0, bytes;
	D		mb = rq->grab_x;
	ER		er;

	er = grow_room(req_link(rq, &lv), path, &ref, &cur, &most);
	if ( er < E_OK ) {
		return er;
	}
	bytes = (UD)mb << 20;
	if ( mb < 0 || bytes <= cur || bytes > most ) {
		tm_printf((UB *)"disk: grow %s refused: %d MB\n", path, (INT)mb);
		return E_PAR;
	}
	er = as_admin(rq, pid, "ディスクの拡張");
	if ( er < E_OK ) {
		tm_printf((UB *)"disk: grow %s refused: not the administrator\n", path);
		return er;
	}
	if ( !knl_ob_chk_hold((CONST char *)path, TRUE) ) {
		tm_printf((UB *)"disk: grow %s refused: being checked\n", path);
		an->how = DT_HOW_DISK_CHECK;
		return E_BUSY;
	}
	er = knl_tsfs_part_grow((CONST char *)path, bytes, &from, &to);
	(void)knl_ob_chk_hold((CONST char *)path, FALSE);
	tm_printf((UB *)"disk: grown %s %d MB -> %d MB (%d)\n", path, (INT)( from >> 20 ), (INT)( to >> 20 ), (INT)er);
	if ( er < E_OK ) {
		return er;
	}
	(void)dt_ans_num(an, (D)( from >> 20 ));
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, (D)( to >> 20 ));
	dt_draw_all();
	return E_OK;
}

/* ---------------------------------------------------------------- ディスク修復's record */

#define LOG_NAME	"ディスク修復記録"

/* The object of that name the system box links to, or a new one linked from it */
LOCAL ER log_object( CONST TS_UUID *box, TS_UUID *p_u )
{
	TS_UUID	*ids;
	UB	*rec, name[sizeof(LOG_NAME)];
	SZ	len = 0;
	INT	n, i;
	ER	er;

	rec = om_obj_record(box, 0, &len);
	if ( rec != NULL ) {
		/* every link of the box: asked how many, then taken */
		n = om_store_links(rec, len, NULL, 0);
		ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( ( n > 0 ) ? n : 1 ));
		n = ( ids != NULL ) ? om_store_links(rec, len, ids, n) : 0;
		Kfree(rec);
		for ( i = 0; i < n; i++ ) {
			if ( om_store_name(&ids[i], name, sizeof(name)) == (INT)sizeof(LOG_NAME) - 1	/* the whole name */
			  && knl_strcmp((char *)name, LOG_NAME) == 0 ) {
				*p_u = ids[i];
				Kfree(ids);
				return E_OK;
			}
		}
		if ( ids != NULL ) Kfree(ids);
	}
	er = om_cre_obj(NULL, (CONST UB *)LOG_NAME, OM_BODY_DOC, p_u);
	if ( er >= E_OK ) {
		er = om_obj_link_add(box, p_u);
	}
	return er;
}

/*
 * 保存 of ディスク修復's 障害情報: the record the program made (target's
 * record 0, a document) written as the text object ディスク修復記録 of
 * the system box (/SYS), a system object only the desktop writes. Only
 * for the program ディスク修復.
 */
LOCAL ER log_do( CONST T_DTREQ *rq, ID pid )
{
	TS_UUID	box, u, prog, fix;
	UB	*x;
	SZ	len = 0;
	ER	er;

	if ( pid <= 0 || knl_prc_uuid(pid, NULL, &prog) < E_OK || ts_str_to_uuid(SYSDEF_PROG_DISKFIX, &fix) < E_OK
	  || ts_uuid_cmp(&prog, &fix) != 0 ) {
		return E_OACV;
	}
	if ( ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &box) < E_OK ) {
		return E_SYS;
	}
	x = om_obj_record(&rq->target, 0, &len);
	if ( x == NULL ) {
		return E_NOEXS;
	}
	er = log_object(&box, &u);
	if ( er >= E_OK ) {
		er = om_obj_record_put(&u, 0, OB_RT_TAD, 0, x, len);
		om_store_forget(&u);
	}
	Kfree(x);
	tm_printf((UB *)"disk: the record of ディスク修復 written (%d)\n", (INT)er);
	return er;
}

/* ---------------------------------------------------------------- the requests */

EXPORT ER dt_disk_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	switch ( rq->recno ) {
	case DT_DISK_LIST:	return connect_row(rq, an);
	case DT_DISK_ATTACH:	return connect_do(rq, an, pid);
	case DT_DISK_DETACH:	return detach_do(rq, an, pid);
	case DT_DISK_STATUS:	return status_do(rq, an);
	case DT_DISK_FREE:	return free_row(rq, an);
	case DT_DISK_MAKE:	return create_do(rq, an, pid);
	case DT_DISK_GROWS:	return grow_ask(rq, an);
	case DT_DISK_GROW:	return grow_do(rq, an, pid);
	case DT_DISK_LOG:	return log_do(rq, pid);
	case DT_DISK_MENU:
	case DT_DISK_REMOVE: {
		/* the first window's menu and its links: as the desktop's own did them */
		T_VOBJ	*v = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ));
		DTWIN	*f = dt_first_win();
		BOOL	ok = TRUE;

		if ( v == NULL ) {
			return E_NOMEM;
		}
		v->target = rq->target;
		(void)ts_str_to_uuid((CONST char *)dt_rq_word(rq, DT_TAIL_TEXT, NULL), &v->tvol);
		if ( rq->recno == DT_DISK_MENU ) {
			dt_disk_menu(dt_host_of(&rq->win), (CONST char *)dt_rq_word(rq, DT_TAIL_NAME, NULL),
				     zero_uuid(&v->target) ? NULL : v);
		} else if ( f == NULL || wm_obj_maker(f->wid) != pid ) {
			ok = TRUE;		/* another window's link: no disk is taken away with it */
		} else {
			ok = dt_disk_on_delete(f, v);
		}
		tad_vobj_clear(v);
		Kfree(v);
		return ok ? E_OK : E_BUSY;
	}
	case DT_DISK_RESCUE:	return rescue_do(rq, an, pid);
	default:		return E_PAR;
	}
}

/*
 * An item of the menu ディスク操作 chosen: the program ディスク操作 started
 * to do it (application/diskop), told the item's code and the link it
 * was chosen on.
 */
EXPORT void dt_disk_menu( DTWIN *d, CONST char *code, CONST T_VOBJ *v )
{
	T_DTDISKARG	a;
	CONST DTPROG	*p = dt_prog_find((CONST UB *)"disk-operation");
	INT		i;

	if ( p == NULL ) {
		(void)wm_msg_put("ディスク操作のプログラムがありません");
		return;
	}
	knl_memset(&a, 0, sizeof(a));
	for ( i = 0; code[i] != 0 && i < (INT)sizeof(a.code) - 1; i++ ) a.code[i] = (UB)code[i];
	if ( v != NULL ) {
		a.target = v->target;
	}
	tm_printf((UB *)"disk: %s asked of the program\n", code);
	(void)dt_prog_run_arg(d, p, &a, sizeof(a));
}

/*
 * At start: when the system volume was repaired before it was attached,
 * ディスク修復 is started to show what the repair found and did, once for
 * a start of the machine (a desktop started again by 強制終了 does not).
 */
EXPORT void dt_fix_boot( void )
{
	CONST DTPROG	*p;
	TS_UUID		u;

	if ( knl_fq_restarts() > 0 || ob_chk_boot_result(NULL, 0, NULL, NULL, 0) <= 0 ) {
		return;
	}
	if ( ts_str_to_uuid(SYSDEF_PROG_DISKFIX, &u) < E_OK || ( p = dt_prog_of(&u) ) == NULL ) {
		return;
	}
	tm_printf((UB *)"disk: the repair at start shown by its program\n");
	(void)dt_prog_run(NULL, p, NULL);
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
	UB	*name, *msg;
	TS_UUID	mine;
	SZ	size, asz;
	BOOL	got;

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
	if ( zero_uuid(&v->tvol) ) {
		/* the top of a disk remembered as attached: that disk's volume, though the link does not say it */
		TS_UUID	*kv, *kt;
		INT	nk = 0, i;
		T_VOBJ	*c = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ));

		kv = kept_vols(&nk, &kt);
		for ( i = 0; c != NULL && kv != NULL && i < nk; i++ ) {
			if ( ts_uuid_cmp(&kt[i], &v->target) == 0 ) {
				if ( tad_vobj_copy(c, v) >= E_OK ) {
					c->tvol = kv[i];
				}
				break;
			}
		}
		if ( kv != NULL ) Kfree(kv);
		if ( c != NULL && !zero_uuid(&c->tvol) ) {
			BOOL	r = dt_disk_absent(d, c);

			tad_vobj_clear(c);
			Kfree(c);
			return r;
		}
		if ( c != NULL ) {
			tad_vobj_clear(c);
			Kfree(c);
		}
	}
	if ( !zero_uuid(&v->tvol) && !knl_obfile_vuuid_on(&v->tvol) ) {
		/* the name the volume had when it was attached, whole; else what the link calls it, all of it */
		name = NULL;
		size = 0;
		got = FALSE;
		while ( knl_obfile_vol_nam(&v->tvol, name, size, &asz) >= E_OK && asz > 1 ) {
			if ( name != NULL && asz <= size ) {
				got = TRUE;
				break;
			}
			if ( name != NULL ) Kfree(name);
			size = asz;		/* the room it takes: renamed longer meanwhile, read again */
			name = (UB *)Kmalloc((size_t)size);
			if ( name == NULL ) {
				break;
			}
		}
		if ( name != NULL && ( !got || name[0] == 0 ) ) {
			Kfree(name);
			name = NULL;
		}
		if ( name == NULL ) {
			char	us[TS_UUID_STRLEN + 1];

			(void)ts_uuid_to_str(&v->tvol, us, sizeof(us));
			us[8] = 0;
			name = dt_cat(us, ( v->vname_len > 0 ) ? "、" : "",
				      ( v->vname_len > 0 ) ? (CONST char *)v->vname : "", NULL);
		}
		msg = ( name != NULL ) ? dt_cat("ボリューム「", (CONST char *)name, "」が接続されていません", NULL) : NULL;
		tm_printf((UB *)"disk: absent: volume \"%s\" not attached\n",
			  ( name != NULL ) ? (CONST char *)name : "?");
		dt_tell(d, ( msg != NULL ) ? (CONST char *)msg : "ボリュームが接続されていません",
			"ディスク操作の接続で接続してください");	/* the panel's line shows what fits */
		if ( name != NULL ) Kfree(name);
		if ( msg != NULL ) Kfree(msg);
		return TRUE;
	}
	tm_printf((UB *)"disk: absent: not found\n");
	dt_tell(d, "実身が見つかりません。", "そのボリュームが接続されていないか、実身が削除されています");
	return TRUE;
}
