/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_bkp.c
 *	バックアップ (design 17.18): the accessory saving and restoring
 *
 *	A small network of objects -- a figure with a picture and links to a
 *	text and a document, the text linking back to the figure -- saved
 *	by the program to a file on /boot, the file read here as a backup
 *	volume, and restored into a cabinet of its own: the same records,
 *	the links joined to the new objects, the counts as they were. The
 *	sample volume BTRON wrote restored the same way. Then the window
 *	driven with keys: save, the 復帰 sheet, the volume read and taken in.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/fs.h>
#include <ts/hid.h>
#include <ts/wm.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/btbk.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/om.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"
#include "../../application/backup/bk_arg.h"

IMPORT CONST UB kt_bk_vol[], kt_bk_vol_end[];
IMPORT CONST UB kt_bk_png[], kt_bk_png_end[];

#define LOG_MAX		( 64 * 1024 )
#define SAVE_FILE	"/boot/\xEF\xBC\x91\xE3\x83\xBBKTEST\xE4\xBF\x9D\xE5\xAD\x98.TAD"	/* １・KTEST保存.TAD */
#define BTRON_FILE	"/boot/BTRONBK.TAD"
#define UI_FILE		"/boot/\xEF\xBC\x91\xE3\x83\xBBKT\xE4\xBF\x9D\xE5\xAD\x98\xE6\x96\x87\xE7\xAB\xA0.TAD"	/* １・KT保存文章.TAD */
#define OTHER_LEN	100
#define MADE_MAX	16

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

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

LOCAL INT log_from( UD *pos, UB *buf, INT max )
{
	INT	n = 0, k;

	while ( n < max && ( k = tm_log_read(pos, buf + n, max - n) ) > 0 ) n += k;
	return n;
}

/* A whole record, in memory to free; NULL when there is none */
LOCAL UB *rec_read( CONST TS_UUID *u, INT recno, INT *p_len, UINT *p_rt )
{
	T_OBREC	r[16];
	INT	cnt = 0, i, size = -1;
	SZ	asz = 0;
	D	at = 0;
	UB	*buf;
	ID	k = ob_opn_obj(u, OB_OP_R);

	*p_len = 0;
	if ( k <= 0 ) return NULL;
	if ( ob_lst_rec(k, r, 16, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 16; i++ ) {
			if ( r[i].recno == recno ) {
				size = (INT)r[i].size;
				if ( p_rt != NULL ) *p_rt = r[i].rt;
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
	T_OBREC	r[16];
	INT	cnt = 0;
	ID	k = ob_opn_obj(u, OB_OP_R);

	if ( k <= 0 ) return -1;
	(void)ob_lst_rec(k, r, 16, &cnt);
	ob_cls_obj(k);
	return cnt;
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

LOCAL BOOL name_of( CONST TS_UUID *u, UB *out, INT max )
{
	UB	*j = (UB *)Kmalloc(OB_ATR_MAX);
	SZ	asz = 0;
	INT	p, k = 0, i;
	ID	key = ob_opn_obj(u, OB_OP_ATRRD);
	BOOL	ok = FALSE;

	out[0] = 0;
	if ( j != NULL && key > 0 && ob_get_atr(key, j, OB_ATR_MAX, &asz) >= E_OK ) {
		p = find(j, (INT)asz, "\"name\":\"", 0);
		if ( p >= 0 ) {
			for ( i = p + 8; i < (INT)asz && j[i] != '"' && k < max - 1; i++ ) out[k++] = j[i];
			out[k] = 0;
			ok = TRUE;
		}
	}
	if ( key > 0 ) ob_cls_obj(key);
	if ( j != NULL ) Kfree(j);
	return ok;
}

LOCAL INT refcnt_of( CONST TS_UUID *u )
{
	T_OBREF	r;

	return ( ob_ref_obj(u, &r) >= E_OK ) ? r.refcnt : -1;
}

/* ---------------------------------------------------------------- the network */

LOCAL TS_UUID	cab0;			/* the first cabinet */
LOCAL TS_UUID	na, nb, nc, kcab;	/* the network, and the cabinet that holds it */
LOCAL BOOL	made;

/* An object of a name and a record 0 */
LOCAL ER make( CONST char *name, CONST char *app, CONST char *xml, TS_UUID *u )
{
	char	json[256];
	T_OBCRE	c;
	INT	rec = -1;
	SZ	asz = 0;
	ID	k;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	tm_sprintf((UB *)json, (UB *)"{\"name\":\"%s\",\"applist\":{\"%s\":{\"name\":\"x\",\"defaultOpen\":true}},"
		   "\"window\":{\"pos\":{\"x\":100,\"y\":90},\"width\":500,\"height\":300}}", name, app);
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	c.near = cab0;
	er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(u, OB_OP_ALL);
	if ( k <= 0 ) return E_NOEXS;
	er = ob_apd_rec(k, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK && xml != NULL ) er = ob_wri_rec(k, rec, 0, xml, s_len(xml), &asz);
	ob_cls_obj(k);
	return er;
}

/* Record 0 of an object written again */
LOCAL ER rewrite0( CONST TS_UUID *u, CONST char *xml )
{
	SZ	asz = 0;
	ID	k = ob_opn_obj(u, OB_OP_ALL);
	ER	er;

	if ( k <= 0 ) return E_NOEXS;
	er = ob_wri_rec(k, 0, 0, xml, s_len(xml), &asz);
	if ( er >= E_OK ) er = ob_trn_rec(k, 0, (UD)s_len(xml));
	ob_cls_obj(k);
	return er;
}

LOCAL char	xa[1024], xb[512], xk[512];

LOCAL void test_network( void )
{
	char	ia[40], ib[40], ic[40];
	UB	other[OTHER_LEN];
	INT	rec = -1, i;
	SZ	asz = 0;
	ID	k;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CABINET, &cab0), E_OK);
	KT_ASSERT_ER(make("KT保存図形", "basic-figure-editor", NULL, &na), E_OK);
	KT_ASSERT_ER(make("KT保存文章", "basic-text-editor", NULL, &nb), E_OK);
	KT_ASSERT_ER(make("KT保存修飾", "basic-text-editor",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p><font size=\"18\" color=\"#ff0000\">赤い"
			  "<bold>太字</bold></font>と<underline>下線</underline></p><p>ABC あいう</p></document></tad>\n",
			  &nc), E_OK);
	ts_uuid_to_str(&na, ia, sizeof(ia));
	ts_uuid_to_str(&nb, ib, sizeof(ib));
	ts_uuid_to_str(&nc, ic, sizeof(ic));
	tm_sprintf((UB *)xa, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
		   "<figView left=\"0\" top=\"0\" right=\"400\" bottom=\"300\"/><figScale hunit=\"-72\" vunit=\"-72\"/>"
		   "<rect left=\"10\" top=\"10\" right=\"100\" bottom=\"60\" fillColor=\"#80a0ff\" strokeColor=\"#000000\"/>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000b001\" vobjleft=\"120\" vobjtop=\"10\""
		   " vobjright=\"260\" vobjbottom=\"35\" height=\"25\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\""
		   " tbcol=\"#ffffff\" bgcol=\"#ffffff\" pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\"/>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000b002\" vobjleft=\"120\" vobjtop=\"40\""
		   " vobjright=\"260\" vobjbottom=\"65\" height=\"25\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\""
		   " tbcol=\"#ffffff\" bgcol=\"#ffffff\" pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\"/>"
		   "<image left=\"10\" top=\"80\" right=\"138\" bottom=\"208\" href=\"%s_0_0.png\"/>"
		   "</figure></tad>\n", ib, ic, ia);
	tm_sprintf((UB *)xb, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>図形へ戻る仮身："
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000b003\" width=\"140\" heightpx=\"25\""
		   " chsz=\"14\"/></p></document></tad>\n", ia);
	KT_ASSERT_ER(rewrite0(&na, xa), E_OK);
	KT_ASSERT_ER(rewrite0(&nb, xb), E_OK);

	/* the figure's picture, and a record of bytes in the document */
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

	/* a cabinet that holds the figure */
	tm_sprintf((UB *)xk, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000b004\" vobjleft=\"8\" vobjtop=\"8\""
		   " vobjright=\"200\" vobjbottom=\"33\"/></figure></tad>\n", ia);
	KT_ASSERT_ER(make("KT保存箱", "virtual-object-list", xk, &kcab), E_OK);
	made = TRUE;
	KT_ASSERT_EQ(refcnt_of(&na), 2);		/* the cabinet and the text */
	KT_ASSERT_EQ(refcnt_of(&nb), 1);
	KT_ASSERT_EQ(refcnt_of(&nc), 1);
}

/* ---------------------------------------------------------------- running the program */

LOCAL UB	*logbuf;
LOCAL UD	logpos;

/* The program run on an argument, waited for; its exit code, or -1 */
LOCAL INT run( CONST void *arg, SZ argsz, TMO tmo )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;

	if ( ts_str_to_uuid(SYSDEF_PROG_BACKUP, &prog) < E_OK ) return -1;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = arg;
	c.argsz = argsz;
	if ( ob_cre_obj(&c, &pu) < E_OK ) return -1;
	pid = knl_prc_of_uuid(&pu);
	psts.exitcd = -1;
	if ( pid <= 0 || ts_wai_prc(pid, &psts, tmo) < E_OK ) return -1;
	return psts.exitcd;
}

LOCAL void show_log( void )
{
	UD	p = logpos;
	INT	n = log_from(&p, logbuf, LOG_MAX - 1);

	logbuf[n] = 0;
	tm_putstring(logbuf);
}

LOCAL BOOL said( CONST char *w )
{
	UD	p = logpos;
	INT	n = log_from(&p, logbuf, LOG_MAX);

	return has(logbuf, n, w);
}

/* The UUID the log gives after "root " */
LOCAL BOOL root_said( TS_UUID *u )
{
	UD	p = logpos;
	INT	n = log_from(&p, logbuf, LOG_MAX), at = find(logbuf, n, " root ", 0);
	char	s[37];
	INT	i;

	if ( at < 0 ) return FALSE;
	for ( i = 0; i < 36 && at + 6 + i < n; i++ ) s[i] = (char)logbuf[at + 6 + i];
	s[i] = 0;
	return (BOOL)( ts_str_to_uuid(s, u) >= E_OK );
}

/* Every object linked from a root, once each, into out[] */
LOCAL INT reach( CONST TS_UUID *root, TS_UUID *out, INT max )
{
	INT	n = 1, i, k, len, m;
	TS_UUID	l[8];
	UB	*x;

	out[0] = *root;
	for ( i = 0; i < n; i++ ) {
		x = rec_read(&out[i], 0, &len, NULL);
		if ( x == NULL ) continue;
		m = links_of(x, len, l, 8);
		for ( k = 0; k < m; k++ ) {
			INT	j;

			for ( j = 0; j < n && ts_uuid_cmp(&out[j], &l[k]) != 0; j++ ) ;
			if ( j == n && n < max ) out[n++] = l[k];
		}
		Kfree(x);
	}
	return n;
}

LOCAL void unmake( TS_UUID *u, INT n )
{
	INT	i;

	for ( i = n - 1; i >= 0; i-- ) (void)ob_del_obj(&u[i]);
}

/* ---------------------------------------------------------------- saving and restoring */

LOCAL UB *file_read( CONST char *path, INT *p_len )
{
	T_FSTAT	st;
	INT	fd, n;
	UB	*b;

	*p_len = 0;
	if ( fs_stat(path, &st) < 0 || st.size == 0 || st.size > 64 * 1024 * 1024 ) return NULL;
	b = (UB *)Kmalloc((SZ)st.size);
	fd = fs_open(path, O_RDONLY);
	if ( b == NULL || fd < 0 ) {
		if ( b != NULL ) Kfree(b);
		if ( fd >= 0 ) fs_close(fd);
		return NULL;
	}
	n = fs_read(fd, b, (SZ)st.size);
	fs_close(fd);
	if ( n != (INT)st.size ) {
		Kfree(b);
		return NULL;
	}
	*p_len = n;
	return b;
}

typedef struct {
	CONST UB *p;
	INT	n, at;
} SRC;

LOCAL INT src_read( void *arg, UB *buf, INT len )
{
	SRC	*s = arg;

	if ( len > s->n - s->at ) len = s->n - s->at;
	knl_memcpy(buf, s->p + s->at, len);
	s->at += len;
	return len;
}

LOCAL TS_UUID	root2;

/* The network saved by the program to a file; the file read as a volume here */
LOCAL void test_save( void )
{
	static T_BKARG	a;
	BK_RD		*rd;
	BK_FRAG		fr[8];
	BK_VOLHEAD	vh;
	BK_OBJHEAD	oh;
	BK_FRAG		*f;
	BK_RECHEAD	rh;
	SRC		s;
	UB		*v, *tad;
	INT		len = 0, nobj = 0, r, nimage = 0, first[4], ntype1 = 0;

	if ( !made ) KT_SKIP("no network");
	(void)fs_unlink(SAVE_FILE);
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_SAVE;
	a.obj = na;
	knl_strcpy((char *)a.path, "/boot");
	knl_strcpy((char *)a.base, "KTEST保存");
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 60000), 0);
	KT_ASSERT(said("backup: saved 3 objects in 1 volumes"));
	if ( !said("backup: saved") ) show_log();

	v = file_read(SAVE_FILE, &len);
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	rd = (BK_RD *)Kmalloc(sizeof(BK_RD));
	if ( rd == NULL ) {
		Kfree(v);
		return;
	}
	s.p = v;
	s.n = len;
	s.at = 0;
	bk_rd_init(rd, fr, 8);
	KT_ASSERT_ER(bk_rd_open(rd, src_read, &s, &vh), E_OK);
	KT_ASSERT_EQ(vh.nobj, 3);
	KT_ASSERT(!vh.more);
	KT_ASSERT_EQ(vh.memo[0], 0x234B);		/* "KTEST保存": row 3 K */
	while ( ( r = bk_rd_object(rd, &oh, &f) ) > 0 ) {
		INT	n = 0;

		first[nobj < 4 ? nobj : 3] = -1;
		KT_ASSERT_EQ(oh.objid, (UINT)( ( nobj + 1 ) << 16 | 1 ));
		KT_ASSERT_EQ(oh.cmp, 1);
		while ( bk_rd_record(rd, &rh) > 0 ) {
			if ( n == 0 ) first[nobj < 4 ? nobj : 3] = rh.type;
			if ( rh.type == 1 && nobj == 0 ) {
				/* the figure as binary TAD: whole, with its picture */
				tad = (UB *)Kmalloc(rh.len);
				if ( tad != NULL && bk_rd_read(rd, tad, rh.len) == rh.len ) {
					INT	at;

					KT_ASSERT_ER(bk_tad_check(tad, rh.len, NULL, NULL), E_OK);
					for ( at = 10; at + 4 <= rh.len; ) {
						UINT	id = tad[at] | ( tad[at + 1] << 8 ), sl;

						if ( id < 0xFF80 ) { at += 2; continue; }
						sl = tad[at + 2] | ( tad[at + 3] << 8 );
						at += 4;
						if ( sl == 0xFFFF ) {
							sl = tad[at] | ( tad[at + 1] << 8 ) | ( tad[at + 2] << 16 ) | ( (UINT)tad[at + 3] << 24 );
							at += 4;
						}
						if ( id == 0xFFE5 ) nimage++;
						at += (INT)sl;
					}
				}
				if ( tad != NULL ) Kfree(tad);
				ntype1++;
			}
			n++;
		}
		KT_ASSERT_EQ(n, oh.st.f_nrec);
		nobj++;
	}
	KT_ASSERT_EQ(r, 0);
	KT_ASSERT_EQ(nobj, 3);
	KT_ASSERT_EQ(first[0], 28);			/* TessronOS's own record first */
	KT_ASSERT_EQ(ntype1, 1);
	KT_ASSERT_EQ(nimage, 1);
	KT_ASSERT_ER(bk_rd_close(rd), E_OK);
	Kfree(rd);
	Kfree(v);
}

/* The file restored into a cabinet of its own: the same network, joined anew */
LOCAL void test_restore( void )
{
	static T_BKARG	a;
	TS_UUID		k2, got[MADE_MAX], l[4];
	UB		*x, *y, name[128];
	INT		n, len, len2, i;
	UINT		rt = 0;

	if ( !made ) KT_SKIP("no network");
	KT_ASSERT_ER(make("KT復帰箱", "virtual-object-list", "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n",
			  &k2), E_OK);
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_RESTORE;
	a.obj = k2;
	knl_strcpy((char *)a.path, SAVE_FILE);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 60000), 0);
	KT_ASSERT(said("backup: restored 3 objects from 1 volumes, 3 links, 0 lost"));
	if ( !said("backup: restored") ) show_log();

	/* the cabinet shows the root */
	x = rec_read(&k2, 0, &len, NULL);
	KT_ASSERT(x != NULL && links_of(x, len, &root2, 1) == 1);
	if ( x != NULL ) Kfree(x);
	n = reach(&root2, got, MADE_MAX);
	KT_ASSERT_EQ(n, 3);

	/* the figure: record 0 as it was but for its links, its picture, its name */
	KT_ASSERT(name_of(&root2, name, sizeof(name)) && same(name, "KT保存図形"));
	{
		/* its metadata as it was, the dates it had kept beside the store's */
		UB	*m = (UB *)Kmalloc(OB_ATR_MAX + 1);
		SZ	asz = 0;
		ID	k = ob_opn_obj(&root2, OB_OP_ATRRD);

		KT_ASSERT(m != NULL && k > 0 && ob_get_atr(k, m, OB_ATR_MAX, &asz) >= E_OK);
		if ( m != NULL && asz > 0 ) {
			KT_ASSERT(has(m, (INT)asz, "\"basic-figure-editor\""));
			KT_ASSERT(has(m, (INT)asz, "\"height\":300"));
			KT_ASSERT(has(m, (INT)asz, "\"btron\":{\"objid\":65537,\"atype\":2"));
			KT_ASSERT(has(m, (INT)asz, "\"ctime\":\"20"));
		}
		if ( k > 0 ) ob_cls_obj(k);
		if ( m != NULL ) Kfree(m);
	}
	x = rec_read(&root2, 0, &len, &rt);
	y = rec_read(&na, 0, &len2, NULL);
	KT_ASSERT(x != NULL && y != NULL);
	if ( x != NULL && y != NULL ) {
		KT_ASSERT_EQ(rt, OB_RT_TAD);
		KT_ASSERT_EQ(len, len2);			/* a UUID for a UUID */
		if ( len != len2 ) {
			tm_putstring(x);
			tm_putstring((UB *)"\n");
			tm_putstring(y);
			tm_putstring((UB *)"\n");
		}
		KT_ASSERT(has(x, len, "fillColor=\"#80a0ff\""));
		KT_ASSERT(has(x, len, "vobjid=\"01a0d8c4-0000-7000-8000-00000000b002\""));
		KT_ASSERT_EQ(links_of(x, len, l, 4), 2);
	}
	if ( x != NULL ) Kfree(x);
	if ( y != NULL ) Kfree(y);
	{
		UB	sig[8];
		SZ	asz = 0;
		ID	k = ob_opn_obj(&root2, OB_OP_R);

		KT_ASSERT(k > 0 && ob_rea_res(k, (CONST UB *)"_0_0.png", 0, sig, 8, &asz) >= E_OK && asz == 8
			  && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
		if ( k > 0 ) ob_cls_obj(k);
	}
	KT_ASSERT(name_of(&l[0], name, sizeof(name)) && same(name, "KT保存文章"));
	KT_ASSERT(name_of(&l[1], name, sizeof(name)) && same(name, "KT保存修飾"));

	/* the text links back to the new figure */
	x = rec_read(&l[0], 0, &len, NULL);
	KT_ASSERT(x != NULL && links_of(x, len, got + 8, 1) == 1 && ts_uuid_cmp(&got[8], &root2) == 0);
	if ( x != NULL ) Kfree(x);

	/* the document: its text, and the record of bytes as it was */
	x = rec_read(&l[1], 0, &len, NULL);
	KT_ASSERT(x != NULL && has(x, len, "<underline>下線</underline>"));
	if ( x != NULL ) Kfree(x);
	KT_ASSERT_EQ(nrec_of(&l[1]), 2);
	x = rec_read(&l[1], 1, &len, &rt);
	KT_ASSERT(x != NULL && len == OTHER_LEN && rt == OB_RT_SYSDATA);
	if ( x != NULL ) {
		for ( i = 0; i < OTHER_LEN && x[i] == (UB)( i * 7 ); i++ ) ;
		KT_ASSERT_EQ(i, OTHER_LEN);
		Kfree(x);
	}
	KT_ASSERT_EQ(nrec_of(&root2), 1);		/* nothing held for the work is left */
	KT_ASSERT_EQ(nrec_of(&l[0]), 1);

	/* the counts as they were */
	KT_ASSERT_EQ(refcnt_of(&root2), 2);
	KT_ASSERT_EQ(refcnt_of(&l[0]), 1);
	KT_ASSERT_EQ(refcnt_of(&l[1]), 1);

	unmake(got, n);
	(void)ob_del_obj(&k2);
	(void)fs_unlink(SAVE_FILE);
}

/*
 * The network cut into volumes of 30,000 bytes: each file within it,
 * a later volume refused as a start, and the set restored from the
 * first with the rest found beside it by their names.
 */
#define VOL_MAX		6
LOCAL CONST char *vol_file[VOL_MAX] = {
	"/boot/\xEF\xBC\x91\xE3\x83\xBBKTVOL.TAD",	/* １・KTVOL.TAD */
	"/boot/\xEF\xBC\x92\xE3\x83\xBBKTVOL.TAD",
	"/boot/\xEF\xBC\x93\xE3\x83\xBBKTVOL.TAD",
	"/boot/\xEF\xBC\x94\xE3\x83\xBBKTVOL.TAD",
	"/boot/\xEF\xBC\x95\xE3\x83\xBBKTVOL.TAD",
	"/boot/\xEF\xBC\x96\xE3\x83\xBBKTVOL.TAD",
};
#define VOL_CAP		30000
#define PNG_MAX		0x20000

LOCAL void test_volumes( void )
{
	static T_BKARG	a;
	TS_UUID		k2, root, got[MADE_MAX];
	UB		*x, *y, *v;
	INT		n, len, len2, i, nv = 0, at;
	char		want[80];

	if ( !made ) KT_SKIP("no network");
	for ( i = 0; i < VOL_MAX; i++ ) (void)fs_unlink(vol_file[i]);
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_SAVE;
	a.obj = na;
	knl_strcpy((char *)a.path, "/boot");
	knl_strcpy((char *)a.base, "KTVOL");
	a.capacity = VOL_CAP;
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 120000), 0);
	KT_ASSERT(said("backup: saved 3 objects in "));
	if ( !said("backup: saved") ) show_log();

	/* the files: as many as the log says, none larger than asked */
	for ( i = 0; i < VOL_MAX; i++ ) {
		v = file_read(vol_file[i], &len);
		if ( v == NULL ) break;
		KT_ASSERT(len > 0 && len <= VOL_CAP);
		Kfree(v);
		nv++;
	}
	KT_ASSERT(nv >= 2);
	{
		UD	p = logpos;

		n = log_from(&p, logbuf, LOG_MAX);
		at = find(logbuf, n, " in ", 0);
		KT_ASSERT(at > 0 && logbuf[at + 4] == (UB)( '0' + nv ) && logbuf[at + 5] == ' ');
	}

	KT_ASSERT_ER(make("KT分割箱", "virtual-object-list", "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n",
			  &k2), E_OK);

	/* the second volume is not where a set begins */
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_RESTORE;
	a.obj = k2;
	knl_strcpy((char *)a.path, vol_file[1]);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 60000), 1);
	KT_ASSERT(said("backup: failed"));
	x = rec_read(&k2, 0, &len, NULL);
	KT_ASSERT(x != NULL && links_of(x, len, &root, 1) == 0);	/* nothing made */
	if ( x != NULL ) Kfree(x);

	/* from the first: all of them */
	knl_strcpy((char *)a.path, vol_file[0]);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 120000), 0);
	knl_strcpy(want, "backup: restored 3 objects from 0 volumes, 3 links, 0 lost");
	want[32] = (char)( '0' + nv );
	KT_ASSERT(said(want));
	if ( !said("backup: restored") ) show_log();
	KT_ASSERT(root_said(&root));
	n = reach(&root, got, MADE_MAX);
	KT_ASSERT_EQ(n, 3);
	x = rec_read(&root, 0, &len, NULL);
	y = rec_read(&na, 0, &len2, NULL);
	KT_ASSERT(x != NULL && y != NULL && len == len2);
	if ( x != NULL ) Kfree(x);
	if ( y != NULL ) Kfree(y);
	{
		/* the picture came across the volumes whole */
		UB	*p1, *p2;
		INT	l1 = 0, l2 = 0;
		SZ	asz = 0;
		ID	k = ob_opn_obj(&root, OB_OP_R), kn = ob_opn_obj(&na, OB_OP_R);

		p1 = (UB *)Kmalloc(PNG_MAX);
		p2 = (UB *)Kmalloc(PNG_MAX);
		if ( k > 0 && p1 != NULL && ob_rea_res(k, (CONST UB *)"_0_0.png", 0, p1, PNG_MAX, &asz) >= E_OK ) l1 = (INT)asz;
		asz = 0;
		if ( kn > 0 && p2 != NULL && ob_rea_res(kn, (CONST UB *)"_0_0.png", 0, p2, PNG_MAX, &asz) >= E_OK ) l2 = (INT)asz;
		KT_ASSERT(l1 > 0 && l1 == l2);
		for ( i = 0; i < l1 && l1 == l2 && p1[i] == p2[i]; i++ ) ;
		KT_ASSERT_EQ(i, l1);
		if ( p1 != NULL ) Kfree(p1);
		if ( p2 != NULL ) Kfree(p2);
		if ( k > 0 ) ob_cls_obj(k);
		if ( kn > 0 ) ob_cls_obj(kn);
	}
	KT_ASSERT_EQ(refcnt_of(&root), 2);

	unmake(got, n);
	(void)ob_del_obj(&k2);
	for ( i = 0; i < VOL_MAX; i++ ) (void)fs_unlink(vol_file[i]);
}

/* The volume BTRON wrote, restored: four cabinets that link to one another */
LOCAL void test_btron( void )
{
	static T_BKARG	a;
	TS_UUID		k3, root, got[MADE_MAX];
	UB		*x, name[128];
	INT		fd, len, n, i, links[4] = { 0 };

	fd = fs_open(BTRON_FILE, O_WRONLY | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	KT_ASSERT_EQ(fs_write(fd, kt_bk_vol, (SZ)( kt_bk_vol_end - kt_bk_vol )), (INT)( kt_bk_vol_end - kt_bk_vol ));
	fs_close(fd);
	KT_ASSERT_ER(make("KT復帰箱２", "virtual-object-list", "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n",
			  &k3), E_OK);
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_RESTORE;
	a.obj = k3;
	knl_strcpy((char *)a.path, BTRON_FILE);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 60000), 0);
	KT_ASSERT(said("backup: restored 4 objects from 1 volumes, 7 links, 0 lost, 4 converted"));
	if ( !said("backup: restored") ) show_log();

	x = rec_read(&k3, 0, &len, NULL);
	KT_ASSERT(x != NULL && links_of(x, len, &root, 1) == 1);
	if ( x != NULL ) Kfree(x);
	n = reach(&root, got, MADE_MAX);
	KT_ASSERT_EQ(n, 4);
	KT_ASSERT(name_of(&root, name, sizeof(name)));
	KT_ASSERT(same(name, "1018キャビネットデモ"));
	for ( i = 0; i < n && i < 4; i++ ) {
		UB	*m = (UB *)Kmalloc(OB_ATR_MAX);
		SZ	asz = 0;
		ID	k = ob_opn_obj(&got[i], OB_OP_R);
		TS_UUID	l[4];

		x = rec_read(&got[i], 0, &len, NULL);
		KT_ASSERT(x != NULL && has(x, len, "<figure>"));
		if ( x != NULL ) {
			links[i] = links_of(x, len, l, 4);
			Kfree(x);
		}
		KT_ASSERT(refcnt_of(&got[i]) >= 1);
		/* a cabinet opens in 仮身一覧; what BTRON said of it is kept */
		if ( m != NULL && k > 0 && ob_get_atr(k, m, OB_ATR_MAX, &asz) >= E_OK ) {
			KT_ASSERT(has(m, (INT)asz, "\"virtual-object-list\""));
			KT_ASSERT(has(m, (INT)asz, "\"btron\":{\"objid\":"));
			KT_ASSERT(has(m, (INT)asz, "\"mtime\":\"2025-10-"));
		}
		if ( k > 0 ) ob_cls_obj(k);
		if ( m != NULL ) Kfree(m);
		KT_ASSERT_EQ(nrec_of(&got[i]), 2);		/* record 0 and the 実行機能付箋 */
	}
	KT_ASSERT_EQ(links[0], 3);
	KT_ASSERT_EQ(links[0] + links[1] + links[2] + links[3], 7);
	unmake(got, n);
	(void)ob_del_obj(&k3);
	(void)fs_unlink(BTRON_FILE);
}

/* ---------------------------------------------------------------- several roots, some left out */

#define ROOTS_FILE	"/boot/\xEF\xBC\x91\xE3\x83\xBBKT\xE6\xA0\xB9.TAD"	/* １・KT根.TAD */

/* The links of a record whose box is (l, t, r, b), their targets into out[] */
LOCAL INT links_at( CONST UB *x, INT n, INT l, INT t, INT r, INT b, TS_UUID *out, INT max )
{
	char	w[96];
	INT	at = 0, k = 0;

	tm_sprintf((UB *)w, (UB *)"vobjleft=\"%d\" vobjtop=\"%d\" vobjright=\"%d\" vobjbottom=\"%d\"", l, t, r, b);
	while ( k < max && ( at = find(x, n, "<link id=\"", at) ) >= 0 ) {
		INT	e = find(x, n, ">", at);
		char	s[37];
		INT	i;

		if ( e < 0 ) break;
		if ( find(x, e, w, at) >= 0 ) {
			for ( i = 0; i < 36; i++ ) s[i] = (char)x[at + 10 + i];
			s[36] = 0;
			if ( ts_str_to_uuid(s, &out[k]) >= E_OK ) k++;
		}
		at = e;
	}
	return k;
}

/*
 * Two roots saved together from the cabinet that links to them, what
 * one links to left out: the archive holds the roots and what the other
 * links to. Restored, the roots go back into that cabinet where their
 * links were, and the link to what was left out points at it still;
 * with the cabinet gone they go into the one given, and with what was
 * left out gone that link is lost.
 */
LOCAL void test_roots( void )
{
	static T_BKARG	a;
	TS_UUID		p, r1, r2, x, y, k2, got[MADE_MAX], n1[2], n2[2];
	char		ir1[40], ir2[40], ix[40], iy[40];
	UB		*rec;
	INT		len = 0, n, m;

	if ( !made ) KT_SKIP("no network");
	KT_ASSERT_ER(make("KT除く", "basic-text-editor",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>除く</p></document></tad>\n", &x), E_OK);
	KT_ASSERT_ER(make("KT根2の子", "basic-text-editor",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>子</p></document></tad>\n", &y), E_OK);
	ts_uuid_to_str(&x, ix, sizeof(ix));
	ts_uuid_to_str(&y, iy, sizeof(iy));
	tm_sprintf((UB *)xb, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>根1："
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000c001\" width=\"140\" heightpx=\"25\""
		   " chsz=\"14\"/></p></document></tad>\n", ix);
	KT_ASSERT_ER(make("KT根1", "basic-text-editor", xb, &r1), E_OK);
	tm_sprintf((UB *)xb, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>根2："
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000c002\" width=\"140\" heightpx=\"25\""
		   " chsz=\"14\"/></p></document></tad>\n", iy);
	KT_ASSERT_ER(make("KT根2", "basic-text-editor", xb, &r2), E_OK);
	ts_uuid_to_str(&r1, ir1, sizeof(ir1));
	ts_uuid_to_str(&r2, ir2, sizeof(ir2));
	tm_sprintf((UB *)xk, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000c003\" vobjleft=\"10\" vobjtop=\"10\""
		   " vobjright=\"150\" vobjbottom=\"35\" height=\"25\" chsz=\"14\"/>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000c004\" vobjleft=\"200\" vobjtop=\"60\""
		   " vobjright=\"340\" vobjbottom=\"85\" height=\"25\" chsz=\"14\"/></figure></tad>\n", ir1, ir2);
	KT_ASSERT_ER(make("KT根の箱", "virtual-object-list", xk, &p), E_OK);
	KT_ASSERT_ER(make("KT根の復帰箱", "virtual-object-list", "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n",
			  &k2), E_OK);

	/* saved: the two roots and the child of the second; not what the first links to */
	(void)fs_unlink(ROOTS_FILE);
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_SAVE;
	a.obj = r1;
	a.nroot = 1;
	a.root[0] = r2;
	a.parent[0] = p;
	a.parent[1] = p;
	a.nexcl = 1;
	a.excl[0] = x;
	knl_strcpy((char *)a.path, "/boot");
	knl_strcpy((char *)a.base, "KT根");
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 60000), 0);
	KT_ASSERT(said("backup: saved 3 objects in 1 volumes"));
	KT_ASSERT(said(", roots 2"));
	if ( !said("backup: saved") ) show_log();

	/* restored with the cabinet there: both roots back in it, at their boxes */
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_RESTORE;
	a.obj = k2;
	knl_strcpy((char *)a.path, ROOTS_FILE);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 60000), 0);
	KT_ASSERT(said("backup: restored 3 objects from 1 volumes"));
	KT_ASSERT(said(", 0 lost,"));
	KT_ASSERT(said(", roots 2 back 2"));
	if ( !said("backup: restored") ) show_log();
	rec = rec_read(&p, 0, &len, NULL);
	KT_ASSERT(rec != NULL);
	m = 0;
	if ( rec != NULL ) {
		KT_ASSERT_EQ(links_of(rec, len, got, 8), 4);
		m = links_at(rec, len, 10, 10, 150, 35, n1, 2) + links_at(rec, len, 200, 60, 340, 85, n2, 2);
		KT_ASSERT_EQ(m, 4);				/* the old and the new, each where it was */
		if ( m != 4 ) {
			tm_putstring(rec);
			tm_putstring((UB *)"\n");
		}
		Kfree(rec);
	}
	rec = rec_read(&k2, 0, &len, NULL);
	KT_ASSERT(rec != NULL && links_of(rec, len, got, 8) == 0);	/* nothing was put here */
	if ( rec != NULL ) Kfree(rec);
	n = 0;
	if ( m == 4 ) {
		TS_UUID	nr1 = ( ts_uuid_cmp(&n1[0], &r1) == 0 ) ? n1[1] : n1[0];
		TS_UUID	nr2 = ( ts_uuid_cmp(&n2[0], &r2) == 0 ) ? n2[1] : n2[0];
		UB	nm[64];

		KT_ASSERT(name_of(&nr1, nm, sizeof(nm)) && same(nm, "KT根1"));
		KT_ASSERT(name_of(&nr2, nm, sizeof(nm)) && same(nm, "KT根2"));
		/* the first root's link to what was left out is to it, the same object */
		rec = rec_read(&nr1, 0, &len, NULL);
		KT_ASSERT(rec != NULL && has(rec, len, ix));
		if ( rec != NULL ) Kfree(rec);
		KT_ASSERT_EQ(refcnt_of(&x), 2);
		/* the second's child is a new one */
		n = reach(&nr2, got, MADE_MAX);
		KT_ASSERT_EQ(n, 2);
		KT_ASSERT(n == 2 && ts_uuid_cmp(&got[1], &y) != 0);
		if ( n < MADE_MAX ) got[n++] = nr1;
	}

	/* the cabinet gone, what was left out gone: into the one given, that link lost */
	(void)ob_del_obj(&p);
	unmake(got, n);
	(void)ob_del_obj(&r1);
	(void)ob_del_obj(&x);
	KT_ASSERT_EQ(refcnt_of(&x), -1);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 60000), 0);
	KT_ASSERT(said(", roots 2 back 0"));
	KT_ASSERT(said(", 1 lost,"));
	rec = rec_read(&k2, 0, &len, NULL);
	n = ( rec != NULL ) ? links_of(rec, len, n1, 2) : 0;
	KT_ASSERT_EQ(n, 2);
	if ( rec != NULL ) Kfree(rec);
	m = 0;
	for ( len = 0; len < n; len++ ) m += reach(&n1[len], got + m, MADE_MAX - m);
	KT_ASSERT_EQ(m, 3);
	(void)ob_del_obj(&k2);
	unmake(got, m);
	(void)ob_del_obj(&r2);
	(void)ob_del_obj(&y);
	(void)fs_unlink(ROOTS_FILE);
}

/* ---------------------------------------------------------------- the room on the store */

#define ROOM_DEV	kt_scratch()
#define ROOM_LEFT	( 256 * 1024 )		/* what is left free for the test */

/* An object on the volume of dev, of a name and record 0 */
LOCAL ER make_on( CONST char *dev, CONST char *name, CONST char *xml, TS_UUID *u )
{
	char	json[160];
	T_OBCRE	c;
	INT	rec = -1;
	SZ	asz = 0;
	ID	k;
	ER	er;

	tm_sprintf((UB *)json, (UB *)"{\"name\":\"%s\",\"applist\":{\"virtual-object-list\":{\"name\":\"x\",\"defaultOpen\":true}}}",
		   name);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	c.vol = dev;
	er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(u, OB_OP_ALL);
	if ( k <= 0 ) return E_NOEXS;
	er = ob_apd_rec(k, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK ) er = ob_wri_rec(k, rec, 0, xml, s_len(xml), &asz);
	ob_cls_obj(k);
	return er;
}

/* A record of n bytes that do not compress added to an object */
LOCAL ER fill( CONST TS_UUID *u, UD n, UINT seed )
{
	UB	*b = (UB *)Kmalloc(65536);
	UD	at = 0;
	SZ	asz = 0;
	INT	rec = -1, i;
	ID	k = ob_opn_obj(u, OB_OP_ALL);
	ER	er;

	if ( b == NULL || k <= 0 ) {
		if ( b != NULL ) Kfree(b);
		if ( k > 0 ) ob_cls_obj(k);
		return E_NOMEM;
	}
	er = ob_apd_rec(k, OB_RT_SYSDATA, 0, &rec);
	while ( er >= E_OK && at < n ) {
		SZ	part = ( n - at > 65536 ) ? 65536 : (SZ)( n - at );

		for ( i = 0; i < (INT)part; i++ ) {
			seed = seed * 1103515245U + 12345U;
			b[i] = (UB)( seed >> 16 );
		}
		er = ob_wri_rec(k, rec, (D)at, b, part, &asz);
		at += (UD)part;
	}
	ob_cls_obj(k);
	Kfree(b);
	return er;
}

/* The free bytes of the volume an object is on */
LOCAL UD room_of( CONST TS_UUID *u )
{
	T_OBVOL	v;

	return ( ob_ref_vol(u, &v) >= E_OK ) ? v.bfree * v.bsize : 0;
}

/* The capacity the program said it gave volume 1 */
LOCAL UD cap_said( void )
{
	UD	p = logpos, v = 0;
	INT	n = log_from(&p, logbuf, LOG_MAX), at = find(logbuf, n, "backup: volume 1 capacity ", 0);

	if ( at < 0 ) return 0;
	for ( at += 26; at < n && logbuf[at] >= '0' && logbuf[at] <= '9'; at++ ) v = v * 10 + (UD)( logbuf[at] - '0' );
	return v;
}

/*
 * Volumes kept as objects on a small store: a volume takes no more than
 * the store has free. A network that does not fit fails, and the volume
 * begun is taken away again, the room as it was; one that fits is one
 * volume, linked into the cabinet given, within the room there was.
 */
LOCAL void test_room( void )
{
	static T_BKARG	a;
	TS_UUID		cab, filler, big, small, vols[4];
	UB		*rec;
	UD		room0, room1, cap;
	INT		len = 0, n;
	T_OBVOL		v;

	if ( !made ) KT_SKIP("no network");
	if ( ROOM_DEV == NULL ) KT_SKIP(KT_NO_SCRATCH);
	(void)ob_det_vol(ROOM_DEV);
	if ( ts_format_blk(ROOM_DEV, "BKROOM") < E_OK || ob_att_vol(ROOM_DEV, TSFS_STORE_BLK) < E_OK ) {
		KT_SKIP("no block device for a small store");
	}
	KT_ASSERT_ER(make_on(ROOM_DEV, "KT巻の箱", "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", &cab), E_OK);
	KT_ASSERT_ER(ob_ref_vol(&cab, &v), E_OK);
	KT_ASSERT(v.bsize > 0 && v.bfree > 0 && v.bfree <= v.blocks);
	tm_printf((UB *)"  the store: %ld blocks of %d, %ld free\n", (D)v.blocks, (INT)v.bsize, (D)v.bfree);

	/* all but a little of it taken */
	KT_ASSERT_ER(make_on(ROOM_DEV, "KT埋め", "<tad version=\"1.0\" encoding=\"UTF-8\"><document></document></tad>\n",
			     &filler), E_OK);
	room0 = room_of(&cab);
	if ( room0 > ROOM_LEFT ) KT_ASSERT_ER(fill(&filler, room0 - ROOM_LEFT, 7), E_OK);
	room0 = room_of(&cab);
	tm_printf((UB *)"  left free: %ld\n", (D)room0);
	KT_ASSERT(room0 > 0 && room0 <= ROOM_LEFT + 64 * 1024);

	/* a network that does not fit: refused, and nothing is left on the store */
	KT_ASSERT_ER(make("KT大きい", "basic-text-editor",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>大</p></document></tad>\n", &big), E_OK);
	KT_ASSERT_ER(fill(&big, 3 * ROOM_LEFT, 11), E_OK);
	knl_memset(&a, 0, sizeof(a));
	a.magic = BK_ARG_MAGIC;
	a.op = BK_OP_SAVE;
	a.obj = big;
	a.into = cab;
	knl_strcpy((char *)a.base, "KT大きい");
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 120000), 1);
	KT_ASSERT(said("backup: failed"));
	KT_ASSERT(said("空きが足りません"));
	cap = cap_said();
	KT_ASSERT(cap > 0 && cap <= room0);
	rec = rec_read(&cab, 0, &len, NULL);
	KT_ASSERT(rec != NULL && links_of(rec, len, vols, 4) == 0);
	if ( rec != NULL ) Kfree(rec);
	room1 = room_of(&cab);
	tm_printf((UB *)"  capacity %ld, free after %ld\n", (D)cap, (D)room1);
	KT_ASSERT(room1 + 8 * v.bsize >= room0);	/* what the volume had taken is back */

	/* one that fits: one volume, in the cabinet, within the room */
	KT_ASSERT_ER(make("KT小さい", "basic-text-editor",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>小</p></document></tad>\n", &small), E_OK);
	KT_ASSERT_ER(fill(&small, ROOM_LEFT / 4, 13), E_OK);
	a.obj = small;
	knl_strcpy((char *)a.base, "KT小さい");
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT_EQ(run(&a, sizeof(a), 120000), 0);
	KT_ASSERT(said("backup: saved 1 objects in 1 volumes"));
	if ( !said("backup: saved") ) show_log();
	rec = rec_read(&cab, 0, &len, NULL);
	n = ( rec != NULL ) ? links_of(rec, len, vols, 4) : 0;
	KT_ASSERT_EQ(n, 1);
	if ( rec != NULL ) Kfree(rec);
	if ( n == 1 ) {
		T_OBREC	r[4];
		INT	cnt = 0;
		ID	k = ob_opn_obj(&vols[0], OB_OP_R);

		KT_ASSERT(k > 0 && ob_lst_rec(k, r, 4, &cnt) >= E_OK && cnt == 2);
		KT_ASSERT(cnt == 2 && r[1].size > ROOM_LEFT / 4 && r[1].size <= cap_said());
		if ( k > 0 ) ob_cls_obj(k);
		(void)ob_del_obj(&vols[0]);
	}
	(void)ob_del_obj(&big);
	(void)ob_del_obj(&small);
	(void)ob_del_obj(&filler);
	(void)ob_det_vol(ROOM_DEV);
}

/* ---------------------------------------------------------------- the window */

LOCAL INT window_of( CONST char *name, TS_UUID *out )
{
	TS_UUID	list[64];
	T_OBREF	r;
	INT	cnt = 0, i, wid = -1, p;
	ID	kw;

	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) < E_OK ) return -1;
	for ( i = 0; i < cnt; i++ ) {
		if ( ob_ref_obj(&list[i], &r) < E_OK || r.sub != OB_S_WINDOW || !same(r.name, name) ) continue;
		*out = list[i];
		kw = ob_opn_obj(out, OB_OP_R);
		if ( kw > 0 ) {
			UB	j[256];
			SZ	asz = 0;
			D	d = 0;

			if ( ob_get_atr(kw, j, sizeof(j) - 1, &asz) >= E_OK ) {
				p = knl_oj_path(j, (INT)asz, "tessronos", "window");
				p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "number") : -1;
				if ( p >= 0 && knl_oj_num(j, (INT)asz, p, &d) ) wid = (INT)d;
			}
			ob_cls_obj(kw);
		}
	}
	return wid;
}

LOCAL void press( INT wid, UINT code )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.wid = wid;
	ev.code = code;
	knl_wmobj_event(&ev);
	tk_dly_tsk(150);
}

LOCAL BOOL wait_for( CONST char *w, INT tenths )
{
	INT	i;

	for ( i = 0; i < tenths; i++ ) {
		if ( said(w) ) return TRUE;
		tk_dly_tsk(100);
	}
	return FALSE;
}

/*
 * Started on the figure: 保存 lists what it links to, Enter saves the
 * first of them (the text, the figure and the document with it) to
 * /boot; Tab to 復帰, Enter reads the volume's head, Enter again
 * restores it into the first cabinet.
 */
LOCAL void test_window( void )
{
	TS_UUID		prog, pu, win, root, got[MADE_MAX];
	T_OBCRE		c;
	T_PSTS		psts;
	UB		*cab;
	INT		cablen = 0, wid = -1, i, n;
	ID		pid;

	if ( !made ) KT_SKIP("no network");
	(void)fs_unlink(UI_FILE);
	cab = rec_read(&cab0, 0, &cablen, NULL);
	KT_ASSERT(cab != NULL);
	if ( cab == NULL ) return;
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_BACKUP, &prog), E_OK);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &na;
	c.argsz = sizeof(na);
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	KT_ASSERT(wait_for("backup: ready", 100));
	for ( i = 0; i < 20 && wid <= 0; i++ ) {
		wid = window_of("バックアップ", &win);
		if ( wid <= 0 ) tk_dly_tsk(100);
	}
	KT_ASSERT(wid > 0);
	if ( wid > 0 && pid > 0 ) {
		press(wid, 0x28);				/* Enter: save */
		KT_ASSERT(wait_for("backup: saved 3 objects in 1 volumes", 600));
		press(wid, 0x2B);				/* Tab: 復帰 */
		press(wid, 0x28);				/* the volume's head */
		KT_ASSERT(wait_for("Enter で復帰します", 100));
		press(wid, 0x28);				/* restore */
		KT_ASSERT(wait_for("backup: restored 3 objects", 600));
		if ( !said("backup: restored") ) show_log();
		KT_ASSERT(root_said(&root));
		n = reach(&root, got, MADE_MAX);
		KT_ASSERT_EQ(n, 3);
		KT_ASSERT(refcnt_of(&root) >= 1);
		KT_ASSERT(said(", roots 1 back 1"));		/* back into the figure it was chosen in */
		(void)om_obj_link_del(&na, &root);
		unmake(got, n);
		(void)ob_del_obj(&win);
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, 0);
	}
	/* the first cabinet as it was */
	{
		SZ	asz = 0;
		ID	k = ob_opn_obj(&cab0, OB_OP_ALL);

		if ( k > 0 ) {
			(void)ob_wri_rec(k, 0, 0, cab, cablen, &asz);
			(void)ob_trn_rec(k, 0, (UD)cablen);
			ob_cls_obj(k);
		}
	}
	Kfree(cab);
	(void)fs_unlink(UI_FILE);
	wm_update();
}

/*
 * One item of the window's menu, chosen by the pointer: the second
 * button pressed on the work area brings the menu up in a panel of its
 * own, and the row with the label is pressed and let go.
 */
LOCAL BOOL menu_choose( INT wid, CONST char *label )
{
	BOOL		had[WM_PANEL_MAX + 1];
	T_WMPART	pt;
	T_WMEV		ev;
	INT		pid, mp = 0, i;

	for ( pid = 1; pid <= WM_PANEL_MAX; pid++ ) had[pid] = (BOOL)( wm_panel_wid(pid) > 0 );
	knl_memset(&ev, 0, sizeof(ev));
	ev.wid = wid;
	ev.x = 60;
	ev.y = 80;
	ev.code = 1;
	ev.when = 1000000000ULL;
	ev.type = HID_EV_BTN_DOWN;
	knl_wmobj_event(&ev);
	ev.type = HID_EV_BTN_UP;
	knl_wmobj_event(&ev);
	for ( i = 0; i < 30 && mp == 0; i++ ) {
		tk_dly_tsk(100);
		for ( pid = 1; pid <= WM_PANEL_MAX && mp == 0; pid++ ) {
			if ( !had[pid] && wm_panel_wid(pid) > 0 ) mp = pid;
		}
	}
	if ( mp == 0 ) return FALSE;
	tk_dly_tsk(200);				/* the program waits on it by now */
	for ( i = 0; knl_pn_part(mp, i, &pt) >= E_OK; i++ ) {
		if ( !same(pt.label, label) ) continue;
		knl_memset(&ev, 0, sizeof(ev));
		ev.wid = wm_panel_wid(mp);
		ev.x = ( pt.r.left + pt.r.right ) / 2;
		ev.y = ( pt.r.top + pt.r.bottom ) / 2;
		ev.when = 9000000000ULL;		/* long after the press that opened it */
		ev.type = HID_EV_MOVE;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_DOWN;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_UP;
		knl_wmobj_event(&ev);
		tk_dly_tsk(200);
		return TRUE;
	}
	knl_memset(&ev, 0, sizeof(ev));			/* not there: put away */
	ev.type = HID_EV_BTN_DOWN;
	knl_wmobj_event(&ev);
	return FALSE;
}

/*
 * What Space, X and D do, from the window's menu: the first listed
 * marked as a root, the next marked to be left out, the place to save
 * to changed, and the window closed.
 */
LOCAL void test_menu( void )
{
	TS_UUID		prog, pu, win;
	T_OBCRE		c;
	T_PSTS		psts;
	INT		wid = -1, i;
	ID		pid;

	if ( !made ) KT_SKIP("no network");
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_BACKUP, &prog), E_OK);
	(void)log_from(&logpos, logbuf, LOG_MAX);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &na;
	c.argsz = sizeof(na);
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	KT_ASSERT(wait_for("backup: ready, 2 listed", 100));
	for ( i = 0; i < 20 && wid <= 0; i++ ) {
		wid = window_of("バックアップ", &win);
		if ( wid <= 0 ) tk_dly_tsk(100);
	}
	KT_ASSERT(wid > 0);
	if ( wid <= 0 || pid <= 0 ) return;

	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT(menu_choose(wid, "保存する根の印"));
	KT_ASSERT(wait_for("backup: picked 1 left out 0", 30));
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT(menu_choose(wid, "除く実身の印"));
	KT_ASSERT(wait_for("backup: picked 1 left out 1", 30));
	(void)log_from(&logpos, logbuf, LOG_MAX);
	KT_ASSERT(!menu_choose(wid, "書庫の場所を替える"));	/* the 復帰 sheet's */
	KT_ASSERT(menu_choose(wid, "保存先を替える"));
	KT_ASSERT(wait_for("backup: to ", 30));
	KT_ASSERT(!said("backup: to clock"));			/* only disks are places */
	KT_ASSERT(menu_choose(wid, "閉じる"));
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	wm_update();
}

EXPORT void ktest_bkp( void )
{
	TS_UUID	prog;
	T_OBREF	r;

	if ( ts_str_to_uuid(SYSDEF_PROG_BACKUP, &prog) < E_OK || ob_ref_obj(&prog, &r) < E_OK ) {
		kt_skip("bkp", "no backup program");
		return;
	}
	logbuf = (UB *)Kmalloc(LOG_MAX);
	if ( logbuf == NULL ) return;
	KT_RUN(test_network);
	KT_RUN(test_save);
	KT_RUN(test_restore);
	KT_RUN(test_volumes);
	KT_RUN(test_btron);
	KT_RUN(test_roots);
	KT_RUN(test_room);
	KT_RUN_SCREEN(test_window);
	KT_RUN_SCREEN(test_menu);
	if ( made ) {
		(void)ob_del_obj(&kcab);
		(void)ob_del_obj(&na);
		(void)ob_del_obj(&nb);
		(void)ob_del_obj(&nc);
		made = FALSE;
	}
	Kfree(logbuf);
	logbuf = NULL;
}
