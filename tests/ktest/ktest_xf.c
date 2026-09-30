/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_xf.c
 *	Files from outside taken in and given out again (design 18.20)
 *
 *	A text file, an xmlTAD file and an executable taken in from memory:
 *	what the file was is in the metadata ("tessronos.file") and nowhere in
 *	the records, the bytes come back out unchanged, and the name it goes
 *	out as is the file's. A box finds what was taken in by the file's
 *	name, a rename changes both names, a link comes and goes; and a
 *	TADjs object keeps its UUID.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/xf.h>
#include <ts/fs.h>
#include <ts/json.h>

LOCAL CONST char text[]  = "一行目\n二行目 <tag> & \"quote\"\n";
LOCAL CONST char xtad[]  = "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"doc\">"
			   "<document><p>そのまま</p></document></tad>\n";
LOCAL CONST UB   elf[]   = { 0x7F, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xAA, 0x55 };

#define TJ_UUID	"01a0d8c4-5214-7e21-9a4b-2c7d1e0f3a56"

typedef struct {
	CONST UB	*b;
	INT		n, at;
	INT		step;		/* bytes a read gives at most */
} MEM;

LOCAL INT mem_read( void *ctx, void *buf, SZ n )
{
	MEM	*m = (MEM *)ctx;
	INT	k = m->n - m->at;

	if ( k > (INT)n ) k = (INT)n;
	if ( k > m->step ) k = m->step;
	knl_memcpy(buf, m->b + m->at, k);
	m->at += k;
	return k;
}

typedef struct {
	UB	b[512];
	INT	n;
} OUT;

LOCAL INT out_write( void *ctx, CONST void *buf, SZ n )
{
	OUT	*o = (OUT *)ctx;

	if ( o->n + (INT)n > (INT)sizeof(o->b) ) return -1;
	knl_memcpy(o->b + o->n, buf, n);
	o->n += (INT)n;
	return (INT)n;
}

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL same_str( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

LOCAL BOOL has( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k;

	for ( i = 0; i < n; i++ ) {
		for ( k = 0; w[k] != 0 && i + k < n && s[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL ER take( CONST char *name, CONST UB *b, INT n, CONST TS_UUID *box, TS_UUID *u )
{
	T_XFSRC	src;
	MEM	m;

	m.b = b;
	m.n = n;
	m.at = 0;
	m.step = 7;				/* read in small pieces, as a network gives it */
	knl_memset(&src, 0, sizeof(src));
	src.filename = (CONST UB *)name;
	src.source = "ktest";
	src.mtime = 1234567;
	src.read = mem_read;
	src.ctx = &m;
	return xf_import(&src, box, u);
}

/* "tessronos.file" of an object's metadata, in memory the caller frees */
LOCAL UB *file_of( CONST TS_UUID *u, T_JSON *f )
{
	T_JSON	root, tf;
	UB	*meta;
	SZ	size = 0;

	meta = om_obj_meta(u, &size);
	if ( meta != NULL && ( js_parse(meta, (INT)size, &root) < E_OK
			    || js_get(&root, "tessronos", &tf) < E_OK || js_get(&tf, "file", f) < E_OK ) ) {
		Kfree(meta);
		meta = NULL;
	}
	return meta;
}

/* A string member of "tessronos.file" */
LOCAL INT file_member( CONST TS_UUID *u, CONST char *name, UB *out, INT max )
{
	T_JSON	f;
	UB	*meta = file_of(u, &f);
	INT	n;

	out[0] = 0;
	if ( meta == NULL ) return -1;
	n = js_get_str(&f, name, out, max);
	Kfree(meta);
	return n;
}

/* A number member of "tessronos.file", or -1 */
LOCAL D file_num( CONST TS_UUID *u, CONST char *name )
{
	T_JSON	f;
	UB	*meta = file_of(u, &f);
	D	d;

	if ( meta == NULL ) return -1;
	d = js_get_num(&f, name, -1);
	Kfree(meta);
	return d;
}

/* A box to take into: an empty figure */
LOCAL ER make_box( TS_UUID *box )
{
	T_OBCRE		c;
	CONST char	*m = "{\"name\":\"試験の箱\",\"refCount\":1,\"recordCount\":1}";
	CONST char	*r = "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>";
	ER		er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)m;
	c.jsonsz = s_len(m);
	er = ob_cre_obj(&c, box);
	if ( er >= E_OK ) er = om_obj_record_put(box, 0, OB_RT_TAD, 0, (CONST UB *)r, s_len(r));
	return er;
}

LOCAL void test_text( void )
{
	TS_UUID	u;
	UB	v[64], *rec;
	SZ	size = 0;
	OUT	o;

	KT_ASSERT_ER(take("memo.TXT", (CONST UB *)text, s_len(text), NULL, &u), E_OK);

	/* what the file was: in the metadata */
	KT_ASSERT(file_member(&u, "name", v, sizeof(v)) > 0 && same_str(v, "memo.TXT"));
	KT_ASSERT(file_member(&u, "mediatype", v, sizeof(v)) > 0 && same_str(v, "text/plain"));
	KT_ASSERT(file_member(&u, "source", v, sizeof(v)) > 0 && same_str(v, "ktest"));
	KT_ASSERT_EQ(file_num(&u, "size"), s_len(text));
	KT_ASSERT(file_member(&u, "mtime", v, sizeof(v)) > 0 && v[4] == '-');

	/* and not in the records: record 0 shows the lines, record 1 is the file */
	rec = om_obj_record(&u, 0, &size);
	KT_ASSERT(rec != NULL && !has(rec, (INT)size, "<info") && has(rec, (INT)size, "二行目 &lt;tag&gt;"));
	if ( rec != NULL ) Kfree(rec);
	KT_ASSERT_EQ(xf_data_rec(&u), 1);

	/* the icon of the template of the program it opens in */
	rec = om_obj_icon(&u, &size);
	KT_ASSERT(rec != NULL && size > 6 && rec[0] == 0 && rec[2] == 1);
	if ( rec != NULL ) Kfree(rec);

	knl_memset(&o, 0, sizeof(o));
	KT_ASSERT_ER(xf_export(&u, out_write, &o, v, sizeof(v)), E_OK);
	KT_ASSERT(same_str(v, "memo.TXT"));
	KT_ASSERT_EQ(o.n, s_len(text));
	KT_ASSERT(same_bytes(o.b, (CONST UB *)text, s_len(text)));
	(void)ob_del_obj(&u);
}

LOCAL void test_xmltad( void )
{
	TS_UUID	u;
	T_OBREF	r;
	UB	v[64], *rec;
	SZ	size = 0;
	OUT	o;

	KT_ASSERT_ER(take("doc.xtad", (CONST UB *)xtad, s_len(xtad), NULL, &u), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.nrec, 1);
	KT_ASSERT_EQ(xf_data_rec(&u), 0);
	rec = om_obj_record(&u, 0, &size);
	KT_ASSERT(rec != NULL && (INT)size == s_len(xtad) && same_bytes(rec, (CONST UB *)xtad, (INT)size));
	if ( rec != NULL ) Kfree(rec);
	KT_ASSERT(file_member(&u, "mediatype", v, sizeof(v)) > 0 && same_str(v, "application/x-xmltad"));

	knl_memset(&o, 0, sizeof(o));
	KT_ASSERT_ER(xf_export(&u, out_write, &o, v, sizeof(v)), E_OK);
	KT_ASSERT(same_str(v, "doc.xtad"));
	KT_ASSERT(o.n == s_len(xtad) && same_bytes(o.b, (CONST UB *)xtad, o.n));
	(void)ob_del_obj(&u);
}

LOCAL void test_exec( void )
{
	TS_UUID	u;
	UB	*meta;
	SZ	size = 0;
	T_JSON	root, tf, ex;
	OUT	o;

	KT_ASSERT_ER(take("tool.bin", elf, sizeof(elf), NULL, &u), E_OK);
	meta = om_obj_meta(&u, &size);
	KT_ASSERT(meta != NULL && js_parse(meta, (INT)size, &root) >= E_OK
		  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "exec", &ex) >= E_OK
		  && js_get_num(&ex, "record", -1) == 1);
	if ( meta != NULL ) Kfree(meta);
	meta = om_obj_icon(&u, &size);			/* no program opens it: no icon */
	KT_ASSERT(meta == NULL);
	if ( meta != NULL ) Kfree(meta);
	knl_memset(&o, 0, sizeof(o));
	KT_ASSERT_ER(xf_export(&u, out_write, &o, NULL, 0), E_OK);
	KT_ASSERT(o.n == (INT)sizeof(elf) && same_bytes(o.b, elf, o.n));
	(void)ob_del_obj(&u);
}

LOCAL void test_box( void )
{
	TS_UUID	box, a, b, got, ids[4];
	UB	v[64], *rec;
	SZ	size = 0;
	INT	n;

	KT_ASSERT_ER(make_box(&box), E_OK);
	KT_ASSERT_ER(take("First.txt", (CONST UB *)text, s_len(text), &box, &a), E_OK);
	KT_ASSERT_ER(take("second.dat", elf, sizeof(elf), &box, &b), E_OK);

	/* found by the file's name, case ignored */
	KT_ASSERT_ER(xf_find(&box, (CONST UB *)"first.TXT", &got), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &a), 0);
	KT_ASSERT_ER(xf_find(&box, (CONST UB *)"third.txt", &got), E_NOEXS);

	/* renamed as a file: both names change */
	KT_ASSERT_ER(xf_rename(&b, (CONST UB *)"renamed.db"), E_OK);
	KT_ASSERT_ER(xf_find(&box, (CONST UB *)"renamed.db", &got), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &b), 0);
	KT_ASSERT_ER(xf_find(&box, (CONST UB *)"second.dat", &got), E_NOEXS);
	KT_ASSERT(om_store_name(&b, v, sizeof(v)) > 0 && same_str(v, "renamed"));

	/* a link added twice is there once; taken out, it is gone */
	KT_ASSERT_ER(om_obj_link_add(&box, &a), E_OK);
	rec = om_obj_record(&box, 0, &size);
	n = ( rec != NULL ) ? om_store_links(rec, size, ids, 4) : -1;
	if ( rec != NULL ) Kfree(rec);
	KT_ASSERT_EQ(n, 2);
	KT_ASSERT_ER(om_obj_link_del(&box, &a), E_OK);
	KT_ASSERT_ER(xf_find(&box, (CONST UB *)"First.txt", &got), E_NOEXS);

	(void)ob_del_obj(&a);
	(void)ob_del_obj(&b);
	(void)ob_del_obj(&box);
}

LOCAL ER put_file( CONST char *path, CONST char *s, INT len )
{
	INT	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC), n;

	if ( fd < 0 ) return E_IO;
	n = fs_write(fd, s, len);
	fs_close(fd);
	return ( n == len ) ? E_OK : E_IO;
}

/* An icon file's head: one 1x1 image, no more than the store checks */
LOCAL CONST UB	ico[] = { 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 32, 0, 0, 0, 0, 0, 22, 0, 0, 0 };

LOCAL void test_tadjs( void )
{
	CONST char	*j = "{\"name\":\"持込み\",\"refCount\":1,\"recordCount\":2,"
			     "\"applist\":{\"basic-text-editor\":{\"defaultOpen\":true}}}";
	CONST char	*r0 = "<tad version=\"1.0\"><document><p>零</p></document></tad>";
	CONST char	*r1 = "<tad version=\"1.0\"><document><p>壱</p></document></tad>";
	TS_UUID		u;
	T_OBREF		r;
	UB		v[64], *rec, *meta;
	SZ		size = 0;

	if ( fs_open("/boot/HELLO.ELF", O_RDONLY) < 0 ) KT_SKIP("no file system");
	KT_ASSERT_ER(ts_str_to_uuid(TJ_UUID, &u), E_OK);
	(void)ob_del_obj(&u);				/* from a run before */
	(void)fs_mkdir("/boot/XFTJS");
	KT_ASSERT_ER(put_file("/boot/XFTJS/" TJ_UUID ".json", j, s_len(j)), E_OK);
	KT_ASSERT_ER(put_file("/boot/XFTJS/" TJ_UUID "_0.xtad", r0, s_len(r0)), E_OK);
	KT_ASSERT_ER(put_file("/boot/XFTJS/" TJ_UUID "_1.xtad", r1, s_len(r1)), E_OK);

	KT_ASSERT_ER(xf_import_tadjs("/boot/XFTJS", &u, "fat", NULL, NULL), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);	/* under the UUID it came with */
	KT_ASSERT_EQ(r.nrec, 2);
	KT_ASSERT(same_str(r.name, "持込み"));
	rec = om_obj_record(&u, 1, &size);
	KT_ASSERT(rec != NULL && (INT)size == s_len(r1) && same_bytes(rec, (CONST UB *)r1, (INT)size));
	if ( rec != NULL ) Kfree(rec);

	/* its metadata as it came, with where it came from put in */
	meta = om_obj_meta(&u, &size);
	KT_ASSERT(meta != NULL && has(meta, (INT)size, "basic-text-editor"));
	if ( meta != NULL ) Kfree(meta);
	KT_ASSERT(file_member(&u, "source", v, sizeof(v)) > 0 && same_str(v, "fat"));
	KT_ASSERT(file_member(&u, "name", v, sizeof(v)) > 0 && same_str(v, TJ_UUID "_0.xtad"));

	/* taken in without an icon, as the set had none */
	rec = om_obj_icon(&u, &size);
	KT_ASSERT(rec == NULL);
	if ( rec != NULL ) Kfree(rec);

	/* taken in again: left as it is, but given the icon the set has now */
	KT_ASSERT_ER(put_file("/boot/XFTJS/" TJ_UUID ".ico", (CONST char *)ico, sizeof(ico)), E_OK);
	KT_ASSERT_ER(xf_import_tadjs("/boot/XFTJS", &u, "fat", NULL, NULL), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.nrec, 2);
	rec = om_obj_icon(&u, &size);
	KT_ASSERT(rec != NULL && (INT)size == (INT)sizeof(ico) && same_bytes(rec, ico, (INT)size));
	if ( rec != NULL ) Kfree(rec);
	(void)fs_unlink("/boot/XFTJS/" TJ_UUID ".ico");
	(void)ob_del_obj(&u);
}

EXPORT void ktest_xf( void )
{
	KT_RUN(test_text);
	KT_RUN(test_xmltad);
	KT_RUN(test_exec);
	KT_RUN(test_box);
	KT_RUN(test_tadjs);
}
