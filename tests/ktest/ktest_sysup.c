/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_sysup.c
 *	The system updated from its package (design 11.8, application/desktop/dtsysup.c)
 *
 *	A package is made here as tools/mktpk.py --system makes one, for a
 *	newer version than the system is: an object of the system's own, an
 *	object of the person's that is to be kept, a box the person added a
 *	link to, an object the system does not have yet, the machine's real
 *	settings as one to be kept, and boot files, one to be replaced and
 *	two to be put only where there is none. Put on, the system's objects
 *	and the boot file are the package's, the person's object, the
 *	settings, the link the person added and the start-up settings the
 *	person wrote are as they were, the previous boot file is kept as
 *	.old, and nothing is left half done. Put on again, and again from
 *	the start as after a cut in the power, it comes out the same. A
 *	package for another machine or processor, one that is not newer
 *	without being told, and one that is not a system's, are refused.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/fs.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/wm.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include "../../application/desktop/desktop.h"

#define SU_SYS		"01a0d8c4-5fa0-7a01-8b01-00000005fa01"	/* the system's own */
#define SU_KEEP		"01a0d8c4-5fa0-7a02-8b02-00000005fa02"	/* the person's, kept */
#define SU_BOX		"01a0d8c4-5fa0-7a03-8b03-00000005fa03"	/* a box the person adds to */
#define SU_NEW		"01a0d8c4-5fa0-7a04-8b04-00000005fa04"	/* one the system does not have */
#define SU_MINE		"01a0d8c4-5fa0-7a05-8b05-00000005fa05"	/* what the person linked from the box */

#if defined(_RPI5_)
#define SU_MACH		"rpi5"
#elif defined(_RPI4_)
#define SU_MACH		"rpi4"
#elif defined(_X64_)
#define SU_MACH		"x64"
#else
#define SU_MACH		"qemu_virt"
#endif
#if defined(_X64_)
#define SU_ARCH		"x86_64"
#define SU_OTHER_ARCH	"aarch64"
#else
#define SU_ARCH		"aarch64"
#define SU_OTHER_ARCH	"x86_64"
#endif

#define SU_IMG		"/boot/SUTEST.IMG"
#define SU_TXT		"/boot/SUTEST.TXT"
#define SU_DIR		"/boot/sutest"
#define SU_NEWTXT	"/boot/sutest/NEW.TXT"

#define NENT_MAX	24

LOCAL INT	nent;
LOCAL char	ename[NENT_MAX][48];
LOCAL CONST UB	*edata[NENT_MAX];
LOCAL SZ	esize[NENT_MAX];
LOCAL UW	ert[NENT_MAX];

LOCAL SZ s_len( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL same( CONST UB *a, SZ an, CONST char *b )
{
	SZ	i;

	if ( a == NULL || an != s_len(b) ) return FALSE;
	for ( i = 0; i < an; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL void ent( CONST char *uuid, CONST char *tail, CONST char *data, UW rt )
{
	if ( nent >= NENT_MAX ) return;
	knl_strcpy(ename[nent], uuid);
	knl_strcat(ename[nent], tail);
	edata[nent] = (CONST UB *)data;
	esize[nent] = s_len(data);
	ert[nent] = rt;
	nent++;
}

LOCAL void put32( UB *p, UW v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
	p[2] = (UB)( v >> 16 );
	p[3] = (UB)( v >> 24 );
}

/* The package as tools/mktpk.py lays it out, with package.json first */
LOCAL UB *pk_make( CONST char *man, SZ *p_len )
{
	SZ	len = 16 + 64 * (SZ)( nent + 1 ), at;
	UB	*raw;
	INT	i, k;

	len += s_len(man);
	for ( i = 0; i < nent; i++ ) len += esize[i];
	raw = (UB *)Kmalloc(len);
	if ( raw == NULL ) return NULL;
	knl_memset(raw, 0, len);
	raw[0] = 'T'; raw[1] = 'S'; raw[2] = 'P'; raw[3] = 'K';
	raw[4] = 1;
	raw[6] = (UB)( nent + 1 );
	at = 16 + 64 * (SZ)( nent + 1 );
	for ( i = -1; i < nent; i++ ) {
		UB		*d = raw + 16 + 64 * ( i + 1 );
		CONST char	*nm = ( i < 0 ) ? "package.json" : ename[i];
		CONST UB	*b = ( i < 0 ) ? (CONST UB *)man : edata[i];
		SZ		n = ( i < 0 ) ? s_len(man) : esize[i];

		for ( k = 0; nm[k] != 0 && k < 47; k++ ) d[k] = (UB)nm[k];
		put32(d + 48, (UW)at);
		put32(d + 52, (UW)n);
		put32(d + 56, ( i < 0 ) ? OB_RT_SYSDATA : ert[i]);
		knl_memcpy(raw + at, b, n);
		at += n;
	}
	*p_len = len;
	return raw;
}

/* The package taken in, as ファイル変換 makes an object of a file */
LOCAL ER pk_object( CONST char *man, TS_UUID *p_u )
{
	CONST char	*j = "{\"name\":\"システムのパッケージ\",\"refCount\":0,\"recordCount\":2}";
	CONST char	*x = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>sysup.tpk</p></document></tad>";
	T_OBCRE		c;
	UB		*raw;
	SZ		len = 0;
	ER		er;

	raw = pk_make(man, &len);
	if ( raw == NULL ) return E_NOMEM;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)j;
	c.jsonsz = s_len(j);
	er = ob_cre_obj(&c, p_u);
	if ( er >= E_OK ) er = om_obj_record_put(p_u, 0, OB_RT_TAD, 0, (CONST UB *)x, s_len(x));
	if ( er >= E_OK ) er = om_obj_record_put(p_u, 1, OB_RT_SYSDATA, 0, raw, len);
	Kfree(raw);
	return er;
}

LOCAL ER mk_object( CONST char *us, CONST char *name, CONST char *rec0, CONST char *rec1 )
{
	char	j[160];
	T_OBCRE	c;
	TS_UUID	u, got;
	ER	er;

	knl_strcpy(j, "{\"name\":\"");
	knl_strcat(j, name);
	knl_strcat(j, "\",\"refCount\":0,\"recordCount\":0}");
	(void)ts_str_to_uuid(us, &u);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)j;
	c.jsonsz = s_len(j);
	c.uuid = u;
	er = ob_cre_obj(&c, &got);
	if ( er >= E_OK ) er = om_obj_record_put(&u, 0, OB_RT_TAD, 0, (CONST UB *)rec0, s_len(rec0));
	if ( er >= E_OK && rec1 != NULL ) {
		er = om_obj_record_put(&u, 1, OB_RT_SYSDATA, 0, (CONST UB *)rec1, s_len(rec1));
	}
	return er;
}

LOCAL ER file_put( CONST char *path, CONST char *s )
{
	INT	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	ER	er;

	if ( fd < 0 ) return (ER)fd;
	er = ( fs_write(fd, s, s_len(s)) == (INT)s_len(s) ) ? E_OK : E_IO;
	fs_close(fd);
	return er;
}

LOCAL BOOL file_is( CONST char *path, CONST char *s )
{
	UB	b[128];
	INT	fd = fs_open(path, O_RDONLY), n;

	if ( fd < 0 ) return FALSE;
	n = fs_read(fd, b, sizeof(b));
	fs_close(fd);
	return (BOOL)( n >= 0 && same(b, (SZ)n, s) );
}

LOCAL BOOL rec_is( CONST char *us, INT recno, CONST char *s )
{
	TS_UUID	u;
	UB	*b;
	SZ	n = 0;
	BOOL	ok;

	(void)ts_str_to_uuid(us, &u);
	b = om_obj_record(&u, recno, &n);
	ok = same(b, n, s);
	if ( b != NULL ) Kfree(b);
	return ok;
}

LOCAL INT links_of( CONST char *us, TS_UUID *ids, INT max )
{
	TS_UUID	u;
	UB	*b;
	SZ	n = 0;
	INT	k;

	(void)ts_str_to_uuid(us, &u);
	b = om_obj_record(&u, 0, &n);
	if ( b == NULL ) return -1;
	k = om_store_links(b, n, ids, max);
	Kfree(b);
	return k;
}

LOCAL BOOL has_link( TS_UUID *ids, INT n, CONST char *us )
{
	TS_UUID	u;
	INT	i;

	(void)ts_str_to_uuid(us, &u);
	for ( i = 0; i < n; i++ ) {
		if ( ts_uuid_cmp(&ids[i], &u) == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL void del_us( CONST char *us )
{
	TS_UUID	u;
	T_OBPRT	*prt;

	if ( ts_str_to_uuid(us, &u) < E_OK ) return;
	prt = knl_ob_prt_get(&u);
	if ( prt != NULL && ( prt->attr & ( OB_A_PERM | OB_A_RONLY ) ) ) {
		prt->attr &= ~( OB_A_PERM | OB_A_RONLY );
		(void)ob_set_prt(&u, prt);
	}
	if ( prt != NULL ) Kfree(prt);
	(void)ob_del_obj(&u);
}

LOCAL void clean( void )
{
	del_us(SU_BOX);
	del_us(SU_SYS);
	del_us(SU_KEEP);
	del_us(SU_NEW);
	del_us(SU_MINE);
	(void)fs_unlink(SU_IMG);
	(void)fs_unlink(SU_IMG ".old");
	(void)fs_unlink(SU_IMG ".new");
	(void)fs_unlink(SU_TXT);
	(void)fs_unlink(SU_NEWTXT);
	(void)fs_rmdir(SU_DIR);
}

/* package.json of a system's package */
LOCAL void manifest( char *m, CONST char *ver, CONST char *mach, CONST char *arch, CONST char *kind )
{
	knl_strcpy(m, "{\"format\":1,\"kind\":\"");
	knl_strcat(m, kind);
	knl_strcat(m, "\",\"id\":\"tessronos\",\"name\":\"TessronOS\",\"version\":\"");
	knl_strcat(m, ver);
	knl_strcat(m, "\",\"machine\":\"");
	knl_strcat(m, mach);
	knl_strcat(m, "\",\"arch\":\"");
	knl_strcat(m, arch);
	knl_strcat(m, "\",\"objects\":[\"" SU_SYS "\",\"" SU_KEEP "\",\"" SU_BOX "\",\"" SU_NEW "\",\""
		   SYSDEF_CONF_DEV "\"],\"keep\":[\"" SU_KEEP "\",\"" SYSDEF_CONF_DEV "\"],"
		   "\"links\":[\"" SU_BOX "\"],\"boot\":[\"SUTEST.IMG\"],"
		   "\"bootkeep\":[\"SUTEST.TXT\",\"sutest/NEW.TXT\"]}");
}

#define BOX_OLD	"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>" \
	"<link id=\"" SU_SYS "_0.xtad\" vobjid=\"01a0d8c4-5fa1-7a01-8b01-00000005fa11\" vobjleft=\"8\" vobjtop=\"8\" vobjright=\"248\" vobjbottom=\"33\" height=\"25\" chsz=\"14\"/>" \
	"<link id=\"" SU_MINE "_0.xtad\" vobjid=\"01a0d8c4-5fa1-7a02-8b02-00000005fa12\" vobjleft=\"8\" vobjtop=\"40\" vobjright=\"248\" vobjbottom=\"65\" height=\"25\" chsz=\"14\"/>" \
	"</figure></tad>"
#define BOX_NEW	"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>" \
	"<link id=\"" SU_SYS "_0.xtad\" vobjid=\"01a0d8c4-5fa1-7a01-8b01-00000005fa11\" vobjleft=\"8\" vobjtop=\"8\" vobjright=\"248\" vobjbottom=\"33\" height=\"25\" chsz=\"14\"/>" \
	"<link id=\"" SU_NEW "_0.xtad\" vobjid=\"01a0d8c4-5fa1-7a03-8b03-00000005fa13\" vobjleft=\"8\" vobjtop=\"40\" vobjright=\"248\" vobjbottom=\"65\" height=\"25\" chsz=\"14\"/>" \
	"</figure></tad>"
#define DOC(s)	"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>" s "</p></document></tad>"

LOCAL void test_sysup( void )
{
	static char	man[1024], ver[24];
	TS_UUID		pkg, other, same_v, notsys, devu, ids[8];
	UB		text[DT_TEXT_MAX], *dev0 = NULL, *dev1 = NULL;
	SZ		dn0 = 0, dn1 = 0;
	char		v[64];
	INT		n, total = 0;
	TS_UUID		u;
	T_OBREF		r;

	clean();
	(void)wm_dev_conf_put("SYSUP", "");
	(void)ts_str_to_uuid(SYSDEF_CONF_DEV, &devu);
	if ( ob_ref_obj(&devu, &r) < E_OK ) {
		KT_SKIP("no machine settings");
	}

	/* the system as it was: its own object, the person's, the box with the person's link */
	KT_ASSERT_ER(mk_object(SU_SYS, "更新の試験・システム", DOC("old system"), "OLD-SYSTEM-RECORD"), E_OK);
	KT_ASSERT_ER(mk_object(SU_KEEP, "更新の試験・利用者", DOC("mine"), "PERSON'S SETTINGS"), E_OK);
	KT_ASSERT_ER(mk_object(SU_MINE, "更新の試験・利用者の実身", DOC("mine too"), NULL), E_OK);
	KT_ASSERT_ER(mk_object(SU_BOX, "更新の試験・箱", BOX_OLD, NULL), E_OK);
	KT_ASSERT_ER(file_put(SU_IMG, "old kernel"), E_OK);
	KT_ASSERT_ER(file_put(SU_TXT, "the person's settings"), E_OK);
	dev1 = om_obj_record(&devu, 1, &dn1);
	KT_ASSERT(dev1 != NULL);

	/* the package: a newer version for this machine */
	nent = 0;
	ent(SU_SYS, ".json", "{\"name\":\"更新の試験・新しいシステム\",\"tessronos\":{\"x\":2}}", OB_RT_SYSDATA);
	ent(SU_SYS, "_0.xtad", DOC("new system"), OB_RT_TAD);
	ent(SU_SYS, "_1.bin", "NEW-SYSTEM-RECORD, LONGER", OB_RT_SYSDATA);
	ent(SU_SYS, "_0_0.png", "a resource", OB_RT_SYSDATA);
	ent(SU_KEEP, ".json", "{\"name\":\"更新の試験・既定\",\"tessronos\":{\"update\":\"keep\"}}", OB_RT_SYSDATA);
	ent(SU_KEEP, "_0.xtad", DOC("default"), OB_RT_TAD);
	ent(SU_KEEP, "_1.bin", "DEFAULT SETTINGS", OB_RT_SYSDATA);
	ent(SU_BOX, ".json", "{\"name\":\"更新の試験・新しい箱\",\"tessronos\":{\"update\":\"links\"}}", OB_RT_SYSDATA);
	ent(SU_BOX, "_0.xtad", BOX_NEW, OB_RT_TAD);
	ent(SU_NEW, ".json", "{\"name\":\"更新の試験・新しい実身\"}", OB_RT_SYSDATA);
	ent(SU_NEW, "_0.xtad", DOC("brand new"), OB_RT_TAD);
	ent(SU_NEW, "_1.bin", "NEW OBJECT", OB_RT_SYSDATA);
	ent(SYSDEF_CONF_DEV, ".json", "{\"name\":\"機器の設定\"}", OB_RT_SYSDATA);
	ent(SYSDEF_CONF_DEV, "_0.xtad", DOC("defaults"), OB_RT_TAD);
	ent(SYSDEF_CONF_DEV, "_1.bin", "VIDEOMODE\t640x480\n", OB_RT_SYSDATA);
	ent("boot/", "SUTEST.IMG", "new kernel, a longer one", OB_RT_SYSDATA);
	ent("boot/", "SUTEST.TXT", "the system's settings", OB_RT_SYSDATA);
	ent("boot/", "sutest/NEW.TXT", "settings put where there were none", OB_RT_SYSDATA);

	manifest(man, "R9.999", SU_MACH, SU_ARCH, "system");
	KT_ASSERT_ER(pk_object(man, &pkg), E_OK);
	KT_ASSERT_ER(dt_sysup_check(&pkg, text, sizeof(text)), E_OK);
	KT_ASSERT(text[0] == 'R' && text[1] == '9' && text[6] == '\t' && text[7] == 'R');
	knl_strncpy(ver, (CONST char *)text + 7, sizeof(ver) - 1);
	ver[sizeof(ver) - 1] = 0;
	tm_printf((UB *)"ktest sysup: package R9.999, system %s\n", ver);

	/* refused: another machine, another processor, the same version, not a system's */
	manifest(man, "R9.999", ( SU_MACH[0] == 'q' ) ? "rpi5" : "qemu_virt", SU_ARCH, "system");
	KT_ASSERT_ER(pk_object(man, &other), E_OK);
	KT_ASSERT_ER(dt_sysup_check(&other, text, sizeof(text)), E_NOSPT);
	KT_ASSERT_ER(dt_sysup_run(&other, TRUE), E_NOSPT);
	(void)ob_del_obj(&other);
	manifest(man, "R9.999", SU_MACH, SU_OTHER_ARCH, "system");
	KT_ASSERT_ER(pk_object(man, &other), E_OK);
	KT_ASSERT_ER(dt_sysup_check(&other, text, sizeof(text)), E_NOSPT);
	(void)ob_del_obj(&other);
	manifest(man, ver, SU_MACH, SU_ARCH, "system");
	KT_ASSERT_ER(pk_object(man, &same_v), E_OK);
	KT_ASSERT_ER(dt_sysup_check(&same_v, text, sizeof(text)), E_LIMIT);
	KT_ASSERT_ER(dt_sysup_begin(&same_v, FALSE, 0, &total), E_LIMIT);
	(void)ob_del_obj(&same_v);
	manifest(man, "R9.999", SU_MACH, SU_ARCH, "application");
	KT_ASSERT_ER(pk_object(man, &notsys), E_OK);
	KT_ASSERT_ER(dt_sysup_check(&notsys, text, sizeof(text)), E_OBJ);
	(void)ob_del_obj(&notsys);
	/* nothing of the refused ones was done */
	KT_ASSERT(rec_is(SU_SYS, 1, "OLD-SYSTEM-RECORD"));
	KT_ASSERT(file_is(SU_IMG, "old kernel"));

	/* put on */
	KT_ASSERT_ER(dt_sysup_run(&pkg, FALSE), E_OK);
	KT_ASSERT(!wm_dev_conf_get("SYSUP", v, sizeof(v)) || v[0] == 0);
	KT_ASSERT(rec_is(SU_SYS, 0, DOC("new system")));
	KT_ASSERT(rec_is(SU_SYS, 1, "NEW-SYSTEM-RECORD, LONGER"));
	{
		UB	*res;
		SZ	rn = 0;

		(void)ts_str_to_uuid(SU_SYS, &u);
		res = om_obj_res(&u, (CONST UB *)"_0_0.png", &rn);
		KT_ASSERT(same(res, rn, "a resource"));
		if ( res != NULL ) Kfree(res);
		KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
		KT_ASSERT(same(r.name, s_len((CONST char *)r.name), "更新の試験・新しいシステム"));
	}
	KT_ASSERT(rec_is(SU_KEEP, 0, DOC("mine")));
	KT_ASSERT(rec_is(SU_KEEP, 1, "PERSON'S SETTINGS"));
	(void)ts_str_to_uuid(SU_KEEP, &u);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT(same(r.name, s_len((CONST char *)r.name), "更新の試験・利用者"));
	KT_ASSERT(rec_is(SU_NEW, 0, DOC("brand new")));
	KT_ASSERT(rec_is(SU_NEW, 1, "NEW OBJECT"));
	(void)ts_str_to_uuid(SU_NEW, &u);
	KT_ASSERT(om_store_refs(&u) >= 1);		/* the box links to it */
	n = links_of(SU_BOX, ids, 8);
	KT_ASSERT_EQ(n, 3);
	KT_ASSERT(has_link(ids, n, SU_SYS) && has_link(ids, n, SU_MINE) && has_link(ids, n, SU_NEW));
	(void)ts_str_to_uuid(SU_BOX, &u);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT(same(r.name, s_len((CONST char *)r.name), "更新の試験・箱"));
	dev0 = om_obj_record(&devu, 1, &dn0);
	KT_ASSERT(dev0 != NULL && dn0 == dn1 && dev1 != NULL);
	if ( dev0 != NULL && dev1 != NULL && dn0 == dn1 ) {
		for ( n = 0; n < (INT)dn0 && dev0[n] == dev1[n]; n++ ) ;
		KT_ASSERT_EQ(n, (INT)dn0);		/* the machine's settings as they were */
	}
	KT_ASSERT(file_is(SU_IMG, "new kernel, a longer one"));
	KT_ASSERT(file_is(SU_IMG ".old", "old kernel"));
	{
		T_FSTAT	st;

		KT_ASSERT(fs_stat(SU_IMG ".new", &st) < E_OK);
	}
	KT_ASSERT(file_is(SU_TXT, "the person's settings"));
	KT_ASSERT(file_is(SU_NEWTXT, "settings put where there were none"));

	/* again, told to: the same comes out, no link twice, the .old not the new one */
	KT_ASSERT_ER(dt_sysup_run(&pkg, TRUE), E_OK);
	KT_ASSERT_EQ(links_of(SU_BOX, ids, 8), 3);
	KT_ASSERT(file_is(SU_IMG ".old", "old kernel"));
	KT_ASSERT(rec_is(SU_KEEP, 1, "PERSON'S SETTINGS"));

	/*
	 * cut in the power after the objects: the boot file and the person's
	 * object set back, the update said begun, and done again at the start
	 */
	KT_ASSERT_ER(file_put(SU_IMG, "old kernel"), E_OK);
	KT_ASSERT_ER(file_put(SU_IMG ".new", "new ker"), E_OK);	/* written short */
	(void)ts_str_to_uuid(SU_SYS, &u);
	KT_ASSERT_ER(om_obj_record_put(&u, 1, OB_RT_SYSDATA, 0, (CONST UB *)"OLD-SYSTEM-RECORD", 17), E_OK);
	{
		char	us[TS_UUID_STRLEN + 1];

		(void)ts_uuid_to_str(&pkg, us, sizeof(us));
		KT_ASSERT_ER(wm_dev_conf_put("SYSUP", us), E_OK);
	}
	dt_sysup_resume(NULL);
	KT_ASSERT(!wm_dev_conf_get("SYSUP", v, sizeof(v)) || v[0] == 0);
	KT_ASSERT(rec_is(SU_SYS, 1, "NEW-SYSTEM-RECORD, LONGER"));
	KT_ASSERT(file_is(SU_IMG, "new kernel, a longer one"));
	KT_ASSERT(file_is(SU_IMG ".old", "old kernel"));
	KT_ASSERT(file_is(SU_TXT, "the person's settings"));

	/* the steps one request at a time, as システム環境設定 asks for them, and only by who began */
	{
		INT	done = 0;

		KT_ASSERT_ER(dt_sysup_begin(&pkg, TRUE, 7, &total), E_OK);
		KT_ASSERT(total > 0);
		KT_ASSERT_ER(dt_sysup_step(8, &done, &total), E_OACV);
		while ( done < total ) {
			ER	er = dt_sysup_step(7, &done, &total);

			KT_ASSERT_ER(er, E_OK);
			if ( er < E_OK ) break;
		}
		KT_ASSERT_ER(dt_sysup_step(7, &done, &total), E_NOEXS);
		KT_ASSERT(!wm_dev_conf_get("SYSUP", v, sizeof(v)) || v[0] == 0);
	}

	(void)ob_del_obj(&pkg);
	if ( dev0 != NULL ) Kfree(dev0);
	if ( dev1 != NULL ) Kfree(dev1);
	clean();
}

/* ---------------------------------------------------------------- the PC's boot files */

/*
 * A boot partition of its own below /boot, as an ESP of a PC the system
 * was put on with システム登録 beside Windows: \EFI\TESSRON\ holds the
 * loader, the kernel, the settings and INSTALL.TXT, \EFI\BOOT\ the
 * loader of Windows.
 */
#define R		"/boot/sutestrt"
#define R_TS		R "/EFI/TESSRON"
#define R_BOOT		R "/EFI/BOOT"
#define R_LOADER	R_TS "/BOOTX64.EFI"
#define R_KERNEL	R_TS "/tessronos.elf"
#define R_CMDLINE	R_TS "/cmdline.txt"
#define R_INSTALL	R_TS "/INSTALL.TXT"
#define R_FALLBACK	R_BOOT "/BOOTX64.EFI"

#define OLD_LOADER	"MZ old TessronOS loader"
#define NEW_LOADER	"MZ new TessronOS loader"
#define WIN_LOADER	"MZ Windows Boot Manager"
#define OLD_KERNEL	"old kernel"
#define NEW_KERNEL	"new kernel, the one to start"
#define PERSON_CMD	"video=1280x800"

/* Kilobytes of the ESP of a Surface Go 2 that Windows set up, and of what is on it, scaled down 64 times */
#define ESP_SCALE	64
#define ESP_BYTES	( 200ULL * 1024 * 1024 )	/* nvmea0: 409600 sectors */
#define ESP_WINDOWS	( 35ULL * 1024 * 1024 )		/* \EFI\Microsoft\ and \EFI\Boot\ */
#define ESP_KERNEL	( 69ULL * 1024 * 1024 )		/* tessronos.elf */
#define ESP_RECORDS	( 4ULL * 1024 * 1024 )		/* the loader's records, LOADER.TXT, HW.TXT, ACPI\ */

LOCAL CONST char * CONST r_files[] = {
	R_LOADER, R_KERNEL, R_CMDLINE, R_INSTALL, R_FALLBACK, R "/tessronos.elf", R "/init.elf",
	R "/cmdline.txt", R "/EFI/TESSRON/init.elf", NULL
};

LOCAL void r_clean( void )
{
	char	p[96];
	INT	i;

	for ( i = 0; r_files[i] != NULL; i++ ) {
		knl_strcpy(p, r_files[i]);
		(void)fs_unlink(p);
		knl_strcat(p, ".new");
		(void)fs_unlink(p);
		knl_strcpy(p, r_files[i]);
		knl_strcat(p, ".old");
		(void)fs_unlink(p);
	}
	(void)fs_rmdir(R_TS);
	(void)fs_rmdir(R_BOOT);
	(void)fs_rmdir(R "/EFI");
	(void)fs_rmdir(R);
}

/* n bytes of c written to path */
LOCAL ER file_fill( CONST char *path, UB c, SZ n )
{
	UB	*b = (UB *)Kmalloc(n);
	INT	fd;
	ER	er;

	if ( b == NULL ) return E_NOMEM;
	knl_memset(b, c, n);
	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	er = ( fd >= 0 && fs_write(fd, b, n) == (INT)n ) ? E_OK : E_IO;
	if ( fd >= 0 ) fs_close(fd);
	Kfree(b);
	return er;
}

LOCAL BOOL gone( CONST char *path )
{
	T_FSTAT	st;

	return (BOOL)( fs_stat(path, &st) < E_OK );
}

/*
 * The file the UEFI loader reads, looked for as it looks (loader.c
 * read_near): in \EFI\TESSRON\ (its own directory, and the next) and
 * the root, in each NAME, NAME.new, NAME.old. Whether what it reads is s.
 */
LOCAL BOOL loader_reads( CONST char *name, CONST char *s )
{
	static CONST char * CONST dirs[] = { R "/EFI/TESSRON/", R "/" };
	static CONST char * CONST tails[] = { "", ".new", ".old" };
	char	p[96];
	T_FSTAT	st;
	INT	i, j;

	for ( i = 0; i < 2; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			knl_strcpy(p, dirs[i]);
			knl_strcat(p, name);
			knl_strcat(p, tails[j]);
			if ( fs_stat(p, &st) >= E_OK ) return file_is(p, s);
		}
	}
	return FALSE;
}

/* The ESP as システム登録 left it beside Windows, with the update before's .old when old_k */
LOCAL ER r_setup( SZ old_k )
{
	ER	er;

	r_clean();
	(void)fs_mkdir(R);
	(void)fs_mkdir(R "/EFI");
	(void)fs_mkdir(R_TS);
	(void)fs_mkdir(R_BOOT);
	er = file_put(R_INSTALL, "TessronOS installed by システム登録\r\nmode=coexist\r\n");
	if ( er >= E_OK ) er = file_put(R_LOADER, OLD_LOADER);
	if ( er >= E_OK ) er = file_put(R_KERNEL, OLD_KERNEL);
	if ( er >= E_OK ) er = file_put(R_CMDLINE, PERSON_CMD);
	if ( er >= E_OK ) er = file_put(R_FALLBACK, WIN_LOADER);
	if ( er >= E_OK && old_k > 0 ) er = file_fill(R_KERNEL ".old", 'o', old_k);
	return er;
}

/* package.json of a package of the PC's boot files only */
LOCAL void manifest_boot( char *m )
{
	knl_strcpy(m, "{\"format\":1,\"kind\":\"system\",\"id\":\"tessronos\",\"name\":\"TessronOS\","
		   "\"version\":\"R9.999\",\"machine\":\"" SU_MACH "\",\"arch\":\"" SU_ARCH "\",\"objects\":[],"
		   "\"boot\":[\"EFI/BOOT/BOOTX64.EFI\",\"tessronos.elf\",\"init.elf\"],"
		   "\"bootkeep\":[\"cmdline.txt\"]}");
}

/*
 * Put on a PC installed beside Windows, the boot files go to \EFI\TESSRON\
 * and Windows's \EFI\BOOT\BOOTX64.EFI is left as it is; nothing is put at
 * the root. On one installed on the whole disk, \EFI\BOOT\BOOTX64.EFI is
 * TessronOS's and is updated as well.
 */
LOCAL void test_sysup_installed( void )
{
	static char	man[512];
	TS_UUID		pkg;

	KT_ASSERT_ER(r_setup(0), E_OK);
	(void)wm_dev_conf_put("SYSUP", "");
	nent = 0;
	ent("boot/", "EFI/BOOT/BOOTX64.EFI", NEW_LOADER, OB_RT_SYSDATA);
	ent("boot/", "tessronos.elf", NEW_KERNEL, OB_RT_SYSDATA);
	ent("boot/", "init.elf", "init", OB_RT_SYSDATA);
	ent("boot/", "cmdline.txt", "video=default", OB_RT_SYSDATA);
	manifest_boot(man);
	KT_ASSERT_ER(pk_object(man, &pkg), E_OK);

	dt_sysup_test(R, -1);
	KT_ASSERT_ER(dt_sysup_run(&pkg, FALSE), E_OK);
	KT_ASSERT(file_is(R_LOADER, NEW_LOADER));
	KT_ASSERT(file_is(R_LOADER ".old", OLD_LOADER));
	KT_ASSERT(file_is(R_KERNEL, NEW_KERNEL));
	KT_ASSERT(file_is(R_KERNEL ".old", OLD_KERNEL));
	KT_ASSERT(gone(R_KERNEL ".new") && gone(R_LOADER ".new"));
	KT_ASSERT(file_is(R_CMDLINE, PERSON_CMD));
	KT_ASSERT(file_is(R_FALLBACK, WIN_LOADER));		/* Windows's, not written */
	KT_ASSERT(gone(R_FALLBACK ".new") && gone(R_FALLBACK ".old"));
	KT_ASSERT(gone(R "/tessronos.elf") && gone(R "/init.elf") && gone(R "/cmdline.txt"));
	KT_ASSERT(gone(R "/EFI/TESSRON/init.elf"));
	KT_ASSERT(loader_reads("tessronos.elf", NEW_KERNEL));

	/* installed on the whole disk: \EFI\BOOT\ has TessronOS's loader too */
	KT_ASSERT_ER(file_put(R_FALLBACK, OLD_LOADER), E_OK);
	KT_ASSERT_ER(dt_sysup_run(&pkg, TRUE), E_OK);
	KT_ASSERT(file_is(R_FALLBACK, NEW_LOADER));
	KT_ASSERT(file_is(R_FALLBACK ".old", OLD_LOADER));
	KT_ASSERT(file_is(R_LOADER, NEW_LOADER));
	KT_ASSERT(file_is(R_KERNEL ".old", OLD_KERNEL));	/* the same kernel: the .old kept */

	dt_sysup_test(NULL, -1);
	(void)ob_del_obj(&pkg);
	r_clean();
}

/*
 * A cut in the power at each rename of the commit: the loader is always
 * there for the firmware, the kernel always readable by the loader (the
 * old one or the new, between the kernel's two renames the new from
 * tessronos.elf.new), and the next start finishes the update.
 */
LOCAL void test_sysup_cut( void )
{
	static char	man[512];
	TS_UUID		pkg;
	char		us[TS_UUID_STRLEN + 1], v[64];
	INT		cut;

	nent = 0;
	ent("boot/", "EFI/BOOT/BOOTX64.EFI", NEW_LOADER, OB_RT_SYSDATA);
	ent("boot/", "tessronos.elf", NEW_KERNEL, OB_RT_SYSDATA);
	ent("boot/", "init.elf", "init", OB_RT_SYSDATA);
	ent("boot/", "cmdline.txt", "video=default", OB_RT_SYSDATA);
	manifest_boot(man);
	KT_ASSERT_ER(pk_object(man, &pkg), E_OK);
	(void)ts_uuid_to_str(&pkg, us, sizeof(us));

	for ( cut = 0; cut < 3; cut++ ) {		/* the loader's one rename, the kernel's two */
		KT_ASSERT_ER(r_setup(0), E_OK);
		(void)wm_dev_conf_put("SYSUP", "");
		dt_sysup_test(R, cut);
		KT_ASSERT_ER(dt_sysup_run(&pkg, FALSE), E_IO);
		KT_ASSERT(wm_dev_conf_get("SYSUP", v, sizeof(v)) && v[0] != 0);
		KT_ASSERT(file_is(R_LOADER, ( cut == 0 ) ? OLD_LOADER : NEW_LOADER));
		KT_ASSERT(loader_reads("tessronos.elf", ( cut < 2 ) ? OLD_KERNEL : NEW_KERNEL));
		if ( cut == 2 ) {
			KT_ASSERT(gone(R_KERNEL));		/* between the two renames */
			KT_ASSERT(file_is(R_KERNEL ".new", NEW_KERNEL));
		}
		KT_ASSERT(loader_reads("cmdline.txt", PERSON_CMD));

		dt_sysup_test(R, -1);
		dt_sysup_resume(NULL);
		KT_ASSERT(!wm_dev_conf_get("SYSUP", v, sizeof(v)) || v[0] == 0);
		KT_ASSERT(file_is(R_LOADER, NEW_LOADER));
		KT_ASSERT(file_is(R_LOADER ".old", OLD_LOADER));
		KT_ASSERT(file_is(R_KERNEL, NEW_KERNEL));
		KT_ASSERT(file_is(R_KERNEL ".old", OLD_KERNEL));
		KT_ASSERT(gone(R_KERNEL ".new"));
		KT_ASSERT(file_is(R_FALLBACK, WIN_LOADER));
		tm_printf((UB *)"ktest sysup: cut after %d renames, put right at the start\n", cut);
	}

	/* a cut between the kernel's two renames, then another package: the old kernel put back first */
	KT_ASSERT_ER(r_setup(0), E_OK);
	KT_ASSERT_ER(fs_rename(R_KERNEL, R_KERNEL ".old"), E_OK);
	KT_ASSERT_ER(file_put(R_KERNEL ".new", "another update's kernel"), E_OK);
	dt_sysup_test(R, -1);
	KT_ASSERT_ER(dt_sysup_run(&pkg, FALSE), E_OK);
	KT_ASSERT(file_is(R_KERNEL, NEW_KERNEL));
	KT_ASSERT(file_is(R_KERNEL ".old", OLD_KERNEL));

	(void)wm_dev_conf_put("SYSUP", "");
	dt_sysup_test(NULL, -1);
	(void)ob_del_obj(&pkg);
	r_clean();
}

/*
 * The room the update asks of the ESP, at sizes 1/64 of a Surface Go 2's:
 * a 200 MB ESP of Windows with a 69 MB kernel in \EFI\TESSRON\ has room for
 * the new kernel once (the room asked before, the twice of every boot
 * file, it does not have), and with the .old of the update before left,
 * room for nothing but what deleting that .old frees.
 */
LOCAL void test_sysup_room( void )
{
	static char	man[512];
	SZ		k = (SZ)( ESP_KERNEL / ESP_SCALE );
	UD		free1 = ( ESP_BYTES - ESP_WINDOWS - ESP_KERNEL - ESP_RECORDS ) / ESP_SCALE;
	UD		free2 = free1 - ESP_KERNEL / ESP_SCALE;
	UD		need = 0, room = 0;
	UB		*kern;
	TS_UUID		pkg;

	kern = (UB *)Kmalloc(k + 1);
	KT_ASSERT(kern != NULL);
	if ( kern == NULL ) return;
	knl_memset(kern, 'n', k);
	kern[k] = 0;
	nent = 0;
	ent("boot/", "EFI/BOOT/BOOTX64.EFI", NEW_LOADER, OB_RT_SYSDATA);
	ent("boot/", "tessronos.elf", (CONST char *)kern, OB_RT_SYSDATA);
	ent("boot/", "init.elf", "init", OB_RT_SYSDATA);
	ent("boot/", "cmdline.txt", "video=default", OB_RT_SYSDATA);
	manifest_boot(man);
	KT_ASSERT_ER(pk_object(man, &pkg), E_OK);
	Kfree(kern);

	KT_ASSERT_ER(r_setup(0), E_OK);
	KT_ASSERT_ER(file_fill(R_KERNEL, 'k', k), E_OK);
	dt_sysup_test(R, -1);
	KT_ASSERT_ER(dt_sysup_need(&pkg, &need, &room), E_OK);
	tm_printf((UB *)"ktest sysup: ESP 200 MB / %d: %d KB free, %d KB needed (twice the files: %d KB)\n",
		  ESP_SCALE, (INT)( free1 / 1024 ), (INT)( need / 1024 ), (INT)( 2 * k / 1024 ));
	KT_ASSERT(need >= (UD)k);
	KT_ASSERT(need <= free1);
	KT_ASSERT(2 * (UD)k > free1);			/* what was asked before would not fit */

	KT_ASSERT_ER(file_fill(R_KERNEL ".old", 'o', k), E_OK);
	KT_ASSERT_ER(dt_sysup_need(&pkg, &need, &room), E_OK);
	tm_printf((UB *)"ktest sysup: with the .old left: %d KB free, %d KB needed\n",
		  (INT)( free2 / 1024 ), (INT)( need / 1024 ));
	KT_ASSERT(need <= free2);

	/* and it is put on so */
	KT_ASSERT_ER(dt_sysup_run(&pkg, FALSE), E_OK);
	{
		T_FSTAT	st;

		KT_ASSERT(fs_stat(R_KERNEL, &st) >= E_OK && st.size == (UD)k);
		KT_ASSERT(fs_stat(R_KERNEL ".old", &st) >= E_OK && st.size == (UD)k);
	}
	KT_ASSERT(file_is(R_FALLBACK, WIN_LOADER));

	dt_sysup_test(NULL, -1);
	(void)ob_del_obj(&pkg);
	r_clean();
}

EXPORT void ktest_sysup( void )
{
	KT_RUN(test_sysup);
	KT_RUN(test_sysup_installed);
	KT_RUN(test_sysup_cut);
	KT_RUN(test_sysup_room);
}
