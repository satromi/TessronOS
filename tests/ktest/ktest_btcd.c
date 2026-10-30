/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_btcd.c
 *	A BTRON CD-ROM read and its 実身 taken in by ファイル変換 (design 11.9)
 *
 *	The image tools/tests/mkbtcd.py makes (KT_BTCD_IMG) is put in a USB
 *	CD-ROM drive plugged into the machine for these tests (the QMP
 *	helper: drive_add with media=cdrom and a usb-storage on it, so that
 *	the drive answers as a CD-ROM of 2048 byte blocks). Then:
 *
 *	  - the drive is a disk object whose blocks are the disc's, and
 *	    lib/libbtfs finds on it the ISO 9660 volume partition descriptor,
 *	    the BTRON file system in that partition, every file of it, the
 *	    records at index levels 0, 1 and 2 and the runs a connection
 *	    index adds, and the link files;
 *	  - ファイル変換 lists the disc with the name of its file system,
 *	    opens it without mounting it, shows the 実身 the root links to,
 *	    goes into one that links to others and back, and takes them all
 *	    in: the text and the figure become xmlTAD 実身 linked to each
 *	    other, the 実行機能付箋 choose the programs that open them, the
 *	    マイクロカード実身 opens in マイクロカード, and the records of
 *	    other types are kept as they were;
 *	  - the drive taken out again.
 *
 *	What the accessory did is read from the console, where it says it.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <stdlib.h>			/* what lib/libbtfs gives is malloc's */
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/wm.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/btfs.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"

#ifndef KT_BTCD_IMG
#define KT_BTCD_IMG	""		/* no image: the drive is not plugged in */
#endif

#define LOG_MAX		( 128 * 1024 )
#define WIN_NAME	"ファイル変換"
#define FS_NAME		"試験CD"
#define CD_ID		"btcdusb"

#define K_ENTER		0x28
#define K_BS		0x2A
#define K_DOWN		0x51
#define K_UP		0x52
#define K_END		0x4D

LOCAL UB	*logb;
LOCAL char	cdname[16];		/* the drive's disk object, "udc" */
LOCAL TS_UUID	cdobj;
LOCAL BOOL	have_cd;

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL INT find( CONST UB *s, INT n, CONST char *w, INT from )
{
	INT	i, k, wl = s_len(w);

	for ( i = from; i + wl <= n; i++ ) {
		for ( k = 0; k < wl && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return i;
	}
	return -1;
}

LOCAL INT count( CONST UB *s, INT n, CONST char *w )
{
	INT	at = 0, c = 0;

	while ( ( at = find(s, n, w, at) ) >= 0 ) {
		c++;
		at += s_len(w);
	}
	return c;
}

LOCAL void cat3( char *out, CONST char *a, CONST char *b, CONST char *c )
{
	knl_strcpy(out, a);
	knl_strcat(out, b);
	knl_strcat(out, c);
}

/* ---------------------------------------------------------------- the console */

LOCAL INT log_from( UD pos, UB *buf, INT max )
{
	INT	n = 0, k;

	while ( n < max && ( k = tm_log_read(&pos, buf + n, max - n) ) > 0 ) n += k;
	return n;
}

LOCAL UD log_now( void )
{
	UD	pos = 0;

	while ( tm_log_read(&pos, logb, LOG_MAX) > 0 ) ;
	return pos;
}

/* Waited for (tenths of a second) until the console says w after pos; the rest of its line into line */
LOCAL BOOL wait_for( UD pos, CONST char *w, INT tries, char *line, INT max )
{
	INT	i, n, at, k;

	for ( i = 0; i < tries; i++ ) {
		n = log_from(pos, logb, LOG_MAX);
		at = find(logb, n, w, 0);
		if ( at >= 0 ) {
			if ( line != NULL ) {
				at += s_len(w);
				for ( k = 0; k < max - 1 && at + k < n && logb[at + k] != '\n'; k++ ) line[k] = (char)logb[at + k];
				line[k] = 0;
			}
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  waited in vain for \"%s\"\n", w);
	return FALSE;
}

/* The lines said after pos, each after the one before it */
LOCAL BOOL in_order( UD pos, CONST char **w )
{
	INT	n, at = 0, i;

	for ( i = 0; w[i] != NULL; i++ ) {
		if ( !wait_for(pos, w[i], 30, NULL, 0) ) return FALSE;
		n = log_from(pos, logb, LOG_MAX);
		at = find(logb, n, w[i], at);
		if ( at < 0 ) {
			tm_printf((UB *)"  \"%s\" came out of order\n", w[i]);
			return FALSE;
		}
	}
	return TRUE;
}

LOCAL BOOL same( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL uuid_after( CONST char *line, CONST char *w, TS_UUID *u )
{
	INT	at = find((CONST UB *)line, s_len(line), w, 0);
	char	s[37];

	if ( at < 0 ) return FALSE;
	at += s_len(w);
	knl_strncpy(s, line + at, 36);
	s[36] = 0;
	return (BOOL)( ts_str_to_uuid(s, u) >= E_OK );
}

/* ---------------------------------------------------------------- the drive */

/* The disk objects there are now */
LOCAL INT disks( TS_UUID *list, INT max )
{
	INT	cnt = 0;

	if ( ob_lst_obj(OB_T_DEVICE, OB_S_DISK, NULL, list, max, &cnt) < E_OK ) return 0;
	return ( cnt < max ) ? cnt : max;
}

typedef struct {
	ID	key;
	UD	size;
	UB	*buf;
} KRD;

#define KRD_SEC		2048

/* The disk's bytes, read through its object in whole blocks of 2048 */
LOCAL ER krd_read( void *ctx, D off, void *buf, UINT len )
{
	KRD	*r = (KRD *)ctx;
	UB	*o = (UB *)buf;
	SZ	asz = 0;

	while ( len > 0 ) {
		D	base = off & ~(D)( KRD_SEC - 1 );
		UINT	in = (UINT)( off - base ), n = KRD_SEC - in;

		if ( (UD)base + KRD_SEC > r->size ) return E_PAR;
		if ( ob_rea_rec(r->key, 1, base, r->buf, KRD_SEC, &asz) < E_OK || asz != KRD_SEC ) return E_IO;
		if ( n > len ) n = len;
		knl_memcpy(o, r->buf + in, n);
		o += n;
		off += n;
		len -= n;
	}
	return E_OK;
}

LOCAL BOOL krd_open( KRD *r, CONST TS_UUID *u )
{
	T_OBREC	rc[2];
	INT	nr = 0;

	knl_memset(r, 0, sizeof(*r));
	r->key = ob_opn_obj(u, OB_OP_R);
	if ( r->key <= 0 ) return FALSE;
	if ( ob_lst_rec(r->key, rc, 2, &nr) >= E_OK && nr >= 2 ) r->size = rc[1].size;
	r->buf = (UB *)Kmalloc(KRD_SEC);
	if ( r->buf == NULL || r->size == 0 ) {
		ob_cls_obj(r->key);
		if ( r->buf != NULL ) Kfree(r->buf);
		return FALSE;
	}
	return TRUE;
}

LOCAL void krd_close( KRD *r )
{
	ob_cls_obj(r->key);
	Kfree(r->buf);
}

/* Whether a disk object is a BTRON CD-ROM */
LOCAL BOOL is_btcd( CONST TS_UUID *u )
{
	KRD	r;
	D	off = 0, len = 0;
	BOOL	yes;

	if ( !krd_open(&r, u) ) return FALSE;
	yes = (BOOL)( btfs_cd_find(krd_read, &r, &off, &len, NULL) >= E_OK );
	krd_close(&r);
	return yes;
}

/*
 * The drive plugged in with the disc in it: a disk object of blocks of
 * 2048 bytes, the size of the image, on which the volume partition
 * descriptor is found
 */
LOCAL void test_attach( void )
{
	TS_UUID	list[64];
	T_OBREF	r;
	INT	i, k, n;

	for ( k = disks(list, 64), i = 0; i < k; i++ ) {
		if ( is_btcd(&list[i]) ) {
			have_cd = TRUE;
			cdobj = list[i];
		}
	}
	if ( !have_cd && KT_BTCD_IMG[0] != 0 ) {
		tm_printf((UB *)"KTEST QMP {\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":"
			  "\"drive_add 0 if=none,id=btcd,file=%s,format=raw,media=cdrom,readonly=on\"}}\n", KT_BTCD_IMG);
		tk_dly_tsk(300);
		tm_printf((UB *)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-storage\",\"id\":\""
			  CD_ID "\",\"bus\":\"xhci.0\",\"port\":\"4.4\",\"drive\":\"btcd\",\"removable\":true}}\n");
		for ( n = 0; n < 200 && !have_cd; n++ ) {
			tk_dly_tsk(100);
			for ( k = disks(list, 64), i = 0; i < k && !have_cd; i++ ) {
				if ( ob_ref_obj(&list[i], &r) < E_OK || r.name[0] != 'u' ) continue;
				if ( is_btcd(&list[i]) ) {
					have_cd = TRUE;
					cdobj = list[i];
				}
			}
		}
	}
	if ( !have_cd ) KT_SKIP("no CD-ROM drive came (the QMP helper is not there, or the image is not)");
	KT_ASSERT(ob_ref_obj(&cdobj, &r) >= E_OK);
	knl_strncpy(cdname, (CONST char *)r.name, sizeof(cdname) - 1);
	cdname[sizeof(cdname) - 1] = 0;
	tm_printf((UB *)"  the CD-ROM is %s\n", cdname);
	{
		KRD	d;
		D	off = 0, len = 0;
		BOOL	boot = TRUE;

		KT_ASSERT(krd_open(&d, &cdobj));
		KT_ASSERT_ER(btfs_cd_find(krd_read, &d, &off, &len, &boot), E_OK);
		KT_ASSERT_EQ(off, 28 * 2048);
		KT_ASSERT(len > 0 && (UD)( off + len ) <= d.size);
		KT_ASSERT(!boot);
		/* a CD-ROM is read in blocks of 2048: half of one is refused */
		{
			SZ	asz = 0;

			KT_ASSERT(ob_rea_rec(d.key, 1, 512, d.buf, 512, &asz) < E_OK);
		}
		krd_close(&d);
	}
}

/* ---------------------------------------------------------------- the file system */

/* A record's bytes read whole, freed with free */
LOCAL UB *rec_bytes( BTFS *fs, CONST BTFSREC *q )
{
	UB	*b = (UB *)malloc(q->size ? q->size : 1);

	if ( b != NULL && btfs_rec_read(fs, q, b) < E_OK ) {
		free(b);
		b = NULL;
	}
	return b;
}

/*
 * The file system: its name, form and files; a text through index
 * level 1 whose record is in runs joined by a connection index; one
 * through level 2 from where the extended form 2 starts its indexes;
 * the link files; and the archive of the root and what it links to
 */
LOCAL void test_read( void )
{
	KRD		d;
	BTFS		fs;
	BTFSFILE	f;
	D		off = 0, len = 0;
	UB		*b;
	INT		i, n, a;

	if ( !have_cd ) KT_SKIP("no CD-ROM");
	KT_ASSERT(krd_open(&d, &cdobj));
	KT_ASSERT_ER(btfs_cd_find(krd_read, &d, &off, &len, NULL), E_OK);
	KT_ASSERT_ER(btfs_open(&fs, krd_read, &d, off, len), E_OK);
	KT_ASSERT(knl_strcmp(fs.uname, FS_NAME) == 0);
	KT_ASSERT_EQ(fs.bsize, 2048);
	KT_ASSERT(fs.le);
	KT_ASSERT_EQ(fs.format, BTFS_FMT_EXT2);
	KT_ASSERT_EQ(fs.base, off);
	for ( i = 0, n = 0; i < (INT)fs.nfmax; i++ ) {
		if ( btfs_fid_used(&fs, (UINT)i) ) n++;
	}
	KT_ASSERT_EQ(n, 9);

	/* the root: a figure and six link records, each to the file of its ID */
	KT_ASSERT_ER(btfs_file(&fs, 0, &f), E_OK);
	KT_ASSERT(knl_strcmp(f.uname, FS_NAME) == 0);
	KT_ASSERT_EQ(f.nrec, 7);
	KT_ASSERT_EQ(f.nlink, 6);
	for ( i = 1; i < f.nrec; i++ ) KT_ASSERT_EQ(btfs_link_target(&fs, &f.rec[i]), i);
	btfs_file_free(&f);

	/* 大きな実身: index level 1, 131 records, the text in runs of two blocks */
	KT_ASSERT_ER(btfs_file(&fs, 4, &f), E_OK);
	KT_ASSERT(knl_strcmp(f.uname, "大きな実身") == 0);
	KT_ASSERT_EQ(f.idxlv, 1);
	KT_ASSERT_EQ(f.nrec, 131);
	if ( f.nrec == 131 ) {
		KT_ASSERT_EQ(f.rec[0].type, 1);
		KT_ASSERT_EQ(f.rec[0].size, 18932);
		KT_ASSERT_EQ(f.rec[0].nrun, 5);
		b = rec_bytes(&fs, &f.rec[0]);
		KT_ASSERT(b != NULL);
		if ( b != NULL ) {
			/* the text: あ (2422) at the start of each of its 450 lines */
			for ( i = 0, a = 0; i + 1 < (INT)f.rec[0].size; i += 2 ) {
				if ( b[i] == 0x22 && b[i + 1] == 0x24 ) a++;
			}
			KT_ASSERT_EQ(a, 450);
			KT_ASSERT(b[0] == 0xE1 && b[1] == 0xFF);
			free(b);
		}
		for ( i = 1, n = 0; i < f.nrec; i++ ) {
			if ( f.rec[i].type == 5 && f.rec[i].sub == 7 && f.rec[i].size == 18 ) n++;
		}
		KT_ASSERT_EQ(n, 130);
		b = rec_bytes(&fs, &f.rec[130]);
		KT_ASSERT(b != NULL && same(b, "未知129", 9));
		if ( b != NULL ) free(b);
	}
	btfs_file_free(&f);

	/* 多段の実身: index level 2, its indexes from byte 256 */
	KT_ASSERT_ER(btfs_file(&fs, 5, &f), E_OK);
	KT_ASSERT_EQ(f.idxlv, 2);
	KT_ASSERT_EQ(f.nrec, 2);
	if ( f.nrec == 2 ) {
		b = rec_bytes(&fs, &f.rec[1]);
		KT_ASSERT(b != NULL && f.rec[1].size == 16 && same(b, "KEEP-ME-AS-IT-IS", 16));
		if ( b != NULL ) free(b);
	}
	btfs_file_free(&f);

	/* the link files: one to another file system, one to 図１ of this one */
	KT_ASSERT_ER(btfs_file(&fs, 7, &f), E_OK);
	KT_ASSERT(f.islink && f.target < 0);
	btfs_file_free(&f);
	KT_ASSERT_ER(btfs_file(&fs, 8, &f), E_OK);
	KT_ASSERT(f.islink && f.target == 2);
	btfs_file_free(&f);
	KT_ASSERT_ER(btfs_file(&fs, 9, &f), E_NOEXS);

	/* the root and what it links to, as one archive that reads back */
	{
		UB	*raw = NULL;
		UINT	rl = 0;
		char	err[120];
		BPKARC	arc;

		KT_ASSERT_ER(btfs_book(&fs, 0, &raw, &rl, &n, err, sizeof(err)), E_OK);
		KT_ASSERT_EQ(n, 7);
		if ( raw != NULL ) {
			KT_ASSERT_ER(bpk_parse(&arc, raw, rl, err, sizeof(err)), E_OK);
			KT_ASSERT_EQ(arc.nfiles, 7);
			if ( arc.nfiles == 7 ) {
				KT_ASSERT_EQ(arc.file[0].nlink, 6);
				for ( i = 0; i < 6 && i < arc.file[0].nlink; i++ ) KT_ASSERT_EQ(arc.file[0].link[i], i + 1);
				KT_ASSERT(bpk_mc_file(&arc.file[6]));
				KT_ASSERT_EQ(arc.file[2].nexec, 1);
			}
			bpk_free(&arc);
			free(raw);
		}
	}
	btfs_close(&fs);
	krd_close(&d);
}

/* ---------------------------------------------------------------- ファイル変換 */

LOCAL INT window_of( CONST char *name, TS_UUID *out )
{
	TS_UUID	list[64];
	T_OBREF	r;
	INT	cnt = 0, i, wid = -1, p;
	ID	kw;

	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) < E_OK ) return -1;
	for ( i = 0; i < cnt && i < 64; i++ ) {
		if ( ob_ref_obj(&list[i], &r) < E_OK || r.sub != OB_S_WINDOW
		  || find(r.name, s_len((CONST char *)r.name), name, 0) < 0 ) continue;
		*out = list[i];
		kw = ob_opn_obj(out, OB_OP_R);
		if ( kw > 0 ) {
			UB	j[256];
			SZ	asz = 0;
			D	v = 0;

			if ( ob_get_atr(kw, j, sizeof(j) - 1, &asz) >= E_OK ) {
				p = knl_oj_path(j, (INT)asz, "tessronos", "window");
				p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "number") : -1;
				if ( p >= 0 && knl_oj_num(j, (INT)asz, p, &v) ) wid = (INT)v;
			}
			ob_cls_obj(kw);
		}
	}
	return wid;
}

LOCAL INT wait_window( CONST char *name, TS_UUID *out, INT tries )
{
	INT	i, wid = -1;

	for ( i = 0; i < tries && wid <= 0; i++ ) {
		wid = window_of(name, out);
		if ( wid <= 0 ) tk_dly_tsk(100);
	}
	if ( wid > 0 ) tk_dly_tsk(200);
	return wid;
}

LOCAL void press( INT wid, UINT code, UINT mods )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.wid = wid;
	ev.code = code;
	ev.mods = mods;
	knl_wmobj_event(&ev);
	tk_dly_tsk(80);
}

LOCAL void type( INT wid, CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		char	c = *s;

		if ( c >= 'a' && c <= 'z' ) press(wid, 0x04 + ( c - 'a' ), 0);
		else if ( c >= '1' && c <= '9' ) press(wid, 0x1E + ( c - '1' ), 0);
		else if ( c == '0' ) press(wid, 0x27, 0);
	}
}

LOCAL void ctrl( INT wid, char c )
{
	press(wid, 0x04 + ( c - 'a' ), HID_MOD_LCTRL);
}

/* The panel of that name answered with Enter, its first choices as they are */
LOCAL BOOL answer_panel( CONST char *name )
{
	TS_UUID	u;
	INT	wid = wait_window(name, &u, 50);

	if ( wid <= 0 ) return FALSE;
	press(wid, K_ENTER, 0);
	return TRUE;
}

/* An object, and what it links to that nothing else does, thrown away */
LOCAL void drop( CONST TS_UUID *u, INT depth )
{
	TS_UUID	ids[16];
	T_OBREF	r;
	UB	*rec;
	SZ	size = 0;
	INT	n = 0, i;
	ER	er;

	rec = om_obj_record(u, 0, &size);
	if ( rec != NULL ) {
		n = om_store_links(rec, size, ids, 16);
		Kfree(rec);
	}
	er = ob_del_obj(u);
	for ( i = 0; er == E_OBJ && i < 8; i++ ) {
		(void)ob_unl_obj(u);
		er = ob_del_obj(u);
	}
	for ( i = 0; i < n && i < 16 && depth > 0; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) >= E_OK && r.refcnt == 0 ) drop(&ids[i], depth - 1);
	}
}

/*
 * The box a 取込 made taken out of the person's cabinet and thrown away with
 * everything it reaches. 実身 taken in link to each other (はじめに and
 * 図１ to each other), so that none of them would ever come to no link:
 * the links of each are taken out first, then each is deleted.
 */
#define DROP_MAX	32

LOCAL void drop_box( CONST TS_UUID *box )
{
	TS_UUID	home, *all, ids[16];
	UB	*rec;
	SZ	size = 0, asz = 0;
	INT	n = 0, i, j, k, m;
	ID	key;
	static CONST char none[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p></p></document></tad>\n";

	if ( ts_str_to_uuid(SYSDEF_CABINET, &home) >= E_OK ) (void)om_obj_link_del(&home, box);
	all = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * DROP_MAX);
	if ( all == NULL ) {
		drop(box, 6);
		return;
	}
	all[n++] = *box;
	for ( i = 0; i < n; i++ ) {
		rec = om_obj_record(&all[i], 0, &size);
		if ( rec == NULL ) continue;
		m = om_store_links(rec, size, ids, 16);
		Kfree(rec);
		for ( k = 0; k < m && k < 16 && n < DROP_MAX; k++ ) {
			for ( j = 0; j < n && !same(&all[j], &ids[k], sizeof(TS_UUID)); j++ ) ;
			if ( j == n ) all[n++] = ids[k];
		}
	}
	for ( i = 0; i < n; i++ ) {
		key = ob_opn_obj(&all[i], OB_OP_R | OB_OP_W);
		if ( key <= 0 ) continue;
		(void)ob_wri_rec(key, 0, 0, none, sizeof(none) - 1, &asz);
		(void)ob_trn_rec(key, 0, sizeof(none) - 1);
		ob_cls_obj(key);
	}
	for ( i = 0; i < n; i++ ) drop(&all[i], 0);
	Kfree(all);
}

/* Record 0 of an object, and the UUID of its n-th link (from 0) */
LOCAL BOOL link_n( CONST TS_UUID *u, INT n, TS_UUID *out )
{
	TS_UUID	ids[16];
	UB	*rec;
	SZ	size = 0;
	INT	k = 0;

	rec = om_obj_record(u, 0, &size);
	if ( rec == NULL ) return FALSE;
	k = om_store_links(rec, size, ids, 16);
	Kfree(rec);
	if ( n >= k ) return FALSE;
	*out = ids[n];
	return TRUE;
}

/* Whether record 0 of an object says w */
LOCAL BOOL says( CONST TS_UUID *u, CONST char *w )
{
	UB	*rec;
	SZ	size = 0;
	BOOL	yes;

	rec = om_obj_record(u, 0, &size);
	if ( rec == NULL ) return FALSE;
	yes = (BOOL)( find(rec, (INT)size, w, 0) >= 0 );
	Kfree(rec);
	return yes;
}

LOCAL BOOL meta_says( CONST TS_UUID *u, CONST char *w )
{
	UB	*m;
	SZ	size = 0;
	BOOL	yes;

	m = om_obj_meta(u, &size);
	if ( m == NULL ) return FALSE;
	yes = (BOOL)( find(m, (INT)size, w, 0) >= 0 );
	Kfree(m);
	return yes;
}

/* How many records of an object are of type rt and subtype sub */
LOCAL INT recs_of( CONST TS_UUID *u, UINT rt, UINT sub )
{
	T_OBREC	*r = (T_OBREC *)Kmalloc(sizeof(T_OBREC) * 200);
	INT	n, i, c = 0;

	if ( r == NULL ) return -1;
	n = om_obj_records(u, r, 200);
	for ( i = 0; i < n; i++ ) {
		if ( r[i].rt == rt && r[i].sub == sub ) c++;
	}
	Kfree(r);
	return c;
}

LOCAL ID start( TS_UUID *p_win, INT *p_wid )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	UD	pos = log_now();
	ID	pid;

	*p_wid = -1;
	if ( ts_str_to_uuid(SYSDEF_PROG_XFCONV, &prog) < E_OK ) return E_SYS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	if ( ob_cre_obj(&c, &pu) < E_OK ) return E_OBJ;
	pid = knl_prc_of_uuid(&pu);
	if ( pid <= 0 || !wait_for(pos, "xfconv: ready", 100, NULL, 0) ) return E_TMOUT;
	*p_wid = wait_window(WIN_NAME, p_win, 30);
	return pid;
}

LOCAL void finish( ID pid, CONST TS_UUID *win )
{
	T_PSTS	psts;

	(void)ob_del_obj(win);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
}

/*
 * The disc listed with the name of its file system and opened without
 * mounting it: the 実身 the root links to, in their order; 資料 gone
 * into and back; all six taken in into a new box, each with what it
 * links to, and what they became looked at
 */
LOCAL void test_xfconv( void )
{
	TS_UUID	win, fwin, box, top[8], u, v;
	T_OBREF	r;
	char	line[256];
	UD	pos;
	INT	wid, fwid, k;
	ID	pid;

	if ( !have_cd ) KT_SKIP("no CD-ROM");
	if ( ts_str_to_uuid(SYSDEF_PROG_XFCONV, &win) < E_OK || ob_ref_obj(&win, &r) < E_OK ) {
		KT_SKIP("no ファイル変換 program");
	}
	pos = log_now();
	pid = start(&win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) return;
	cat3(line, "xfconv: media ", cdname, " ");
	KT_ASSERT(wait_for(pos, line, 30, NULL, 0));
	KT_ASSERT(wait_for(pos, " btron " FS_NAME, 30, NULL, 0));

	/* opened: not mounted, the root's links listed in their order */
	pos = log_now();
	type(wid, cdname);
	press(wid, K_ENTER, 0);
	KT_ASSERT(wait_for(pos, "xfconv: btron " FS_NAME " cdrom block 2048 semi-standard extended", 100, NULL, 0));
	cat3(line, "xfconv: dir ", cdname, " 7 entries");
	KT_ASSERT(wait_for(pos, line, 100, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: iso ", 30, NULL, 0));
	{
		/* the ISO 9660 side's folder after the 実身 */
		static CONST char *root[] = { "xfconv: - はじめに/ 126", "xfconv: - 図1/ 226", "xfconv: - 資料/ 318",
					      "xfconv: - 大きな実身 21272", "xfconv: - 多段の実身 70",
					      "xfconv: - 住所カード/ 96", "xfconv: - ISO 9660/ -1", NULL };

		KT_ASSERT(in_order(pos, root));
	}
	KT_ASSERT(find(logb, log_from(pos, logb, LOG_MAX), "xfconv: opened disk", 0) < 0);
	fwid = wait_window(FS_NAME, &fwin, 30);
	KT_ASSERT(fwid > 0);
	if ( fwid <= 0 ) {
		finish(pid, &win);
		return;
	}

	/* 資料 gone into: the link file to another file system is left out; then back */
	pos = log_now();
	press(fwid, K_DOWN, 0);
	press(fwid, K_DOWN, 0);
	press(fwid, K_DOWN, 0);
	KT_ASSERT(wait_for(pos, "xfconv: selected 1: 資料", 30, NULL, 0));
	press(fwid, K_ENTER, 0);
	cat3(line, "xfconv: dir ", cdname, "/資料 4 entries");
	KT_ASSERT(wait_for(pos, line, 50, NULL, 0));
	{
		static CONST char *docs[] = { "xfconv: - はじめに/ 126", "xfconv: - 大きな実身 21272",
					      "xfconv: - 多段の実身 70", "xfconv: - 図1/ 226", NULL };

		KT_ASSERT(in_order(pos, docs));
	}
	pos = log_now();
	press(fwid, K_BS, 0);
	cat3(line, "xfconv: dir ", cdname, " 7 entries");
	KT_ASSERT(wait_for(pos, line, 50, NULL, 0));

	/* all six taken in, with no panel to answer: 14 実身 in all, each with what it links to */
	pos = log_now();
	press(fwid, K_END, 0);
	press(fwid, K_UP, 0);
	for ( k = 0; k < 5; k++ ) press(fwid, K_UP, HID_MOD_LSHIFT);
	KT_ASSERT(wait_for(pos, "xfconv: selected 6: はじめに, 図1, 資料, 大きな実身, 多段の実身, 住所カード", 30, NULL, 0));
	ctrl(fwid, 'i');
	KT_ASSERT(wait_for(pos, "xfconv: imported 6 of 6 objects 14 box ", 600, line, sizeof(line)));
	KT_ASSERT(find(logb, log_from(pos, logb, LOG_MAX), "xfconv: asked how", 0) < 0);
	if ( !uuid_after(line, "", &box) ) {
		KT_ASSERT(FALSE);
		finish(pid, &win);
		return;
	}
	for ( k = 0; k < 6; k++ ) KT_ASSERT(link_n(&box, k, &top[k]));

	/* はじめに: a text, its 仮身 to 図１, which links back to it */
	KT_ASSERT(says(&top[0], "<document>"));
	KT_ASSERT(says(&top[0], "BTRONのCD−ROMの試験です。"));
	KT_ASSERT(meta_says(&top[0], "\"name\":\"はじめに\""));
	KT_ASSERT(meta_says(&top[0], "\"maker\":\"ファイル変換（CD-ROM「" FS_NAME "」）\""));
	KT_ASSERT(link_n(&top[0], 0, &u));
	KT_ASSERT(says(&u, "<figure>") && says(&u, "<rect "));
	KT_ASSERT(meta_says(&u, "\"name\":\"図1\""));
	KT_ASSERT(meta_says(&u, "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":true}"));
	KT_ASSERT(link_n(&u, 0, &v) && same(&v, &top[0], sizeof(v)));
	KT_ASSERT(ob_ref_obj(&top[0], &r) >= E_OK && r.refcnt == 2);	/* the box and 図１ */

	/* 資料: 仮身一覧, four links (one to 図１ through a link file), the fifth gone nowhere */
	KT_ASSERT(meta_says(&top[2], "\"virtual-object-list\":{\"name\":\"仮身一覧\",\"defaultOpen\":true}"));
	KT_ASSERT(link_n(&top[2], 3, &u) && says(&u, "filename=\"図1\""));
	KT_ASSERT(!link_n(&top[2], 4, &u));

	/* 大きな実身: the long text whole, the 130 records of an unknown type kept as type 15 (more than 64) */
	{
		UB	*rec;
		SZ	size = 0;

		rec = om_obj_record(&top[3], 0, &size);
		KT_ASSERT(rec != NULL && count(rec, (INT)size, "あいうえおかきくけこさしすせそたちつてと") == 450);
		if ( rec != NULL ) Kfree(rec);
	}
	KT_ASSERT_EQ(recs_of(&top[3], OB_RT_SYSDATA, 7), 130);

	/* 多段の実身: its text through index level 2, and its record of type 5 */
	KT_ASSERT(says(&top[4], "段を重ねた索引の実身。"));
	KT_ASSERT_EQ(recs_of(&top[4], OB_RT_SYSDATA, 3), 1);

	/* 住所カード: a マイクロカード実身, record 0 naming its link, its type 31 records kept */
	KT_ASSERT(says(&top[5], "<mclinks/><link id=\""));
	KT_ASSERT(meta_says(&top[5], "\"tessronos-microcard\":{\"name\":\"マイクロカード\",\"defaultOpen\":true}"));
	KT_ASSERT_EQ(recs_of(&top[5], OB_RT_SYSDATA, 1), 1);
	KT_ASSERT_EQ(recs_of(&top[5], OB_RT_SYSDATA, 2), 1);

	drop_box(&box);

	/*
	 * The ISO 9660 side: its folder holds the files beside the partition
	 * (the partition's own image left out), by their Joliet names; a file
	 * taken in after the panel as a file of a mounted disk is, and a
	 * directory gone into and back
	 */
	pos = log_now();
	press(fwid, K_END, 0);
	KT_ASSERT(wait_for(pos, "xfconv: selected 1: ISO 9660", 30, NULL, 0));
	press(fwid, K_ENTER, 0);
	cat3(line, "xfconv: dir ", cdname, "/ISO 9660 2 entries");
	KT_ASSERT(wait_for(pos, line, 50, NULL, 0));
	{
		static CONST char *iso[] = { "xfconv: - 文書/ -1", "xfconv: - はじめにお読みください.txt 52", NULL };

		KT_ASSERT(in_order(pos, iso));
	}
	pos = log_now();
	press(fwid, K_END, 0);
	KT_ASSERT(wait_for(pos, "xfconv: selected 1: はじめにお読みください.txt", 30, NULL, 0));
	ctrl(fwid, 'i');
	KT_ASSERT(wait_for(pos, "xfconv: asked how to import", 50, NULL, 0));
	KT_ASSERT(answer_panel("実身への変換"));
	KT_ASSERT(wait_for(pos, "xfconv: imported 1 of 1 objects 1 box ", 300, line, sizeof(line)));
	if ( uuid_after(line, "", &box) ) {
		KT_ASSERT(link_n(&box, 0, &u));
		KT_ASSERT(meta_says(&u, "\"name\":\"はじめにお読みください.txt\""));
		KT_ASSERT(says(&u, "This disc holds a BTRON partition"));
		drop_box(&box);
	} else {
		KT_ASSERT(FALSE);
	}
	pos = log_now();
	press(fwid, K_UP, 0);
	press(fwid, K_ENTER, 0);
	cat3(line, "xfconv: dir ", cdname, "/ISO 9660/文書 1 entries");
	KT_ASSERT(wait_for(pos, line, 50, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: - メモ.txt 34", 30, NULL, 0));
	pos = log_now();
	press(fwid, K_BS, 0);
	press(fwid, K_BS, 0);
	cat3(line, "xfconv: dir ", cdname, " 7 entries");
	KT_ASSERT(wait_for(pos, line, 50, NULL, 0));

	/* closed: nothing was mounted, the disc may come out */
	pos = log_now();
	ctrl(fwid, 'e');
	KT_ASSERT(wait_for(pos, "xfconv: closed btron " FS_NAME, 50, NULL, 0));
	KT_ASSERT(find(logb, log_from(pos, logb, LOG_MAX), "xfconv: closed disk", 0) < 0);
	finish(pid, &win);
}

/* The drive taken out: its disk object goes */
LOCAL void test_detach( void )
{
	T_OBREC	rc[2];
	INT	i, nr = 0;
	ID	k;
	BOOL	gone = FALSE;

	if ( !have_cd ) KT_SKIP("no CD-ROM");
	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"" CD_ID "\"}}\n");
	for ( i = 0; i < 100 && !gone; i++ ) {
		tk_dly_tsk(100);
		k = ob_opn_obj(&cdobj, OB_OP_ATRRD);
		if ( k <= 0 ) {
			gone = TRUE;
			break;
		}
		nr = 0;
		if ( ob_lst_rec(k, rc, 2, &nr) >= E_OK && ( nr < 2 || rc[1].size == 0 ) ) gone = TRUE;
		ob_cls_obj(k);
	}
	KT_ASSERT(gone);
	have_cd = FALSE;
}

EXPORT void ktest_btcd( void )
{
	logb = (UB *)Kmalloc(LOG_MAX);
	if ( logb == NULL ) return;
	KT_RUN(test_attach);
	KT_RUN(test_read);
	KT_RUN_SCREEN(test_xfconv);
	KT_RUN(test_detach);
	Kfree(logb);
}
