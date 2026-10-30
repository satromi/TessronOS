/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_arc.c
 *	書庫管理 (design 11.20): books written and read back
 *
 *	In the kernel: the LH5 encoder against the decoder, the sample
 *	archive BTRON wrote read and written again into the very same
 *	stream, and archives broken in each way refused or found out.
 *
 *	Then the program, started with the argument that asks for work
 *	without its window: networks of objects -- a figure with a picture
 *	and links to a text and a document, the text linking back; a
 *	hierarchy five deep with a shared object and a cycle; a large
 *	object; an archive inside -- packed into an archive object, the book
 *	read here, and taken out again into a cabinet. What comes out is
 *	compared with what went in: the names, the times, the programs, the
 *	window, record 0 as the binary TAD it is written as, the other
 *	records byte for byte, the links joined to the new objects. A book
 *	is stored as well as compressed, refused when the archive has one,
 *	deleted, copied from another archive, and the sample archive is
 *	copied in, taken out and packed again.
 *
 *	An empty archive made as the template 空書庫 makes one, filled with
 *	a network and taken out elsewhere, gives back each object with the
 *	programs of its applist and the one it opens with; and a book whose
 *	links name programs of their own (VLINK.appl) gives the objects they
 *	point at those programs to open with.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/bpk.h>
#include <ts/btbk.h>
#include <ts/om.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../application/archive/arc_arg.h"
#include <stdlib.h>

IMPORT CONST UB kt_bk_png[], kt_bk_png_end[];

#define SAMPLE		"01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1"
#define SAMPLE_NOBJ	33
#define LOG_MAX		( 64 * 1024 )
#define OTHER_LEN	100
#define MADE_MAX	48
#define BIG_TEXT	( 120 * 1024 )		/* letters of the large text */
#define BIG_REC		( 2 * 1024 * 1024 )	/* bytes of the large record */
#define DATE_MAKE	"2025-10-18T01:02:03Z"
#define DATE_UPDATE	"2026-09-30T12:34:56Z"

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL INT find( CONST UB *s, INT n, CONST char *w, INT from )
{
	INT	i, k, m = s_len(w);

	for ( i = from; i + m <= n; i++ ) {
		for ( k = 0; k < m && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return i;
	}
	return -1;
}

LOCAL BOOL has( CONST UB *s, INT n, CONST char *w )
{
	return (BOOL)( find(s, n, w, 0) >= 0 );
}

LOCAL BOOL same( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (BOOL)( a[i] == b[i] );
}

LOCAL INT log_from( UD *pos, UB *buf, INT max )
{
	INT	n = 0, k;

	while ( n < max && ( k = tm_log_read(pos, buf + n, max - n) ) > 0 ) n += k;
	return n;
}

/* A whole record, in memory to free; NULL when there is none */
LOCAL UB *rec_read( CONST TS_UUID *u, INT recno, INT *p_len, UINT *p_rt, UINT *p_sub )
{
	T_OBREC	r[32];
	INT	cnt = 0, i, size = -1;
	SZ	asz = 0;
	D	at = 0;
	UB	*buf;
	ID	k = ob_opn_obj(u, OB_OP_R);

	*p_len = 0;
	if ( k <= 0 ) return NULL;
	if ( ob_lst_rec(k, r, 32, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 32; i++ ) {
			if ( r[i].recno == recno ) {
				size = (INT)r[i].size;
				if ( p_rt != NULL ) *p_rt = r[i].rt;
				if ( p_sub != NULL ) *p_sub = r[i].sub;
			}
		}
	}
	buf = ( size >= 0 ) ? (UB *)Kmalloc((SZ)size + 1) : NULL;
	while ( buf != NULL && at < (D)size ) {
		if ( ob_rea_rec(k, recno, at, buf + at, (SZ)size - (SZ)at, &asz) < E_OK || asz <= 0 ) break;
		at += asz;
	}
	ob_cls_obj(k);
	if ( buf != NULL && at != (D)size ) {
		Kfree(buf);
		buf = NULL;
	}
	if ( buf != NULL ) {
		buf[size] = 0;
		*p_len = size;
	}
	return buf;
}

LOCAL INT nrec_of( CONST TS_UUID *u )
{
	T_OBREC	r[32];
	INT	cnt = 0;
	ID	k = ob_opn_obj(u, OB_OP_R);

	if ( k <= 0 ) return -1;
	(void)ob_lst_rec(k, r, 32, &cnt);
	ob_cls_obj(k);
	return cnt;
}

/* The record of plain data that holds an archive, -1 none */
LOCAL INT book_rec( CONST TS_UUID *u )
{
	T_OBREC	r[32];
	INT	cnt = 0, i, at = -1;
	UB	head[2];
	SZ	asz = 0;
	ID	k = ob_opn_obj(u, OB_OP_R);

	if ( k <= 0 ) return -1;
	(void)ob_lst_rec(k, r, 32, &cnt);
	for ( i = 0; i < cnt && i < 32 && at < 0; i++ ) {
		if ( r[i].rt == OB_RT_SYSDATA && r[i].size > 2 && ob_rea_rec(k, r[i].recno, 0, head, 2, &asz) >= E_OK
		  && head[0] == 0xE0 && head[1] == 0xFF ) at = r[i].recno;
	}
	ob_cls_obj(k);
	return at;
}

LOCAL INT links_of( CONST UB *x, INT n, TS_UUID *out, INT max )
{
	INT	at = 0, k = 0;

	while ( k < max && ( at = find(x, n, "<link id=\"", at) ) >= 0 ) {
		char	s[37];
		INT	i;

		at += 10;
		for ( i = 0; i < 36 && at + i < n; i++ ) s[i] = (char)x[at + i];
		s[i] = 0;
		if ( ts_str_to_uuid(s, &out[k]) >= E_OK ) k++;
	}
	return k;
}

/* The metadata of an object, to free; NULL none */
LOCAL UB *meta_of( CONST TS_UUID *u, INT *p_len )
{
	UB	*j = (UB *)Kmalloc(KT_META_ROOM + 1);
	SZ	asz = 0;
	ID	key = ob_opn_obj(u, OB_OP_ATRRD);

	*p_len = 0;
	if ( j != NULL && key > 0 && ob_get_atr(key, j, KT_META_ROOM, &asz) >= E_OK ) {
		j[asz] = 0;
		*p_len = (INT)asz;
	} else if ( j != NULL ) {
		Kfree(j);
		j = NULL;
	}
	if ( key > 0 ) ob_cls_obj(key);
	return j;
}

/* A string member of the metadata, "" when it is not there */
LOCAL void meta_str( CONST UB *m, INT n, CONST char *key, char *out, INT max )
{
	char	k[48];
	INT	at, i = 0;

	tm_sprintf((UB *)k, (UB *)"\"%s\":\"", key);
	at = find(m, n, k, 0);
	out[0] = 0;
	if ( at < 0 ) return;
	for ( at += s_len(k); at < n && m[at] != '"' && i < max - 1; at++ ) out[i++] = (char)m[at];
	out[i] = 0;
}

LOCAL INT refcnt_of( CONST TS_UUID *u )
{
	T_OBREF	r;

	return ( ob_ref_obj(u, &r) >= E_OK ) ? r.refcnt : -1;
}

/* Every object linked from a root, once each, into out[] */
LOCAL INT reach( CONST TS_UUID *root, TS_UUID *out, INT max )
{
	INT	n = 1, i, k, len, m;
	TS_UUID	*l = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 64);
	UB	*x;

	out[0] = *root;
	for ( i = 0; i < n && l != NULL; i++ ) {
		x = rec_read(&out[i], 0, &len, NULL, NULL);
		if ( x == NULL ) continue;
		m = links_of(x, len, l, 64);
		for ( k = 0; k < m; k++ ) {
			INT	j;

			for ( j = 0; j < n && ts_uuid_cmp(&out[j], &l[k]) != 0; j++ ) ;
			if ( j == n && n < max ) out[n++] = l[k];
		}
		Kfree(x);
	}
	if ( l != NULL ) Kfree(l);
	return n;
}

/* The links of an object given up: its xmlTAD records emptied, its link table made again on close */
LOCAL void unlink_all( CONST TS_UUID *u )
{
	T_OBREC	r[32];
	INT	cnt = 0, i;
	ID	k = ob_opn_obj(u, OB_OP_ALL);

	if ( k <= 0 ) return;
	if ( ob_lst_rec(k, r, 32, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 32; i++ ) {
			if ( r[i].rt == OB_RT_TAD ) (void)ob_trn_rec(k, r[i].recno, 0);
		}
	}
	ob_cls_obj(k);
}

/*
 * Objects that link to one another (a text back to its figure, a cycle)
 * keep each other's count above zero, and deleting them one by one would
 * leave them as 隠れ屑実身 on the system volume: their links go first.
 */
LOCAL void unmake( TS_UUID *u, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) unlink_all(&u[i]);
	for ( i = n - 1; i >= 0; i-- ) (void)ob_del_obj(&u[i]);
}

/* ---------------------------------------------------------------- objects */

LOCAL TS_UUID	cab0;

/* An object: its name, its applist, its times and window, record 0 */
LOCAL ER make_apps( CONST char *name, CONST char *apps, CONST char *xml, TS_UUID *u )
{
	char	*json = (char *)Kmalloc(1024);
	T_OBCRE	c;
	INT	rec = -1;
	SZ	asz = 0;
	ID	k;
	ER	er;

	if ( json == NULL ) return E_NOMEM;
	tm_sprintf((UB *)json, (UB *)"{\"name\":\"%s\",\"makeDate\":\"" DATE_MAKE "\",\"updateDate\":\"" DATE_UPDATE "\","
		   "\"accessDate\":\"" DATE_UPDATE "\",\"periodDate\":null,\"editable\":true,\"deletable\":true,"
		   "\"applist\":%s,"
		   "\"window\":{\"pos\":{\"x\":110,\"y\":90},\"width\":500,\"height\":300,\"backgroundColor\":\"#ffeecc\"}}",
		   name, apps);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	c.near = cab0;
	er = ob_cre_obj(&c, u);
	Kfree(json);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(u, OB_OP_ALL);
	if ( k <= 0 ) return E_NOEXS;
	er = ob_apd_rec(k, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK && xml != NULL ) er = ob_wri_rec(k, rec, 0, xml, s_len(xml), &asz);
	ob_cls_obj(k);
	return er;
}

/* An object the program app opens */
LOCAL ER make( CONST char *name, CONST char *app, CONST char *xml, TS_UUID *u )
{
	char	apps[96];

	tm_sprintf((UB *)apps, (UB *)"{\"%s\":{\"name\":\"x\",\"defaultOpen\":true}}", app);
	return make_apps(name, apps, xml, u);
}

LOCAL ER rewrite0( CONST TS_UUID *u, CONST char *xml, INT len )
{
	SZ	asz = 0;
	ID	k = ob_opn_obj(u, OB_OP_ALL);
	ER	er;

	if ( k <= 0 ) return E_NOEXS;
	er = ob_wri_rec(k, 0, 0, xml, len, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(k, 0, (UD)len);
	ob_cls_obj(k);
	return er;
}

/* An empty archive object, as the template 空書庫 makes one */
LOCAL ER make_archive( CONST char *name, TS_UUID *u )
{
	return make(name, ARC_PROG_ID, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", u);
}

/* A link in xmlTAD to u, in a figure, at the row given */
LOCAL INT put_link( char *out, CONST TS_UUID *u, INT row, INT serial )
{
	char	id[40];

	ts_uuid_to_str(u, id, sizeof(id));
	return tm_sprintf((UB *)out, (UB *)"<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-0000000ca%03d\""
			  " vobjleft=\"120\" vobjtop=\"%d\" vobjright=\"260\" vobjbottom=\"%d\" height=\"25\" chsz=\"14\""
			  " frcol=\"#000000\" chcol=\"#000000\" tbcol=\"#ffffff\" bgcol=\"#ffffff\" pictdisp=\"true\""
			  " namedisp=\"true\" framedisp=\"true\"/>", id, serial, 10 + row * 30, 35 + row * 30);
}

/* ---------------------------------------------------------------- the program */

LOCAL UB	*logbuf;
LOCAL UD	logpos;

LOCAL INT run( CONST T_ARCARG *a, TMO tmo )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;

	if ( ts_str_to_uuid(SYSDEF_PROG_ARCHIVE, &prog) < E_OK ) return -1;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = a;
	c.argsz = sizeof(*a);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	if ( ob_cre_obj(&c, &pu) < E_OK ) return -1;
	pid = knl_prc_of_uuid(&pu);
	psts.exitcd = -1;
	if ( pid <= 0 || ts_wai_prc(pid, &psts, tmo) < E_OK ) return -1;
	return psts.exitcd;
}

LOCAL INT op( UINT what, CONST TS_UUID *archive, CONST TS_UUID *obj, UINT method )
{
	static T_ARCARG	a;

	knl_memset(&a, 0, sizeof(a));
	a.magic = ARC_ARG_MAGIC;
	a.op = what;
	a.archive = *archive;
	if ( obj != NULL ) a.obj = *obj;
	a.method = method;
	return run(&a, 300000);
}

LOCAL BOOL said( CONST char *w )
{
	UD	p = logpos;
	INT	n = log_from(&p, logbuf, LOG_MAX);

	return has(logbuf, n, w);
}

LOCAL void show_log( void )
{
	UD	p = logpos;
	INT	n = log_from(&p, logbuf, LOG_MAX - 1);

	logbuf[n] = 0;
	tm_putstring(logbuf);
}

/* The UUID the log gives after "root " */
LOCAL BOOL root_said( TS_UUID *u )
{
	UD	p = logpos;
	INT	n = log_from(&p, logbuf, LOG_MAX), at = find(logbuf, n, " root ", 0), i;
	char	s[37];

	if ( at < 0 ) return FALSE;
	for ( i = 0; i < 36 && at + 6 + i < n; i++ ) s[i] = (char)logbuf[at + 6 + i];
	s[i] = 0;
	return (BOOL)( ts_str_to_uuid(s, u) >= E_OK );
}

/* The book of an archive object read into a; FALSE when there is none */
LOCAL UB	*bookraw;
LOCAL BOOL book_of( CONST TS_UUID *u, BPKARC *a )
{
	INT	rec = book_rec(u), len = 0;
	char	err[160];

	knl_memset(a, 0, sizeof(*a));
	if ( bookraw != NULL ) Kfree(bookraw);
	bookraw = ( rec >= 0 ) ? rec_read(u, rec, &len, NULL, NULL) : NULL;
	if ( bookraw == NULL ) return FALSE;
	if ( bpk_parse(a, bookraw, (UINT)len, err, sizeof(err)) < E_OK ) {
		tm_printf((UB *)"book: %s\n", err);
		return FALSE;
	}
	return TRUE;
}

/* Record 0 of an object as binary TAD, its pictures left out; to free */
LOCAL UB *as_tad( CONST TS_UUID *u, INT *p_len )
{
	INT	len = 0, n = 0;
	UB	*x = rec_read(u, 0, &len, NULL, NULL), *t = NULL;
	BK_TADW	*w = (BK_TADW *)Kmalloc(sizeof(BK_TADW));

	*p_len = 0;
	if ( x != NULL && w != NULL && bk_tad_from_xml(w, x, len, NULL, 0, &n, NULL, NULL) >= E_OK ) {
		t = (UB *)Kmalloc((SZ)n + 16);
		if ( t != NULL && bk_tad_from_xml(w, x, len, t, n + 16, &n, NULL, NULL) >= E_OK ) *p_len = n;
	}
	if ( x != NULL ) Kfree(x);
	if ( w != NULL ) Kfree(w);
	return t;
}

/* Two objects' record 0 the same binary TAD */
LOCAL BOOL same_tad( CONST TS_UUID *a, CONST TS_UUID *b )
{
	INT	na = 0, nb = 0, i = -1;
	UB	*ta = as_tad(a, &na), *tb = as_tad(b, &nb);
	BOOL	ok = (BOOL)( ta != NULL && tb != NULL && na == nb && na > 0 );

	if ( ok ) {
		for ( i = 0; i < na && ta[i] == tb[i]; i++ ) ;
		ok = (BOOL)( i == na );
	}
	if ( !ok ) tm_printf((UB *)"tad %d and %d bytes, apart at %d\n", na, nb, i);
	if ( ta != NULL ) Kfree(ta);
	if ( tb != NULL ) Kfree(tb);
	return ok;
}

/* A time of an object's metadata, as STIME; 0 none */
LOCAL UINT meta_time( CONST TS_UUID *u, CONST char *key )
{
	INT	n = 0;
	UB	*m = meta_of(u, &n);
	char	v[48];

	if ( m == NULL ) return 0;
	meta_str(m, n, key, v, sizeof(v));
	Kfree(m);
	return bpk_iso_stime(v);
}

/*
 * Two objects' metadata alike in what the book keeps: the name and the
 * window as they are; the times of the first, which the store gave it,
 * kept beside the second's own as tessronos.btron
 */
LOCAL void same_meta( CONST TS_UUID *a, CONST TS_UUID *b )
{
	INT	na = 0, nb = 0, i;
	UB	*ma = meta_of(a, &na), *mb = meta_of(b, &nb);
	static CONST char *keys[] = { "name", "backgroundColor" };
	static CONST char *times[3][2] = { { "makeDate", "ctime" }, { "updateDate", "mtime" }, { "accessDate", "atime" } };
	char	va[128], vb[128];

	KT_ASSERT(ma != NULL && mb != NULL);
	if ( ma != NULL && mb != NULL ) {
		for ( i = 0; i < (INT)( sizeof(keys) / sizeof(keys[0]) ); i++ ) {
			meta_str(ma, na, keys[i], va, sizeof(va));
			meta_str(mb, nb, keys[i], vb, sizeof(vb));
			KT_ASSERT(va[0] != 0 && same(va, vb));
			if ( !same(va, vb) ) tm_printf((UB *)"%s: %s / %s\n", keys[i], va, vb);
		}
		KT_ASSERT(has(mb, nb, "\"btron\":{"));
		for ( i = 0; i < 3; i++ ) {
			meta_str(ma, na, times[i][0], va, sizeof(va));
			meta_str(mb, nb, times[i][1], vb, sizeof(vb));
			KT_ASSERT(va[0] != 0 && bpk_iso_stime(va) == bpk_iso_stime(vb));
			if ( bpk_iso_stime(va) != bpk_iso_stime(vb) ) tm_printf((UB *)"%s: %s / %s\n", times[i][0], va, vb);
		}
		KT_ASSERT(has(mb, nb, "\"width\":500"));
		KT_ASSERT(has(mb, nb, "\"height\":300"));
		KT_ASSERT(has(mb, nb, "\"editable\":true"));
	}
	if ( ma != NULL ) Kfree(ma);
	if ( mb != NULL ) Kfree(mb);
}

/* ---------------------------------------------------------------- in the kernel */

/* n bytes that look like data: runs, text and noise, the same each time */
LOCAL void fill( UB *p, INT n, UINT seed )
{
	INT	i;
	UINT	r = seed;

	for ( i = 0; i < n; i++ ) {
		r = r * 1103515245U + 12345U;
		switch ( ( i >> 12 ) & 3 ) {
		case 0:	p[i] = (UB)( "BTRON 書庫管理 TessronOS "[i % 26] );	break;
		case 1:	p[i] = (UB)( r >> 24 );				break;
		case 2:	p[i] = (UB)( ( i / 37 ) & 0xFF );			break;
		default: p[i] = (UB)( ( r >> 28 ) + 'a' );			break;
		}
	}
}

LOCAL BOOL lh5_round( CONST UB *in, INT n, INT *p_comp )
{
	BPKLH5E	*ze = (BPKLH5E *)Kmalloc(sizeof(BPKLH5E));
	BPKLH5	*zd = (BPKLH5 *)Kmalloc(sizeof(BPKLH5));
	INT	room = n + n / 4 + 4096, m = -1, k = -1, i = 0;
	UB	*enc = (UB *)Kmalloc(room), *dec = (UB *)Kmalloc(n + 1);

	if ( ze != NULL && zd != NULL && enc != NULL && dec != NULL ) {
		m = lh5_encode(ze, in, (UINT)n, enc, (UINT)room);
		if ( m >= 0 ) k = lh5_decode(zd, enc, (UINT)m, dec, (UINT)n);
		if ( k == n ) for ( i = 0; i < n && dec[i] == in[i]; i++ ) ;
	}
	*p_comp = m;
	if ( ze != NULL ) Kfree(ze);
	if ( zd != NULL ) Kfree(zd);
	if ( enc != NULL ) Kfree(enc);
	if ( dec != NULL ) Kfree(dec);
	return (BOOL)( k == n && i == n );
}

LOCAL BPKARC	sample_arc;
LOCAL UB	*sample_raw;
LOCAL INT	sample_len;

/* The encoder's output read back by the decoder: data of each kind, and the sample's stream */
LOCAL void test_lh5e( void )
{
	UB	*b = (UB *)Kmalloc(256 * 1024);
	INT	comp = 0;
	TS_UUID	s;
	char	err[160];

	KT_ASSERT(b != NULL);
	if ( b == NULL ) return;
	fill(b, 256 * 1024, 1);
	KT_ASSERT(lh5_round(b, 256 * 1024, &comp));
	tm_printf((UB *)"lh5: mixed 262144 -> %d\n", comp);
	KT_ASSERT(comp > 0 && comp < 200 * 1024);
	knl_memset(b, 0, 100000);
	KT_ASSERT(lh5_round(b, 100000, &comp));
	KT_ASSERT(comp > 0 && comp < 200);
	KT_ASSERT(lh5_round((CONST UB *)"A", 1, &comp));
	KT_ASSERT(lh5_round((CONST UB *)"ABABABABABABABABABABABABAB", 26, &comp));
	Kfree(b);

	KT_ASSERT_ER(ts_str_to_uuid(SAMPLE, &s), E_OK);
	sample_raw = rec_read(&s, 1, &sample_len, NULL, NULL);
	if ( sample_raw == NULL ) KT_SKIP("no sample archive");
	KT_ASSERT_ER(bpk_parse(&sample_arc, sample_raw, (UINT)sample_len, err, sizeof(err)), E_OK);
	KT_ASSERT(sample_arc.crcok);
	KT_ASSERT_EQ(sample_arc.head.crc, 0x517A);
	KT_ASSERT(lh5_round(sample_arc.plain, (INT)sample_arc.plainlen, &comp));
	tm_printf((UB *)"lh5: sample 147780 -> %d (BTRON: 29142)\n", comp);
	KT_ASSERT(comp > 0 && comp < 30000);
}

/*
 * The sample written again from what was read -- the local headers, the
 * records, the extension -- makes the same stream, so the same CRC and
 * the same figures, compressed or stored
 */
LOCAL UB	*rewritten;
LOCAL UINT	rewritten_len;

LOCAL void test_rewrite( void )
{
	BPKWRITE	w;
	BPKWFILE	*f;
	BPKWSTAT	st;
	BPKARC		b;
	char		err[160];
	INT		m, i;

	if ( sample_raw == NULL ) KT_SKIP("no sample archive");
	f = (BPKWFILE *)Kcalloc(sample_arc.nfiles, sizeof(BPKWFILE));
	if ( f == NULL ) KT_SKIP("no memory");
	for ( i = 0; i < (INT)sample_arc.nfiles; i++ ) {
		f[i].head = sample_arc.file[i].head;
		f[i].nrec = sample_arc.file[i].nrec;
		f[i].rec = sample_arc.file[i].rec;
	}
	for ( m = 0; m < 2; m++ ) {
		UB	*out = NULL;
		UINT	len = 0;

		knl_memset(&w, 0, sizeof(w));
		w.name = sample_arc.name;
		w.time = sample_arc.head.time;
		w.method = ( m == 0 ) ? BPK_LH5 : BPK_LH0;
		w.nfiles = (INT)sample_arc.nfiles;
		w.file = f;
		w.ext = sample_arc.plain;
		w.extlen = sample_arc.extsize;
		KT_ASSERT_ER(bpk_write(&w, &out, &len, &st, err, sizeof(err)), E_OK);
		if ( out == NULL ) continue;
		KT_ASSERT(bpk_is_archive(out, len));
		KT_ASSERT_ER(bpk_parse(&b, out, len, err, sizeof(err)), E_OK);
		KT_ASSERT_EQ(b.nfiles, SAMPLE_NOBJ);
		KT_ASSERT_EQ(b.head.crc, 0x517A);
		KT_ASSERT(b.crcok);
		KT_ASSERT_EQ(b.head.filesize, sample_arc.head.filesize);
		KT_ASSERT_EQ(b.origsize, sample_arc.origsize);
		KT_ASSERT_EQ(b.method, w.method);
		KT_ASSERT(b.plainlen == sample_arc.plainlen);
		for ( i = 0; i < (INT)b.plainlen && b.plain[i] == sample_arc.plain[i]; i++ ) ;
		KT_ASSERT_EQ(i, (INT)sample_arc.plainlen);
		KT_ASSERT(same(b.name, sample_arc.name));
		KT_ASSERT(b.hasroot && b.rootview[2] == sample_arc.rootview[2]);
		for ( i = 0; i < (INT)b.nfiles; i++ ) {
			KT_ASSERT_EQ(b.file[i].nlink, sample_arc.file[i].nlink);
			KT_ASSERT_EQ(b.file[i].nexec, sample_arc.file[i].nexec);
		}
		bpk_free(&b);
		if ( m == 0 ) {
			rewritten = out;
			rewritten_len = len;
		} else {
			KT_ASSERT_EQ(st.compsize, st.origsize);
			free(out);
		}
	}
	Kfree(f);
}

/* An archive broken in each way: refused, or its CRC found wrong */
LOCAL void test_broken( void )
{
	UB	*c;
	BPKARC	a;
	char	err[160];
	UINT	g, i;

	if ( rewritten == NULL ) KT_SKIP("no archive written");
	c = (UB *)Kmalloc(rewritten_len);
	if ( c == NULL ) KT_SKIP("no memory");

	/* no archive at all */
	KT_ASSERT(bpk_parse(&a, (CONST UB *)"not an archive", 14, err, sizeof(err)) < E_OK);
	KT_ASSERT(!bpk_is_archive((CONST UB *)"not an archive", 14));
	bpk_free(&a);

	/* the global header: 10 (管理情報) + 28 (the figure) + 8 + DF_DATA */
	g = 10 + 28 + 8 + DF_DATA;
	knl_memcpy(c, rewritten, rewritten_len);
	c[g + GH_NFILES] ^= 0x01;
	KT_ASSERT_ER(bpk_parse(&a, c, rewritten_len, err, sizeof(err)), E_OBJ);
	bpk_free(&a);

	/* cut short */
	KT_ASSERT(bpk_parse(&a, rewritten, rewritten_len / 2, err, sizeof(err)) < E_OK);
	bpk_free(&a);

	/* the compressed stream spoiled: refused, or read with its CRC wrong */
	knl_memcpy(c, rewritten, rewritten_len);
	for ( i = 0; i < 64; i++ ) c[g + BPK_GLOBALHEAD + 3000 + i * 7] ^= 0x5A;
	if ( bpk_parse(&a, c, rewritten_len, err, sizeof(err)) >= E_OK ) KT_ASSERT(!a.crcok);
	bpk_free(&a);

	/* a method it does not know */
	knl_memcpy(c, rewritten, rewritten_len);
	c[g + GH_COMPMETHOD] = 7;
	c[g + GH_CHECKSUM] = (UB)bpk_ghead_sum(c + g);
	KT_ASSERT_ER(bpk_parse(&a, c, rewritten_len, err, sizeof(err)), E_NOSPT);
	bpk_free(&a);
	Kfree(c);
}

/* ---------------------------------------------------------------- a network packed and taken out */

LOCAL TS_UUID	na, nb, nc, arc1;
LOCAL BOOL	made;
LOCAL char	*xa, *xb;

LOCAL void test_network( void )
{
	UB	other[OTHER_LEN];
	INT	rec = -1, i, n;
	SZ	asz = 0;
	ID	k;
	char	ia[40];

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CABINET, &cab0), E_OK);
	xa = (char *)Kmalloc(4096);
	xb = (char *)Kmalloc(1024);
	if ( xa == NULL || xb == NULL ) KT_SKIP("no memory");
	KT_ASSERT_ER(make("KT書庫図形", "basic-figure-editor", NULL, &na), E_OK);
	KT_ASSERT_ER(make("KT書庫文章", "basic-text-editor", NULL, &nb), E_OK);
	KT_ASSERT_ER(make("KT書庫修飾", "basic-text-editor",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p><font size=\"18\" color=\"#ff0000\">赤い"
			  "<bold>太字</bold></font>と<underline>下線</underline></p><p>ABC あいう</p></document></tad>\n",
			  &nc), E_OK);
	n = tm_sprintf((UB *)xa, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
		       "<figView left=\"0\" top=\"0\" right=\"400\" bottom=\"300\"/><figScale hunit=\"-72\" vunit=\"-72\"/>"
		       "<rect left=\"10\" top=\"10\" right=\"100\" bottom=\"60\" fillColor=\"#80a0ff\" strokeColor=\"#000000\"/>");
	n += put_link(xa + n, &nb, 0, 1);
	n += put_link(xa + n, &nc, 1, 2);
	ts_uuid_to_str(&na, ia, sizeof(ia));
	n += tm_sprintf((UB *)xa + n, (UB *)"<image left=\"10\" top=\"80\" right=\"138\" bottom=\"208\" href=\"%s_0_0.png\"/>"
			"</figure></tad>\n", ia);
	KT_ASSERT_ER(rewrite0(&na, xa, n), E_OK);
	n = tm_sprintf((UB *)xb, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>図形へ戻る仮身：");
	n += tm_sprintf((UB *)xb + n, (UB *)"<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-0000000cb003\" width=\"140\""
			" heightpx=\"25\" chsz=\"14\"/></p></document></tad>\n", ia);
	KT_ASSERT_ER(rewrite0(&nb, xb, n), E_OK);

	k = ob_opn_obj(&na, OB_OP_ALL);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_wri_res(k, (CONST UB *)"_0_0.png", kt_bk_png, (SZ)( kt_bk_png_end - kt_bk_png )), E_OK);
		ob_cls_obj(k);
	}
	for ( i = 0; i < OTHER_LEN; i++ ) other[i] = (UB)( i * 7 );
	k = ob_opn_obj(&nc, OB_OP_ALL);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_apd_rec(k, OB_RT_SYSDATA, 3, &rec), E_OK);
		KT_ASSERT_ER(ob_wri_rec(k, rec, 0, other, OTHER_LEN, &asz), E_OK);
		ob_cls_obj(k);
	}
	KT_ASSERT_ER(make_archive("KT書庫", &arc1), E_OK);
	made = TRUE;
}

/* The network packed into the archive object, and the book read here */
LOCAL void test_pack( void )
{
	BPKARC	a;
	INT	i, nl = 0, ntad = 0;
	UINT	stime = meta_time(&na, "makeDate");

	if ( !made ) KT_SKIP("no network");
	KT_ASSERT_EQ(op(ARC_OP_PACK, &arc1, &na, BPK_LH5), 0);
	KT_ASSERT(said("archive: packed 3 objects"));
	if ( !said("archive: packed") ) show_log();
	KT_ASSERT(book_of(&arc1, &a));
	if ( a.nfiles != 3 ) return;
	KT_ASSERT(a.crcok);
	KT_ASSERT_EQ(a.method, BPK_LH5);
	KT_ASSERT(same(a.name, "KT書庫図形"));
	KT_ASSERT(same(a.file[0].name, "KT書庫図形"));
	KT_ASSERT(same(a.file[1].name, "KT書庫文章"));
	KT_ASSERT(same(a.file[2].name, "KT書庫修飾"));
	KT_ASSERT(a.hasroot);
	/* the figure's two links name the text and the document, the text's the figure */
	KT_ASSERT_EQ(a.file[0].nlink, 2);
	KT_ASSERT(a.file[0].link[0] == 1 && a.file[0].link[1] == 2);
	KT_ASSERT_EQ(a.file[1].nlink, 1);
	KT_ASSERT_EQ(a.file[1].link[0], 0);
	KT_ASSERT_EQ(a.file[0].head.f_nlink, 2);
	/* the times, the program that opens it and its window */
	KT_ASSERT(stime != 0);
	KT_ASSERT_EQ(a.file[0].head.f_ctime, stime);
	KT_ASSERT_EQ(a.file[0].head.f_mtime, meta_time(&na, "updateDate"));
	KT_ASSERT_EQ(a.file[0].head.f_ltime, 0xFFFFFFFFU);
	KT_ASSERT_EQ(a.file[0].head.f_atype, 2);
	KT_ASSERT(a.file[0].nexec >= 1 && a.file[0].exec[0].appl[1] == 0x0002);
	KT_ASSERT(a.file[0].nexec >= 1 && a.file[0].exec[0].haswin
		  && a.file[0].exec[0].wr - a.file[0].exec[0].wl == 500);
	KT_ASSERT(a.file[0].nexec >= 1 && a.file[0].exec[0].hasbg
		  && ( a.file[0].exec[0].bgraw & 0xFFFFFF ) == 0xFFEECC);
	/* record 0 as binary TAD with its picture */
	for ( i = 0; i < a.file[0].nrec; i++ ) {
		if ( a.file[0].rec[i].type == BPK_RT_TAD ) {
			ntad++;
			KT_ASSERT_ER(bk_tad_check(a.file[0].rec[i].data, (INT)a.file[0].rec[i].size, NULL, NULL), E_OK);
			KT_ASSERT(bpk_tad_is_fig(a.file[0].rec[i].data, a.file[0].rec[i].size));
			KT_ASSERT(has(a.file[0].rec[i].data, (INT)a.file[0].rec[i].size, "\xE5\xFF"));	/* 画像セグメント */
		}
		if ( a.file[0].rec[i].type == BPK_RT_LINK ) nl++;
	}
	KT_ASSERT_EQ(ntad, 1);
	KT_ASSERT_EQ(nl, 2);
	/* the document's record of bytes, as it was */
	for ( i = 0; i < a.file[2].nrec; i++ ) {
		if ( a.file[2].rec[i].type == OB_RT_SYSDATA && a.file[2].rec[i].sub == 3 ) {
			INT	k;

			KT_ASSERT_EQ(a.file[2].rec[i].size, OTHER_LEN);
			for ( k = 0; k < OTHER_LEN && a.file[2].rec[i].data[k] == (UB)( k * 7 ); k++ ) ;
			KT_ASSERT_EQ(k, OTHER_LEN);
		}
	}
	bpk_free(&a);

	/* the list and the figures */
	KT_ASSERT_EQ(op(ARC_OP_LIST, &arc1, NULL, 0), 0);
	KT_ASSERT(said("archive: list 3"));
	KT_ASSERT(said("archive: object 1 KT書庫文章"));
	KT_ASSERT_EQ(op(ARC_OP_INFO, &arc1, NULL, 0), 0);
	KT_ASSERT(said("archive: info name KT書庫図形, 3 objects"));
	KT_ASSERT(said(", lh5, crc ok, made 20"));
}

/* The book taken out into a cabinet: the same network, joined anew */
LOCAL TS_UUID	kcab;

LOCAL void test_unpack( void )
{
	TS_UUID	root, got[8], l[4];
	INT	n, len;
	UB	*x;
	UINT	rt = 0, sub = 0;

	if ( !made ) KT_SKIP("no network");
	KT_ASSERT_ER(make("KT書庫の取出先", "virtual-object-list", "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n",
			  &kcab), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_UNPACK, &arc1, &kcab, 0), 0);
	KT_ASSERT(said("archive: unpacked 3 objects"));
	if ( !said("archive: unpacked") ) show_log();
	KT_ASSERT(root_said(&root));
	x = rec_read(&kcab, 0, &len, NULL, NULL);
	KT_ASSERT(x != NULL && links_of(x, len, l, 4) == 1 && ts_uuid_cmp(&l[0], &root) == 0);
	if ( x != NULL ) Kfree(x);
	n = reach(&root, got, 8);
	KT_ASSERT_EQ(n, 3);
	if ( n != 3 ) return;

	/* each as it was: its metadata, record 0 as binary TAD */
	same_meta(&na, &got[0]);
	same_meta(&nb, &got[1]);
	same_meta(&nc, &got[2]);
	{
		/* taken out of a book: made by 書庫管理, and it says so */
		UB	*m = meta_of(&got[0], &len);
		char	mk[64];

		mk[0] = 0;
		if ( m != NULL ) {
			meta_str(m, len, "maker", mk, sizeof(mk));
			Kfree(m);
		}
		KT_ASSERT(knl_strcmp(mk, "書庫管理") == 0);
	}
	KT_ASSERT(same_tad(&na, &got[0]));
	KT_ASSERT(same_tad(&nb, &got[1]));
	KT_ASSERT(same_tad(&nc, &got[2]));

	/* the links joined: the figure to the text and the document, the text back */
	x = rec_read(&got[0], 0, &len, NULL, NULL);
	KT_ASSERT(x != NULL && links_of(x, len, l, 4) == 2 && ts_uuid_cmp(&l[0], &got[1]) == 0
		  && ts_uuid_cmp(&l[1], &got[2]) == 0);
	KT_ASSERT(x != NULL && has(x, len, "<image") && has(x, len, "fgcolors=\"#80a0ff\""));
	if ( x != NULL && !( has(x, len, "<image") && has(x, len, "fgcolors=\"#80a0ff\"") ) ) {
		tm_putstring(x);
		tm_putstring((UB *)"\n");
	}
	if ( x != NULL ) Kfree(x);
	x = rec_read(&got[1], 0, &len, NULL, NULL);
	KT_ASSERT(x != NULL && links_of(x, len, l, 4) == 1 && ts_uuid_cmp(&l[0], &got[0]) == 0);
	if ( x != NULL ) Kfree(x);
	{
		UB	sig[8];
		SZ	asz = 0;
		ID	k = ob_opn_obj(&got[0], OB_OP_R);

		KT_ASSERT(k > 0 && ob_rea_res(k, (CONST UB *)"_0_0.png", 0, sig, 8, &asz) >= E_OK && asz == 8
			  && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
		if ( k > 0 ) ob_cls_obj(k);
	}

	/* the document's record of bytes */
	KT_ASSERT_EQ(nrec_of(&got[2]), 2);
	x = rec_read(&got[2], 1, &len, &rt, &sub);
	KT_ASSERT(x != NULL && len == OTHER_LEN && rt == OB_RT_SYSDATA && sub == 3);
	if ( x != NULL ) {
		INT	i;

		for ( i = 0; i < OTHER_LEN && x[i] == (UB)( i * 7 ); i++ ) ;
		KT_ASSERT_EQ(i, OTHER_LEN);
		Kfree(x);
	}
	KT_ASSERT_EQ(nrec_of(&got[0]), 1);
	KT_ASSERT_EQ(nrec_of(&got[1]), 1);

	/* the counts as the network has them */
	KT_ASSERT_EQ(refcnt_of(&got[0]), 2);
	KT_ASSERT_EQ(refcnt_of(&got[1]), 1);
	KT_ASSERT_EQ(refcnt_of(&got[2]), 1);
	(void)rewrite0(&kcab, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", 61);
	unmake(got, n);
}

/* ---------------------------------------------------------------- a hierarchy */

/*
 * h0 → h1 → h2 → h3 → h4, and h0 → h2 too (an object reached twice),
 * h4 → h0 (a cycle): five objects, each once, in the order they are
 * reached
 */
LOCAL void test_hierarchy( void )
{
	TS_UUID	h[5], ar, got[8], root;
	char	*x = (char *)Kmalloc(4096), nm[32];
	INT	i, n, len;
	static CONST char *names[5] = { "KT階層0", "KT階層1", "KT階層2", "KT階層3", "KT階層4" };

	if ( x == NULL ) KT_SKIP("no memory");
	for ( i = 0; i < 5; i++ ) KT_ASSERT_ER(make(names[i], "basic-figure-editor", NULL, &h[i]), E_OK);
	for ( i = 0; i < 5; i++ ) {
		n = tm_sprintf((UB *)x, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
			       "<text left=\"10\" top=\"10\" right=\"200\" bottom=\"40\">階層 %d</text>", i);
		if ( i < 4 ) n += put_link(x + n, &h[i + 1], 0, 10 + i);
		if ( i == 0 ) n += put_link(x + n, &h[2], 1, 20);
		if ( i == 4 ) n += put_link(x + n, &h[0], 1, 21);
		n += tm_sprintf((UB *)x + n, (UB *)"</figure></tad>\n");
		KT_ASSERT_ER(rewrite0(&h[i], x, n), E_OK);
	}
	KT_ASSERT_ER(make_archive("KT書庫階層", &ar), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_PACK, &ar, &h[0], BPK_LH5), 0);
	KT_ASSERT(said("archive: packed 5 objects"));
	KT_ASSERT_EQ(op(ARC_OP_LIST, &ar, NULL, 0), 0);
	for ( i = 0; i < 5; i++ ) {
		tm_sprintf((UB *)nm, (UB *)"object %d %s", i, names[i]);
		KT_ASSERT(said(nm));
	}
	KT_ASSERT_EQ(op(ARC_OP_UNPACK, &ar, &kcab, 0), 0);
	KT_ASSERT(said("archive: unpacked 5 objects"));
	KT_ASSERT(root_said(&root));
	n = reach(&root, got, 8);
	KT_ASSERT_EQ(n, 5);
	for ( i = 0; i < n && i < 5; i++ ) KT_ASSERT(same_tad(&h[i], &got[i]));
	/* h0 is linked from h4 and from the cabinet, h2 from h0 and h1 */
	if ( n == 5 ) {
		KT_ASSERT_EQ(refcnt_of(&got[0]), 2);
		KT_ASSERT_EQ(refcnt_of(&got[2]), 2);
		KT_ASSERT_EQ(refcnt_of(&got[4]), 1);
	}
	len = 61;
	(void)rewrite0(&kcab, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", len);
	unmake(got, n);
	(void)ob_del_obj(&ar);
	unmake(h, 5);
	Kfree(x);
}

/* ---------------------------------------------------------------- large, and stored */

/*
 * A text of 120 KB of letters and a record of 2 MB, packed compressed
 * and stored: the same records come out of both, and LH5 makes the book
 * smaller
 */
LOCAL void test_large( void )
{
	TS_UUID	big, a5, a0, root, got[2];
	char	*x = (char *)Kmalloc(BIG_TEXT * 3 + 1024);
	UB	*r = (UB *)Kmalloc(BIG_REC), *back;
	INT	n = 0, i, len, rec = -1, m;
	SZ	asz = 0;
	ID	k;
	BPKARC	a;
	UINT	comp5 = 0, comp0 = 0, rt = 0;

	if ( x == NULL || r == NULL ) {
		if ( x != NULL ) Kfree(x);
		if ( r != NULL ) Kfree(r);
		KT_SKIP("no memory");
	}
	n = tm_sprintf((UB *)x, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>");
	for ( i = 0; i < BIG_TEXT / 30; i++ ) {
		n += tm_sprintf((UB *)x + n, (UB *)"書庫管理の大きな実身%05d。", i);
		if ( i % 40 == 39 ) n += tm_sprintf((UB *)x + n, (UB *)"</p><p>");
	}
	n += tm_sprintf((UB *)x + n, (UB *)"</p></document></tad>\n");
	KT_ASSERT_ER(make("KT大きな実身", "basic-text-editor", NULL, &big), E_OK);
	KT_ASSERT_ER(rewrite0(&big, x, n), E_OK);
	fill(r, BIG_REC, 7);
	k = ob_opn_obj(&big, OB_OP_ALL);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_apd_rec(k, OB_RT_SYSDATA, 9, &rec), E_OK);
		for ( i = 0; i < BIG_REC; i += 65536 ) KT_ASSERT_ER(ob_wri_rec(k, rec, i, r + i, 65536, &asz), E_OK);
		ob_cls_obj(k);
	}
	KT_ASSERT_ER(make_archive("KT書庫大", &a5), E_OK);
	KT_ASSERT_ER(make_archive("KT書庫無圧縮", &a0), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_PACK, &a5, &big, BPK_LH5), 0);
	KT_ASSERT(said("archive: packed 1 objects"));
	KT_ASSERT_EQ(op(ARC_OP_PACK, &a0, &big, BPK_LH0), 0);
	KT_ASSERT(said("(stored)"));
	if ( book_of(&a5, &a) ) {
		comp5 = a.compsize;
		KT_ASSERT(a.crcok && a.method == BPK_LH5);
		KT_ASSERT(a.origsize > BIG_REC);
		bpk_free(&a);
	}
	if ( book_of(&a0, &a) ) {
		comp0 = a.compsize;
		KT_ASSERT(a.crcok && a.method == BPK_LH0 && a.compsize == a.origsize);
		bpk_free(&a);
	}
	tm_printf((UB *)"large: stored %u, lh5 %u\n", comp0, comp5);
	KT_ASSERT(comp5 > 0 && comp5 < comp0 * 3 / 4);
	KT_ASSERT_EQ(op(ARC_OP_INFO, &a0, NULL, 0), 0);
	KT_ASSERT(said(", stored, crc ok"));

	/* both taken out: the same text and the same 2 MB */
	for ( m = 0; m < 2; m++ ) {
		KT_ASSERT_EQ(op(ARC_OP_UNPACK, m == 0 ? &a5 : &a0, &kcab, 0), 0);
		KT_ASSERT(root_said(&root));
		KT_ASSERT_EQ(reach(&root, got, 2), 1);
		KT_ASSERT(same_tad(&big, &root));
		back = rec_read(&root, 1, &len, &rt, NULL);
		KT_ASSERT(back != NULL && len == BIG_REC && rt == OB_RT_SYSDATA);
		if ( back != NULL ) {
			for ( i = 0; i < BIG_REC && back[i] == r[i]; i++ ) ;
			KT_ASSERT_EQ(i, BIG_REC);
			Kfree(back);
		}
		(void)rewrite0(&kcab, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", 61);
		(void)ob_del_obj(&root);
	}
	(void)ob_del_obj(&a5);
	(void)ob_del_obj(&a0);
	(void)ob_del_obj(&big);
	Kfree(x);
	Kfree(r);
}

/* ---------------------------------------------------------------- one book: refused, deleted, copied */

LOCAL void test_add_delete( void )
{
	TS_UUID	a3;
	INT	l1 = 0, l3 = 0, i;
	UB	*b1, *b3;

	if ( !made ) KT_SKIP("no network");
	/* an archive holds one book */
	KT_ASSERT_EQ(op(ARC_OP_PACK, &arc1, &nb, BPK_LH5), 4);
	KT_ASSERT(said("has a book already"));
	/* copied into another, empty, archive: the same bytes */
	KT_ASSERT_ER(make_archive("KT書庫写し", &a3), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_COPY, &a3, &arc1, 0), 0);
	b1 = rec_read(&arc1, book_rec(&arc1), &l1, NULL, NULL);
	b3 = rec_read(&a3, book_rec(&a3), &l3, NULL, NULL);
	KT_ASSERT(b1 != NULL && b3 != NULL && l1 == l3);
	if ( b1 != NULL && b3 != NULL && l1 == l3 ) {
		for ( i = 0; i < l1 && b1[i] == b3[i]; i++ ) ;
		KT_ASSERT_EQ(i, l1);
	}
	if ( b1 != NULL ) Kfree(b1);
	if ( b3 != NULL ) Kfree(b3);
	KT_ASSERT_EQ(op(ARC_OP_COPY, &a3, &arc1, 0), 4);		/* not empty now */
	/* deleted: empty, and a book can be made again */
	KT_ASSERT_EQ(op(ARC_OP_DELETE, &arc1, NULL, 0), 0);
	KT_ASSERT_EQ(book_rec(&arc1), -1);
	KT_ASSERT_EQ(nrec_of(&arc1), 1);
	KT_ASSERT_EQ(op(ARC_OP_LIST, &arc1, NULL, 0), 2);
	KT_ASSERT(said("archive: empty"));
	KT_ASSERT_EQ(op(ARC_OP_PACK, &arc1, &nb, BPK_LH5), 0);
	KT_ASSERT(said("archive: packed 3 objects"));		/* the text reaches the figure and so the rest */
	KT_ASSERT_EQ(op(ARC_OP_LIST, &arc1, NULL, 0), 0);
	KT_ASSERT(said("archive: object 0 KT書庫文章"));

	/* an archive inside a network: its book goes as it is and comes out as an archive */
	{
		TS_UUID	outer, root, got[4], copy;
		char	*x = (char *)Kmalloc(1024);
		INT	n, len;

		if ( x == NULL ) return;
		KT_ASSERT_ER(make("KT書庫を持つ図形", "basic-figure-editor", NULL, &outer), E_OK);
		n = tm_sprintf((UB *)x, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>");
		n += put_link(x + n, &a3, 0, 30);
		n += tm_sprintf((UB *)x + n, (UB *)"</figure></tad>\n");
		KT_ASSERT_ER(rewrite0(&outer, x, n), E_OK);
		Kfree(x);
		copy = a3;
		KT_ASSERT_ER(make_archive("KT書庫の書庫", &a3), E_OK);	/* a3 now a new, empty one */
		KT_ASSERT_EQ(op(ARC_OP_PACK, &a3, &outer, BPK_LH5), 0);
		KT_ASSERT(said("archive: packed 2 objects"));
		KT_ASSERT_EQ(op(ARC_OP_UNPACK, &a3, &kcab, 0), 0);
		KT_ASSERT(root_said(&root));
		n = reach(&root, got, 4);
		KT_ASSERT_EQ(n, 2);
		if ( n == 2 ) {
			UB	*m = meta_of(&got[1], &len);

			KT_ASSERT(m != NULL && has(m, len, "\"archive-manager\":{\"name\":\"書庫管理\",\"defaultOpen\":true}"));
			if ( m != NULL ) Kfree(m);
			/* the archive inside is one: its book lists the same three objects */
			KT_ASSERT(book_rec(&got[1]) >= 0);
			KT_ASSERT_EQ(op(ARC_OP_LIST, &got[1], NULL, 0), 0);
			KT_ASSERT(said("archive: list 3"));
		}
		(void)rewrite0(&kcab, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", 61);
		unmake(got, n);
		(void)ob_del_obj(&outer);
		(void)ob_del_obj(&copy);
		(void)ob_del_obj(&a3);
	}
}

/* ---------------------------------------------------------------- a book that is broken */

LOCAL void test_broken_book( void )
{
	TS_UUID	ab, root;
	INT	rec, len = 0, i;
	UB	*b;
	SZ	asz = 0;
	ID	k;

	if ( !made ) KT_SKIP("no network");
	KT_ASSERT_ER(make_archive("KT壊れた書庫", &ab), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_COPY, &ab, &arc1, 0), 0);
	rec = book_rec(&ab);
	b = ( rec >= 0 ) ? rec_read(&ab, rec, &len, NULL, NULL) : NULL;
	KT_ASSERT(b != NULL && len > 400);
	if ( b == NULL ) return;
	/* the stream spoiled after its start: refused, or read with the CRC wrong */
	for ( i = 0; i < 40; i++ ) b[200 + i * 5] ^= 0xA5;
	k = ob_opn_obj(&ab, OB_OP_ALL);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_wri_rec(k, rec, 0, b, len, &asz), E_OK);
		ob_cls_obj(k);
	}
	i = op(ARC_OP_INFO, &ab, NULL, 0);
	KT_ASSERT(i == 6 || ( i == 0 && said("crc bad") ));
	i = op(ARC_OP_UNPACK, &ab, &kcab, 0);
	KT_ASSERT(i == 6 || i == 7);
	KT_ASSERT(!said("unpacked"));
	KT_ASSERT(!root_said(&root));
	/* the global header spoiled: refused */
	b[10 + 28 + 8 + DF_DATA + GH_ORIGSIZE] ^= 0x10;
	k = ob_opn_obj(&ab, OB_OP_ALL);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_wri_rec(k, rec, 0, b, len, &asz), E_OK);
		ob_cls_obj(k);
	}
	KT_ASSERT_EQ(op(ARC_OP_LIST, &ab, NULL, 0), 6);
	KT_ASSERT(said("書庫のヘッダが壊れています"));
	/* deleted, it is an empty archive again */
	KT_ASSERT_EQ(op(ARC_OP_DELETE, &ab, NULL, 0), 0);
	KT_ASSERT_EQ(book_rec(&ab), -1);
	Kfree(b);
	(void)ob_del_obj(&ab);
}

/* ---------------------------------------------------------------- the book BTRON wrote */

/*
 * The sample copied into an archive object, taken out (33 objects),
 * and the root of what came out packed again: the same 33 names
 */
LOCAL void test_sample( void )
{
	TS_UUID	s, as, ap, root, *got;
	BPKARC	a;
	INT	n, i, j, found = 0;

	KT_ASSERT_ER(ts_str_to_uuid(SAMPLE, &s), E_OK);
	if ( book_rec(&s) < 0 ) KT_SKIP("no sample archive");
	got = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * MADE_MAX);
	if ( got == NULL ) KT_SKIP("no memory");
	KT_ASSERT_ER(make_archive("KT見本の書庫", &as), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_COPY, &as, &s, 0), 0);
	KT_ASSERT_EQ(op(ARC_OP_UNPACK, &as, &kcab, 0), 0);
	KT_ASSERT(said("archive: unpacked 33 objects"));
	KT_ASSERT(root_said(&root));
	n = reach(&root, got, MADE_MAX);
	KT_ASSERT_EQ(n, SAMPLE_NOBJ);
	KT_ASSERT_ER(make_archive("KT見本の書き直し", &ap), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_PACK, &ap, &root, BPK_LH5), 0);
	KT_ASSERT(said("archive: packed 33 objects"));
	if ( book_of(&ap, &a) ) {
		KT_ASSERT_EQ(a.nfiles, SAMPLE_NOBJ);
		KT_ASSERT(same(a.file[0].name, sample_arc.file[0].name));
		for ( i = 0; i < (INT)a.nfiles && sample_raw != NULL; i++ ) {
			for ( j = 0; j < (INT)sample_arc.nfiles && !same(a.file[i].name, sample_arc.file[j].name); j++ ) ;
			if ( j < (INT)sample_arc.nfiles ) {
				found++;
				/* the times BTRON gave it, through the object and back */
				KT_ASSERT_EQ(a.file[i].head.f_ctime, sample_arc.file[j].head.f_ctime);
				KT_ASSERT_EQ(a.file[i].head.f_mtime, sample_arc.file[j].head.f_mtime);
			}
		}
		KT_ASSERT_EQ(found, SAMPLE_NOBJ);
		tm_printf((UB *)"sample: packed again %u bytes into %u\n", a.origsize, a.compsize);
		bpk_free(&a);
	}
	(void)rewrite0(&kcab, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", 61);
	unmake(got, n);
	(void)ob_del_obj(&ap);
	(void)ob_del_obj(&as);
	Kfree(got);
}

/* ---------------------------------------------------------------- マイクロカード実身 */

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n && a[i] == b[i]; i++ ) ;
	return (BOOL)( i == n );
}

LOCAL void put16le( UB *p, UINT v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
}

/*
 * A book of two objects: a マイクロカード実身 of one card (a string item
 * and a 仮身 item, vobjnumber 5, whose VDATA names link record 0) and
 * the object that link record points at. Taken out, the card opens in
 * マイクロカード, record 0 names the second object after <mclinks/>, and
 * the records of type 31 are there byte for byte.
 */
#define MC_DATALEN	( 4 + 22 + 48 )
LOCAL void test_mcard( void )
{
	static UB	fmt[32 + 24 * 2], card[MC_DATALEN], vd[44], lk[BPK_LINKREC], other[16];
	static CONST UH	nm0[] = { 0x234B, 0x2354, 0x234D, 0x2343, 0 }, nm1[] = { 0x234B, 0x2354, 0x2350, 0x2348, 0 };
	BPKREC		r0[4], r1[1];
	BPKWFILE	f[2];
	BPKWRITE	w;
	BPKWSTAT	st;
	TS_UUID		am, root, got[4], l[4];
	char		err[160];
	UB		*out = NULL, *x, *m;
	UINT		len = 0, rt = 0, sub = 0;
	INT		i, n, xl, ml, rec = -1, sub1 = 0, sub2 = 0, sub4 = 0;
	SZ		asz = 0;
	ID		k;

	if ( !made ) KT_SKIP("no network");
	if ( sample_raw == NULL ) KT_SKIP("no sample archive");
	knl_memset(fmt, 0, sizeof(fmt));
	put16le(fmt + 0, 0x2343);
	put16le(fmt + 2, 0x2352);
	put16le(fmt + 4, 0x2344);
	put16le(fmt + 6, 0x0100);
	put16le(fmt + 8, 1);			/* ncards */
	put16le(fmt + 10, MC_DATALEN);
	put16le(fmt + 30, 2);			/* nitems */
	put16le(fmt + 32, 0x2341);		/* "Ａ" */
	put16le(fmt + 32 + 16, 4);
	put16le(fmt + 32 + 18, 2);		/* a string of 10 */
	put16le(fmt + 32 + 20, 10);
	put16le(fmt + 32 + 22, 10);
	put16le(fmt + 56, 0x2342);		/* "Ｂ" */
	put16le(fmt + 56 + 16, 4 + 22);
	put16le(fmt + 56 + 18, 3);		/* a 仮身 */
	put16le(fmt + 56 + 22, 10);
	knl_memset(card, 0, sizeof(card));
	put16le(card, 0xA5B6);
	put16le(card + 2, MC_DATALEN);
	put16le(card + 4, 0x2358);		/* "ＸＹ" */
	put16le(card + 6, 0x2359);
	put16le(card + 26, 5);
	knl_memset(vd, 0, sizeof(vd));
	put16le(vd, 5);
	put16le(vd + 2, 0);
	put16le(vd + 14 + 4, 120);
	put16le(vd + 14 + 6, 20);
	put16le(vd + 14 + 8, 20);
	bpk_link_put(lk, 1, 0, 0, NULL);
	for ( i = 0; i < (INT)sizeof(other); i++ ) other[i] = (UB)( i * 3 );
	r0[0].type = BPK_RT_LINK;	r0[0].sub = 0;	r0[0].size = sizeof(lk);	r0[0].data = lk;
	r0[1].type = BPK_RT_MCARD;	r0[1].sub = 1;	r0[1].size = sizeof(fmt);	r0[1].data = fmt;
	r0[2].type = BPK_RT_MCARD;	r0[2].sub = 2;	r0[2].size = sizeof(card);	r0[2].data = card;
	r0[3].type = BPK_RT_MCARD;	r0[3].sub = 4;	r0[3].size = sizeof(vd);	r0[3].data = vd;
	r1[0].type = OB_RT_SYSDATA;	r1[0].sub = 3;	r1[0].size = sizeof(other);	r1[0].data = other;
	for ( i = 0; i < 2; i++ ) {
		f[i].head = sample_arc.file[0].head;
		knl_memset(f[i].head.name, 0, sizeof(f[i].head.name));
		knl_memcpy(f[i].head.name, ( i == 0 ) ? nm0 : nm1, sizeof(nm0));
	}
	f[0].nrec = 4;
	f[0].rec = r0;
	f[1].nrec = 1;
	f[1].rec = r1;
	knl_memset(&w, 0, sizeof(w));
	w.name = "KTカードの書庫";
	w.time = sample_arc.head.time;
	w.method = BPK_LH5;
	w.nfiles = 2;
	w.file = f;
	KT_ASSERT_ER(bpk_write(&w, &out, &len, &st, err, sizeof(err)), E_OK);
	if ( out == NULL ) return;

	/* into an archive object, and taken out */
	KT_ASSERT_ER(make_archive("KTカードの書庫", &am), E_OK);
	k = ob_opn_obj(&am, OB_OP_ALL);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_apd_rec(k, OB_RT_SYSDATA, 0, &rec), E_OK);
		KT_ASSERT_ER(ob_wri_rec(k, rec, 0, out, len, &asz), E_OK);
		ob_cls_obj(k);
	}
	free(out);
	KT_ASSERT_EQ(op(ARC_OP_UNPACK, &am, &kcab, 0), 0);
	KT_ASSERT(said("archive: unpacked 2 objects"));
	if ( !said("archive: unpacked") ) show_log();
	KT_ASSERT(root_said(&root));
	n = reach(&root, got, 4);
	KT_ASSERT_EQ(n, 2);
	if ( n != 2 ) {
		(void)ob_del_obj(&am);
		return;
	}

	/* opened in マイクロカード */
	m = meta_of(&got[0], &ml);
	KT_ASSERT(m != NULL && has(m, ml, "\"" BPK_MC_PROG "\":{\"name\":\"" BPK_MC_NAME "\",\"defaultOpen\":true}"));
	if ( m != NULL ) Kfree(m);

	/* record 0: the link record's object after <mclinks/> */
	x = rec_read(&got[0], 0, &xl, NULL, NULL);
	KT_ASSERT(x != NULL && has(x, xl, "<p><mclinks/><link id=\""));
	KT_ASSERT(x != NULL && links_of(x, xl, l, 4) == 1 && ts_uuid_cmp(&l[0], &got[1]) == 0);
	if ( x != NULL ) Kfree(x);

	/* the records of the card, as they were */
	for ( i = 1; i < 8; i++ ) {
		x = rec_read(&got[0], i, &xl, &rt, &sub);
		if ( x == NULL ) continue;
		if ( sub == 1 && xl == (INT)sizeof(fmt) && same_bytes(x, fmt, sizeof(fmt)) ) sub1++;
		if ( sub == 2 && xl == (INT)sizeof(card) && same_bytes(x, card, sizeof(card)) ) sub2++;
		if ( sub == 4 && xl == (INT)sizeof(vd) && same_bytes(x, vd, sizeof(vd)) ) sub4++;
		KT_ASSERT(rt == OB_RT_SYSDATA || rt == BPK_RT_MCARD);
		Kfree(x);
	}
	KT_ASSERT(sub1 == 1 && sub2 == 1 && sub4 == 1);
	KT_ASSERT_EQ(refcnt_of(&got[1]), 1);

	(void)rewrite0(&kcab, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", 61);
	unmake(got, n);
	(void)ob_del_obj(&am);
}

/* ---------------------------------------------------------------- the programs that open each object */

#define APPS_MAX	8

/* The ids of an object's applist, and the one it opens with; how many */
LOCAL INT apps_of( CONST TS_UUID *u, UB (*ids)[OM_APP_ID], UB *dflt )
{
	INT	n;

	for ( n = 0; n < APPS_MAX && om_store_app_id(u, n, ids[n], OM_APP_ID) > 0; n++ ) ;
	if ( om_store_default_app(u, dflt, OM_APP_ID) <= 0 ) dflt[0] = 0;
	return n;
}

/* Whether an object's applist names the program id */
LOCAL BOOL names_app( UB (*ids)[OM_APP_ID], INT n, CONST UB *id )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( same((CONST char *)ids[i], (CONST char *)id) ) return TRUE;
	}
	return FALSE;
}

/* Two objects' applists name the same programs and open with the same one */
LOCAL BOOL same_apps( CONST TS_UUID *a, CONST TS_UUID *b )
{
	UB	ia[APPS_MAX][OM_APP_ID], ib[APPS_MAX][OM_APP_ID], da[OM_APP_ID], db[OM_APP_ID];
	INT	na = apps_of(a, ia, da), nb = apps_of(b, ib, db), i;
	BOOL	ok = (BOOL)( na == nb && na > 0 && same((CONST char *)da, (CONST char *)db) );

	for ( i = 0; ok && i < na; i++ ) ok = names_app(ib, nb, ia[i]);
	if ( !ok ) {
		tm_printf((UB *)"  applists: %d programs opening with %s, %d with %s\n", na, da, nb, db);
		for ( i = 0; i < nb; i++ ) tm_printf((UB *)"    %s\n", ib[i]);
	}
	return ok;
}

/* The one an object opens with is id, and its applist has n programs */
LOCAL BOOL opens_with( CONST TS_UUID *u, CONST char *id, INT n )
{
	UB	ids[APPS_MAX][OM_APP_ID], d[OM_APP_ID];
	INT	k = apps_of(u, ids, d);

	if ( k != n || !same((CONST char *)d, id) ) {
		INT	i;

		tm_printf((UB *)"  %d programs, opening with %s\n", k, d);
		for ( i = 0; i < k; i++ ) tm_printf((UB *)"    %s\n", ids[i]);
	}
	return (BOOL)( k == n && same((CONST char *)d, id) );
}

LOCAL CONST char	*ap_fig = "{\"virtual-object-list\":{\"name\":\"仮身一覧\",\"defaultOpen\":false},"
				  "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":true}}";
LOCAL CONST char	*ap_calc = "{\"basic-calc-editor\":{\"name\":\"基本表計算\",\"defaultOpen\":true},"
				   "\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":false}}";
LOCAL CONST char	*ap_text = "{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true},"
				   "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":false},"
				   "\"virtual-object-list\":{\"name\":\"仮身一覧\",\"defaultOpen\":false}}";

/* The application ID of a link record: 8000 mid 8000 */
LOCAL void link_appl( BPKFILE *f, INT nth, UINT mid )
{
	INT	r, l = 0;

	for ( r = 0; r < f->nrec; r++ ) {
		if ( f->rec[r].type != BPK_RT_LINK ) continue;
		if ( l++ == nth ) {
			UB	*p = (UB *)f->rec[r].data;

			put16le(p + LK_APPL, 0x8000);
			put16le(p + LK_APPL + 2, mid);
			put16le(p + LK_APPL + 4, 0x8000);
		}
	}
}

/*
 * 空書庫 there and back: an archive object made as the template makes
 * one, a figure that opens in 基本図形編集 (or 仮身一覧) put in with its
 * links to a table that opens in 基本表計算 (or 基本文章編集) and a text
 * that opens in 基本文章編集 (or the two others), packed, and taken out
 * into a cabinet: the same names, the same record 0, the same programs
 * opening with the same one. Then the book written again with the
 * figure's links naming programs of their own -- 基本文章編集 for the
 * table, which its applist has but does not open with, and 基本表計算
 * for the text, which its applist does not have -- and taken out: each
 * opens with what its link names, and the text has that one besides.
 */
LOCAL void test_apps( void )
{
	TS_UUID		src[3], ar, av, root, got[8], back[8];
	BPKARC		a;
	BPKWFILE	f[3];
	BPKWRITE	w;
	BPKWSTAT	st;
	UB		*out = NULL, mk[64];
	char		*x = (char *)Kmalloc(2048), err[160];
	UINT		len = 0;
	INT		n, nb = 0, i, rec = -1, ml = 0;
	SZ		asz = 0;
	ID		k;

	if ( !made || x == NULL ) {
		if ( x != NULL ) Kfree(x);
		KT_SKIP("no cabinet to take out into");
	}
	KT_ASSERT_ER(make_apps("KT付箋の表", ap_calc,
			       "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>表のつもりの文章</p></document></tad>\n",
			       &src[1]), E_OK);
	KT_ASSERT_ER(make_apps("KT付箋の文章", ap_text,
			       "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>三つで開ける文章</p></document></tad>\n",
			       &src[2]), E_OK);
	n = tm_sprintf((UB *)x, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
		       "<figView left=\"0\" top=\"0\" right=\"400\" bottom=\"300\"/><figScale hunit=\"-72\" vunit=\"-72\"/>");
	n += put_link(x + n, &src[1], 0, 21);
	n += put_link(x + n, &src[2], 1, 22);
	n += tm_sprintf((UB *)x + n, (UB *)"</figure></tad>\n");
	KT_ASSERT_ER(make_apps("KT付箋の図形", ap_fig, x, &src[0]), E_OK);
	Kfree(x);

	/* in, and out */
	KT_ASSERT_ER(make_archive("KT空書庫", &ar), E_OK);
	KT_ASSERT_EQ(op(ARC_OP_PACK, &ar, &src[0], BPK_LH5), 0);
	KT_ASSERT(said("archive: packed 3 objects"));
	KT_ASSERT_EQ(op(ARC_OP_UNPACK, &ar, &kcab, 0), 0);
	KT_ASSERT(said("archive: unpacked 3 objects"));
	if ( !said("archive: unpacked") ) show_log();
	KT_ASSERT(root_said(&root));
	n = reach(&root, got, 8);
	KT_ASSERT_EQ(n, 3);
	for ( i = 0; i < n && i < 3; i++ ) {
		char	na[128], nb2[128];
		UB	*m1 = meta_of(&src[i], &ml), *m2;
		INT	l2 = 0;

		m2 = meta_of(&got[i], &l2);
		na[0] = nb2[0] = 0;
		if ( m1 != NULL ) meta_str(m1, ml, "name", na, sizeof(na));
		if ( m2 != NULL ) meta_str(m2, l2, "name", nb2, sizeof(nb2));
		KT_ASSERT(na[0] != 0 && same(na, nb2));
		if ( m1 != NULL ) Kfree(m1);
		if ( m2 != NULL ) Kfree(m2);
		KT_ASSERT(same_tad(&src[i], &got[i]));
		KT_ASSERT(same_apps(&src[i], &got[i]));
	}

	/* the figure's links naming programs of their own */
	if ( book_of(&ar, &a) && a.nfiles == 3 ) {
		link_appl(&a.file[0], 0, 0x0003);
		link_appl(&a.file[0], 1, 0x0009);
		for ( i = 0; i < 3; i++ ) {
			f[i].head = a.file[i].head;
			f[i].nrec = a.file[i].nrec;
			f[i].rec = a.file[i].rec;
		}
		knl_memset(&w, 0, sizeof(w));
		w.name = a.name;
		w.time = a.head.time;
		w.method = BPK_LH5;
		w.nfiles = 3;
		w.file = f;
		w.ext = a.plain;
		w.extlen = a.extsize;
		KT_ASSERT_ER(bpk_write(&w, &out, &len, &st, err, sizeof(err)), E_OK);
		bpk_free(&a);
	}
	KT_ASSERT(out != NULL);
	if ( out != NULL ) {
		KT_ASSERT_ER(make_archive("KT仮身の付箋", &av), E_OK);
		k = ob_opn_obj(&av, OB_OP_ALL);
		KT_ASSERT(k > 0);
		if ( k > 0 ) {
			KT_ASSERT_ER(ob_apd_rec(k, OB_RT_SYSDATA, 0, &rec), E_OK);
			KT_ASSERT_ER(ob_wri_rec(k, rec, 0, out, len, &asz), E_OK);
			ob_cls_obj(k);
		}
		free(out);
		KT_ASSERT_EQ(op(ARC_OP_UNPACK, &av, &kcab, 0), 0);
		KT_ASSERT(said("archive: unpacked 3 objects"));
		KT_ASSERT(root_said(&root));
		nb = reach(&root, back, 8);
		KT_ASSERT_EQ(nb, 3);
		if ( nb == 3 ) {
			KT_ASSERT(same_apps(&src[0], &back[0]));
			KT_ASSERT(opens_with(&back[1], "basic-text-editor", 2));
			KT_ASSERT(opens_with(&back[2], "basic-calc-editor", 4));
			mk[0] = 0;
			(void)om_store_app_id(&back[2], 3, mk, sizeof(mk));
			KT_ASSERT(same((CONST char *)mk, "basic-calc-editor"));	/* put after those its records name */
		}
		(void)ob_del_obj(&av);
	}

	(void)rewrite0(&kcab, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", 61);
	if ( nb > 0 ) unmake(back, nb);
	unmake(got, n);
	unmake(src, 3);
	(void)ob_del_obj(&ar);
}

/* With nothing it says what it is for and ends */
LOCAL void test_noarg( void )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_OBREF	r;
	T_PSTS	psts;
	ID	pid;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_ARCHIVE, &prog), E_OK);
	if ( ob_ref_obj(&prog, &r) < E_OK ) KT_SKIP("no archive program");
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	psts.exitcd = -1;
	if ( pid > 0 ) KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
}

EXPORT void ktest_arc( void )
{
	TS_UUID	prog;
	T_OBREF	r;

	logbuf = (UB *)Kmalloc(LOG_MAX + 1);
	if ( logbuf == NULL ) return;
	/* the objects made here are the person's, as the desktop makes them (design 8.7.4) */
	knl_ob_make_for_person(TRUE);
	KT_RUN(test_lh5e);
	KT_RUN(test_rewrite);
	KT_RUN(test_broken);
	if ( ts_str_to_uuid(SYSDEF_PROG_ARCHIVE, &prog) >= E_OK && ob_ref_obj(&prog, &r) >= E_OK ) {
		KT_RUN(test_network);
		KT_RUN(test_pack);
		KT_RUN(test_unpack);
		KT_RUN(test_hierarchy);
		KT_RUN(test_large);
		KT_RUN(test_add_delete);
		KT_RUN(test_broken_book);
		KT_RUN(test_sample);
		KT_RUN(test_mcard);
		KT_RUN(test_apps);
		KT_RUN(test_noarg);
		if ( made ) {
			TS_UUID	net[3];

			net[0] = na;
			net[1] = nb;
			net[2] = nc;
			(void)ob_del_obj(&arc1);
			unmake(net, 3);
			(void)ob_del_obj(&kcab);
		}
	}
	if ( sample_raw != NULL ) {
		bpk_free(&sample_arc);
		Kfree(sample_raw);
		sample_raw = NULL;
	}
	if ( rewritten != NULL ) free(rewritten);
	if ( bookraw != NULL ) Kfree(bookraw);
	if ( xa != NULL ) Kfree(xa);
	if ( xb != NULL ) Kfree(xb);
	rewritten = bookraw = NULL;
	xa = xb = NULL;
	knl_ob_make_for_person(FALSE);
	Kfree(logbuf);
}
