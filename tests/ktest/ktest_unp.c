/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_unp.c
 *	An archive taken in (design 11.20): the sample archive of etc/xtad
 *	taken in as a .bpk file, and the programs it is offered with
 *
 *	The LH5 decoder of lib/libbpk runs here in the kernel on the
 *	archive's stream. Then the archive's bytes are taken in (xf_import)
 *	as a file named .bpk, which makes an object 書庫管理 opens. An object
 *	whose applist names 書庫管理 by the id it had before (unpack-file)
 *	opens with 書庫管理 too, and 実行 of a link's menu and 付箋指定 of
 *	管理情報 offer the same programs (dt_app_list). A program's window
 *	asks for that list too (DT_RQ_APPS), and gets it entry by entry.
 *
 *	Taking an archive out -- the book carried out of 書庫管理's window
 *	by the desktop and let go in a window -- needs the desktop running:
 *	ktest_drop does it, onto the system volume and onto a FAT store.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/wm.h>
#include <ts/xf.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/bpk.h>
#include <ts/tsfs.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"
#include <ts/om.h>
#include "../../application/desktop/desktop.h"
#include "../../application/calendar/cal_tad.h"

#define SAMPLE		"01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1"
#define SAMPLE_NAME	"20251018発表資料公開用"
#define SAMPLE_SIZE	29288
#define ROOT_NAME	"BTRON　CLUB発表公開用"
#define NOBJ		33
#define LOG_MAX		( 64 * 1024 )

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL has( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k, m = s_len(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( k = 0; k < m && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return TRUE;
	}
	return FALSE;
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

LOCAL UINT rd16( CONST UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 );
}

LOCAL UINT rd32( CONST UB *p )
{
	return rd16(p) | ( rd16(p + 2) << 16 );
}

/* A whole record of an object, in memory the caller frees; NULL when there is none */
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

/* The name in an object's metadata */
LOCAL BOOL name_of( CONST TS_UUID *u, UB *out, INT max )
{
	UB	*j = (UB *)Kmalloc(KT_META_ROOM);
	SZ	asz = 0;
	INT	p, k = 0, i;
	ID	key = ob_opn_obj(u, OB_OP_ATRRD);
	BOOL	ok = FALSE;

	out[0] = 0;
	if ( j != NULL && key > 0 && ob_get_atr(key, j, KT_META_ROOM, &asz) >= E_OK ) {
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

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

/* ---------------------------------------------------------------- the decoder */

LOCAL TS_UUID	sample;
LOCAL UB	*raw;
LOCAL INT	rawlen;

/*
 * The archive's stream inflated by lh5_decode, and the headers it gives
 * looked at: 33 objects, the root's name and its 30 records.
 */
LOCAL void test_lh5( void )
{
	UINT		rt = 0, pos, len, origsize, compsize, extsize, nfiles, sum = 0;
	CONST UB	*g = NULL;
	BPKLH5		*z;
	UB		*out;
	INT		n, i;

	KT_ASSERT_ER(ts_str_to_uuid(SAMPLE, &sample), E_OK);
	raw = rec_read(&sample, 1, &rawlen, &rt);
	if ( raw == NULL ) KT_SKIP("no sample archive");
	KT_ASSERT_EQ(rawlen, SAMPLE_SIZE);
	KT_ASSERT_EQ(rt, OB_RT_SYSDATA);

	/* the 指定付箋 of the archive, the segment after the figure's start */
	for ( pos = 0; pos + 8 <= (UINT)rawlen && g == NULL; pos += len ) {
		UINT	w = rd16(raw + pos);

		if ( w <= TC_SPEC ) {
			len = 2;
			continue;
		}
		len = rd16(raw + pos + 2);
		pos += 4;
		if ( len == TS_LONG ) {
			len = rd32(raw + pos);
			pos += 4;
		}
		if ( w == TS_DFUSEN && rd16(raw + pos + DF_APPL + 2) == BPK_APPL_ARCHIVE ) g = raw + pos + DF_DATA;
	}
	KT_ASSERT(g != NULL);
	if ( g == NULL ) return;
	nfiles = rd16(g + GH_NFILES);
	origsize = rd32(g + GH_ORIGSIZE);
	compsize = rd32(g + GH_COMPSIZE);
	extsize = rd32(g + GH_EXTSIZE);
	KT_ASSERT_EQ(nfiles, NOBJ);
	KT_ASSERT_EQ(rd16(g + GH_COMPMETHOD), BPK_LH5);
	KT_ASSERT_EQ(origsize, 147780);
	KT_ASSERT_EQ(compsize, 29142);
	for ( i = GH_VERSION; i < BPK_GLOBALHEAD; i++ ) sum += g[i];
	KT_ASSERT_EQ(sum & 0xFF, g[GH_CHECKSUM]);

	z = (BPKLH5 *)Kmalloc(sizeof(BPKLH5));
	out = (UB *)Kmalloc(origsize);
	if ( z == NULL || out == NULL ) {
		if ( z != NULL ) Kfree(z);
		if ( out != NULL ) Kfree(out);
		KT_SKIP("no memory");
	}
	n = lh5_decode(z, g + BPK_GLOBALHEAD, compsize, out, origsize);
	KT_ASSERT_EQ(n, (INT)origsize);
	/* the first local header: the root, "BTRON" in TRON code, 30 records, its records just after the headers */
	KT_ASSERT_EQ(rd16(out + extsize + LH_NAME), 0x2342);
	KT_ASSERT_EQ(rd32(out + extsize + LH_F_NREC), 30);
	KT_ASSERT_EQ(rd32(out + extsize + LH_OFFSET), extsize + nfiles * BPK_LOCALHEAD);
	/* and the last object's records end where the stream does */
	KT_ASSERT(rd32(out + extsize + ( nfiles - 1 ) * BPK_LOCALHEAD + LH_OFFSET) < origsize);
	Kfree(z);
	Kfree(out);
}

/* ---------------------------------------------------------------- taken in and taken out */

typedef struct {
	CONST UB	*b;
	INT		n, at;
} MEM;

LOCAL INT mem_read( void *ctx, void *buf, SZ n )
{
	MEM	*m = (MEM *)ctx;
	INT	k = m->n - m->at;

	if ( k > (INT)n ) k = (INT)n;
	knl_memcpy(buf, m->b + m->at, k);
	m->at += k;
	return k;
}

LOCAL TS_UUID	archive;

/* The file taken in: its name without .bpk, 書庫管理 opens it, its bytes in record 1 */
LOCAL void test_import( void )
{
	T_XFSRC	src;
	MEM	m;
	UB	name[128], *meta, *back;
	UINT	rt = 0;
	INT	n = 0;
	SZ	asz = 0;
	ID	k;

	if ( raw == NULL ) KT_SKIP("no sample archive");
	m.b = raw;
	m.n = rawlen;
	m.at = 0;
	knl_memset(&src, 0, sizeof(src));
	src.filename = (CONST UB *)SAMPLE_NAME ".bpk";
	src.source = "ktest";
	src.read = mem_read;
	src.ctx = &m;
	KT_ASSERT_ER(xf_import(&src, NULL, &archive), E_OK);
	KT_ASSERT(name_of(&archive, name, sizeof(name)));
	KT_ASSERT(same(name, SAMPLE_NAME));
	meta = (UB *)Kmalloc(KT_META_ROOM);
	k = ob_opn_obj(&archive, OB_OP_ATRRD);
	if ( meta != NULL && k > 0 && ob_get_atr(k, meta, KT_META_ROOM, &asz) >= E_OK ) {
		KT_ASSERT(has(meta, (INT)asz, "\"archive-manager\":{\"name\":\"書庫管理\",\"defaultOpen\":true}"));
		KT_ASSERT(has(meta, (INT)asz, "application/x-btron-archive"));
	} else {
		KT_ASSERT(FALSE);
	}
	if ( k > 0 ) ob_cls_obj(k);
	if ( meta != NULL ) Kfree(meta);
	back = rec_read(&archive, 1, &n, &rt);
	KT_ASSERT(back != NULL);
	KT_ASSERT_EQ(n, rawlen);
	KT_ASSERT_EQ(rt, OB_RT_SYSDATA);
	if ( back != NULL ) {
		for ( n = 0; n < rawlen && back[n] == raw[n]; n++ ) ;
		KT_ASSERT_EQ(n, rawlen);
		Kfree(back);
	}
	(void)ob_del_obj(&archive);
}

/* An object of the store: its metadata, and record 0 as xmlTAD when xml is given */
LOCAL ER make_obj( CONST char *json, CONST char *xml, TS_UUID *u )
{
	T_OBCRE	c;
	INT	rec = -1;
	SZ	asz = 0;
	ID	k;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	er = ob_cre_obj(&c, u);
	if ( er < E_OK || xml == NULL ) return er;
	k = ob_opn_obj(u, OB_OP_ALL);
	if ( k <= 0 ) return E_NOEXS;
	er = ob_apd_rec(k, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK ) er = ob_wri_rec(k, rec, 0, xml, s_len(xml), &asz);
	ob_cls_obj(k);
	return er;
}

/*
 * The list as a program's window gets it (DT_RQ_APPS, read with
 * ct_app_entry) the same as the desktop's own: each entry's id, name and
 * whether it is of the object's kind, how many, and none past the last.
 * applist, when not NULL, is carried as the link's own (DT_F_APPLIST).
 */
LOCAL void same_as_asked( CONST TS_UUID *u, CONST DTAPP *a, INT n, CONST char *applist )
{
	T_DTREQ	*rq = (T_DTREQ *)Kmalloc(sizeof(T_DTREQ));
	T_DTANS	*an = (T_DTANS *)Kmalloc(sizeof(T_DTANS));
	UB	id[CT_APP_ID], name[CT_APP_NAME];
	BOOL	kind = FALSE;
	INT	k, i;

	if ( rq == NULL || an == NULL ) {
		if ( rq != NULL ) Kfree(rq);
		if ( an != NULL ) Kfree(an);
		KT_ASSERT(FALSE);
		return;
	}
	for ( k = 0; k <= n; k++ ) {
		knl_memset(rq, 0, sizeof(*rq));
		knl_memset(an, 0, sizeof(*an));
		rq->req = DT_RQ_APPS;
		rq->target = *u;
		rq->recno = k;
		if ( applist != NULL ) {
			for ( i = 0; applist[i] != 0 && i < DT_CARRY_TEXT - 1; i++ ) rq->text[i] = (UB)applist[i];
			rq->flags |= DT_F_APPLIST;
		}
		an->er = dt_apps_req(rq, an);
		KT_ASSERT_EQ((INT)an->how, n);
		if ( k == n ) {
			KT_ASSERT_ER(an->er, E_NOEXS);
			KT_ASSERT(!ct_app_entry(an, id, name, &kind));
			break;
		}
		KT_ASSERT(ct_app_entry(an, id, name, &kind));
		KT_ASSERT(same(id, (CONST char *)( ( a[k].listed && a[k].raw[0] != 0 ) ? a[k].raw : a[k].id )));
		KT_ASSERT(same(name, (CONST char *)a[k].name));
		KT_ASSERT_EQ(kind, (BOOL)!a[k].listed);
	}
	Kfree(rq);
	Kfree(an);
}

LOCAL INT app_at( CONST DTAPP *a, INT n, CONST char *id )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( same(a[i].id, id) ) return i;
	}
	return -1;
}

/*
 * The programs an object is offered with (dt_app_list), which 実行 and
 * 付箋指定 both show. An archive whose applist names unpack-file, as one
 * taken in before 書庫管理 took 書庫解凍 in: 書庫管理, under its own id
 * and name, the one it opens with. A text: its applist in its order,
 * then the programs of its kind, none twice and none of them its own.
 */
LOCAL void test_programs( void )
{
	CONST DTPROG	*pa, *pp;
	TS_UUID		old, txt;
	T_VOBJ		*v;
	DTAPP		*a;
	INT		room, n, i, k, dflt = -1, kinds = 0;
	CONST char	*jo = "{\"name\":\"KT古い書庫\",\"applist\":{\"unpack-file\":{\"name\":\"書庫解凍\",\"defaultOpen\":true}}}";
	CONST char	*jt = "{\"name\":\"KT実行の文章\",\"applist\":{\"basic-figure-editor\":{\"name\":\"x\",\"defaultOpen\":false},"
			      "\"basic-text-editor\":{\"name\":\"x\",\"defaultOpen\":true}}}";

	(void)dt_prog_start();
	pa = dt_prog_find((CONST UB *)"archive-manager");
	if ( pa == NULL ) KT_SKIP("no program box in the store");
	KT_ASSERT(dt_prog_find((CONST UB *)"unpack-file") == pa);
	KT_ASSERT(same(pa->name, "書庫管理"));
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) KT_SKIP("no memory");

	KT_ASSERT_ER(make_obj(jo, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", &old), E_OK);
	knl_memset(v, 0, sizeof(*v));
	v->target = old;
	room = dt_app_list(v, NULL, 0, NULL);
	a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));
	n = ( a != NULL ) ? dt_app_list(v, a, room, &dflt) : 0;
	KT_ASSERT(n >= 1);
	if ( n >= 1 ) {
		KT_ASSERT(same(a[0].id, "archive-manager"));
		KT_ASSERT(same(a[0].raw, "unpack-file"));
		KT_ASSERT(same(a[0].name, "書庫管理"));
		KT_ASSERT(a[0].listed);
		KT_ASSERT_EQ(dflt, 0);
		same_as_asked(&old, a, n, NULL);
	}
	if ( a != NULL ) Kfree(a);
	(void)ob_del_obj(&old);

	KT_ASSERT_ER(make_obj(jt, "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>KT</p></document></tad>\n", &txt), E_OK);
	knl_memset(v, 0, sizeof(*v));
	v->target = txt;
	room = dt_app_list(v, NULL, 0, NULL);
	a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));
	n = ( a != NULL ) ? dt_app_list(v, a, room, &dflt) : 0;
	KT_ASSERT(n >= 2);
	if ( n >= 2 ) {
		KT_ASSERT(same(a[0].id, "basic-figure-editor") && a[0].listed);
		KT_ASSERT(same(a[1].id, "basic-text-editor") && a[1].listed);
		KT_ASSERT(same(a[0].name, "基本図形編集"));
		KT_ASSERT_EQ(dflt, 1);
		for ( i = 2; i < n; i++ ) {
			KT_ASSERT(!a[i].listed);
			pp = dt_prog_find(a[i].id);
			KT_ASSERT(pp != NULL && pp->types[0] != 0);
			for ( k = 0; k < i; k++ ) KT_ASSERT(!same(a[k].id, (CONST char *)a[i].id));
			kinds++;
		}
		/* スライド opens any xmlTAD: offered after the applist */
		if ( dt_prog_find((CONST UB *)"basic-slide") != NULL ) KT_ASSERT(app_at(a, n, "basic-slide") >= 2);
		tm_printf((UB *)"  a text is offered %d programs, %d of them for its kind\n", n, kinds);
		same_as_asked(&txt, a, n, NULL);
	}
	if ( a != NULL ) Kfree(a);
	(void)ob_del_obj(&txt);

	/* an object with no applist, its link carrying one: the link's, then those of its kind */
	KT_ASSERT_ER(make_obj("{\"name\":\"KT仮身のapplist\"}",
			      "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>KT</p></document></tad>\n", &txt), E_OK);
	knl_memset(v, 0, sizeof(*v));
	v->target = txt;
	{
		CONST char	*la = "{\"basic-calc-editor\":{\"name\":\"x\",\"defaultOpen\":true}}";

		for ( i = 0; la[i] != 0; i++ ) v->applist[i] = (UB)la[i];
		v->applist[i] = 0;
		room = dt_app_list(v, NULL, 0, NULL);
		a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));
		n = ( a != NULL ) ? dt_app_list(v, a, room, &dflt) : 0;
		KT_ASSERT(n >= 1 && same(a[0].id, "basic-calc-editor") && a[0].listed);
		KT_ASSERT_EQ(dflt, 0);
		if ( n >= 1 ) same_as_asked(&txt, a, n, la);
		if ( a != NULL ) Kfree(a);
	}
	(void)ob_del_obj(&txt);
	Kfree(v);
}

EXPORT void ktest_unp( void )
{
	KT_RUN(test_lh5);
	KT_RUN(test_import);
	KT_RUN(test_programs);
	if ( raw != NULL ) {
		Kfree(raw);
		raw = NULL;
	}
}
