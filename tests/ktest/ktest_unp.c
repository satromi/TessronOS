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
 *	書庫解凍 (design 17.16): the sample archive of etc/xtad taken in as a
 *	.bpk file and taken out by the program
 *
 *	The LH5 decoder of lib/libbpk runs here in the kernel on the
 *	archive's stream. Then the archive's bytes are taken in (xf_import)
 *	as a file named .bpk, which makes an object 書庫解凍 opens, and the
 *	program started with no archive says what it is for and ends.
 *
 *	Taking an archive out -- the root's link carried out of the
 *	program's window by the desktop and let go in a window -- needs the
 *	desktop running: ktest_drop does it, onto the system volume and onto
 *	a FAT store.
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

/* The file taken in: its name without .bpk, 書庫解凍 opens it, its bytes in record 1 */
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
	meta = (UB *)Kmalloc(OB_ATR_MAX);
	k = ob_opn_obj(&archive, OB_OP_ATRRD);
	if ( meta != NULL && k > 0 && ob_get_atr(k, meta, OB_ATR_MAX, &asz) >= E_OK ) {
		KT_ASSERT(has(meta, (INT)asz, "\"unpack-file\":{\"name\":\"書庫解凍\",\"defaultOpen\":true}"));
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

/* With no archive it says what it is for and ends */
LOCAL void test_noarg( void )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_OBREF	r;
	T_PSTS	psts;
	ID	pid;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_UNPACK, &prog), E_OK);
	if ( ob_ref_obj(&prog, &r) < E_OK ) KT_SKIP("no unpack program");
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	psts.exitcd = -1;
	if ( pid > 0 ) KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
}

EXPORT void ktest_unp( void )
{
	KT_RUN(test_lh5);
	KT_RUN(test_import);
	KT_RUN_SCREEN(test_noarg);
	if ( raw != NULL ) {
		Kfree(raw);
		raw = NULL;
	}
}
