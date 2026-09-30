/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_ob.c
 *	Real objects of every kind through the one set of basic operations
 *	(design 18): storage in memory and on a volume, channels, devices,
 *	processes, windows and their parts; the protection and what it
 *	decides; keys handed on with fewer operations; users and logging
 *	in; records carried between objects and notices of what happens.
 *
 *	The tests run in a kernel task, which acts as the system; what a
 *	user may do is tested by asking the decision directly with the
 *	credentials of that user.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/conf.h>
#include <ts/ob.h>
#include <ts/sysdef.h>
#include <ts/xf.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/proc.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/hid.h>
#include <ts/tadview.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"
#include "sysman/space.h"
#include <ts/om.h>
#include <ts/fn.h>
#include "../../application/desktop/desktop.h"

#define STORE	"/boot/OBTEST"

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;
	while ( s[n] != '\0' ) n++;
	return n;
}

LOCAL BOOL has( CONST UB *text, INT len, CONST char *want )
{
	INT	i, k, w = s_len(want);

	for ( i = 0; i + w <= len; i++ ) {
		for ( k = 0; k < w && text[i + k] == (UB)want[k]; k++ ) ;
		if ( k == w ) return TRUE;
	}
	return FALSE;
}

LOCAL void make_uuid( TS_UUID *u, UB seed )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) u->b[i] = (UB)( seed + i );
}

/* ---------------------------------------------------------------- hashing */

LOCAL void test_hash( void )
{
	/* FIPS 180-4 "abc"; RFC 7914 section 11 for PBKDF2-HMAC-SHA256 */
	CONST UB abc[OB_SHA256_LEN] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
		0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
	CONST UB pb1[OB_SHA256_LEN] = {
		0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c, 0x43, 0xe7, 0x22, 0x52,
		0x56, 0xc4, 0xf8, 0x37, 0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48,
		0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b };
	CONST UB pb2[OB_SHA256_LEN] = {
		0xae, 0x4d, 0x0c, 0x95, 0xaf, 0x6b, 0x46, 0xd3, 0x2d, 0x0a, 0xdf, 0xf9,
		0x28, 0xf0, 0x6d, 0xd0, 0x2a, 0x30, 0x3f, 0x8e, 0xf3, 0xc2, 0x51, 0xdf,
		0xd6, 0xe2, 0xd8, 0x5a, 0x95, 0x47, 0x4c, 0x43 };
	UB	out[OB_SHA256_LEN];
	INT	i, bad;

	knl_ob_sha256((CONST UB *)"abc", 3, out);
	for ( i = 0, bad = 0; i < OB_SHA256_LEN; i++ ) bad += ( out[i] != abc[i] );
	KT_ASSERT_EQ(bad, 0);

	knl_ob_pbkdf2((CONST UB *)"password", 8, (CONST UB *)"salt", 4, 1, out);
	for ( i = 0, bad = 0; i < OB_SHA256_LEN; i++ ) bad += ( out[i] != pb1[i] );
	KT_ASSERT_EQ(bad, 0);

	knl_ob_pbkdf2((CONST UB *)"password", 8, (CONST UB *)"salt", 4, 2, out);
	for ( i = 0, bad = 0; i < OB_SHA256_LEN; i++ ) bad += ( out[i] != pb2[i] );
	KT_ASSERT_EQ(bad, 0);
}

/* ---------------------------------------------------------------- JSON */

LOCAL void test_json( void )
{
	UB	j[256];
	INT	len, v;
	UB	s[32];
	CONST char *src = "{ \"name\" : \"a\\\"b\", \"n\": 12, \"o\": {\"k\": [1, {\"x\": \"y\"}]} }";

	len = s_len(src);
	knl_memcpy(j, src, len + 1);

	v = knl_oj_path(j, len, "name", NULL);
	KT_ASSERT_EQ(knl_oj_str(j, len, v, s, sizeof(s)), 3);
	KT_ASSERT_EQ(s[1], '"');
	v = knl_oj_path(j, len, "o", "k");
	KT_ASSERT(v >= 0 && j[v] == '[');

	/* a member added inside an object that is there, and one made on the way */
	len = knl_oj_set_path(j, len, sizeof(j), "o", "z", (CONST UB *)"true", 4);
	KT_ASSERT(len > 0);
	len = knl_oj_set_path(j, len, sizeof(j), "tessronos", "access", (CONST UB *)"{}", 2);
	KT_ASSERT(len > 0);
	KT_ASSERT(knl_oj_path(j, len, "o", "z") >= 0);
	KT_ASSERT(knl_oj_path(j, len, "tessronos", "access") >= 0);
	/* what was there is kept as it was */
	KT_ASSERT(has(j, len, "\"name\" : \"a\\\"b\""));
	/* a value replaced */
	len = knl_oj_set(j, len, sizeof(j), knl_oj_root(j, len), "n", (CONST UB *)"345", 3);
	{
		D	n = 0;

		KT_ASSERT(knl_oj_num(j, len, knl_oj_path(j, len, "n", NULL), &n));
		KT_ASSERT_EQ(n, 345);
	}
}

/* ---------------------------------------------------------------- protection */

LOCAL void test_permit( void )
{
	T_OBPRT	prt;
	T_OBCRD	own, mem, other, adm;
	TS_UUID	u_own, u_mem, u_other, g1, g2;
	UINT	ops;

	make_uuid(&u_own, 0x10);
	make_uuid(&u_mem, 0x20);
	make_uuid(&u_other, 0x30);
	make_uuid(&g1, 0x40);
	make_uuid(&g2, 0x50);

	knl_memset(&own, 0, sizeof(own));   own.user = u_own;
	knl_memset(&mem, 0, sizeof(mem));   mem.user = u_mem; mem.ngrp = 2;
	mem.grp[0] = g2;  mem.grp[1] = g1;	/* the object's group is not its first */
	knl_memset(&other, 0, sizeof(other)); other.user = u_other;
	knl_memset(&adm, 0, sizeof(adm));   adm.user = u_other; adm.ngrp = 1;
	adm.grp[0] = ob_group_admin;

	knl_memset(&prt, 0, sizeof(prt));
	prt.owner = u_own;
	prt.group = g1;
	prt.mode = 0750;			/* rwxr-x--- */

	ops = knl_ob_permit(&own, &prt, -1);
	KT_ASSERT_EQ(ops, OB_OP_ALL);		/* all, with changing the protection */
	ops = knl_ob_permit(&mem, &prt, -1);
	KT_ASSERT_EQ(ops, OB_OP_R | OB_OP_X);
	KT_ASSERT_EQ(knl_ob_permit(&other, &prt, -1), 0);
	KT_ASSERT_EQ(knl_ob_permit(&adm, &prt, -1), OB_OP_ALL);

	/* the list gives one user the attributes' writing, and nothing more */
	prt.nacl = 1;
	prt.acl[0].kind = OB_ACE_USER;
	prt.acl[0].who = u_other;
	prt.acl[0].ops = OB_OP_READ | OB_OP_ATRWR;
	KT_ASSERT_EQ(knl_ob_permit(&other, &prt, -1), OB_OP_READ | OB_OP_ATRWR);

	/* a group entry, matched by any group of the subject */
	prt.acl[0].kind = OB_ACE_GROUP;
	prt.acl[0].who = g2;
	prt.acl[0].ops = OB_OP_WRITE;
	KT_ASSERT_EQ(knl_ob_permit(&mem, &prt, -1), OB_OP_R | OB_OP_X | OB_OP_WRITE);

	/* a record's limit takes away on that record only */
	prt.nrmask = 1;
	prt.rmask[0].recno = 1;
	prt.rmask[0].ops = 0;
	ops = knl_ob_permit(&own, &prt, 1);
	KT_ASSERT_EQ(ops & ( OB_OP_READ | OB_OP_WRITE ), 0);
	KT_ASSERT(( ops & OB_OP_ATRRD ) != 0);	/* the attributes are not the record */
	KT_ASSERT(( knl_ob_permit(&own, &prt, 0) & OB_OP_READ ) != 0);

	/* the attributes bind administrators too */
	prt.attr = OB_A_RONLY | OB_A_PERM;
	ops = knl_ob_permit(&adm, &prt, -1);
	KT_ASSERT_EQ(ops & ( OB_OP_WRITE | OB_OP_RECORD | OB_OP_DELETE ), 0);
	KT_ASSERT(( ops & OB_OP_ATRWR ) != 0);

	/* what the TADjs Desktop writes: no owner, the flags it has */
	knl_ob_prt_legacy(&prt, TRUE, FALSE, TRUE);
	ops = knl_ob_permit(&other, &prt, -1);
	KT_ASSERT(( ops & OB_OP_READ ) != 0);
	KT_ASSERT_EQ(ops & OB_OP_WRITE, 0);
}

/* the protection goes into the metadata and comes back the same */
LOCAL void test_prt_text( void )
{
	T_OBPRT	a, b;
	UB	*j = (UB *)Kmalloc(OB_META_MAX);
	INT	len;
	CONST char *src = "{\"name\":\"x\",\"window\":{\"width\":10}}";

	if ( j == NULL ) KT_SKIP("no memory");

	knl_memset(&a, 0, sizeof(a));
	make_uuid(&a.owner, 1);
	make_uuid(&a.group, 2);
	a.mode = 0640;
	a.attr = OB_A_PERM;
	a.nacl = 1;
	a.acl[0].kind = OB_ACE_USER;
	make_uuid(&a.acl[0].who, 3);
	a.acl[0].ops = OB_OP_READ | OB_OP_ATRWR;
	a.nrmask = 1;
	a.rmask[0].recno = 2;
	a.rmask[0].ops = OB_OP_READ;

	len = s_len(src);
	knl_memcpy(j, src, len + 1);
	len = knl_ob_prt_store(j, len, OB_META_MAX, &a);
	KT_ASSERT(len > 0);
	KT_ASSERT(has(j, len, "\"mode\":\"rw-r-----\""));
	KT_ASSERT(has(j, len, "\"deletable\":false"));
	KT_ASSERT(has(j, len, "\"window\":{\"width\":10}"));	/* the rest untouched */
	KT_ASSERT_ER(knl_ob_prt_parse(j, len, &b), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&a.owner, &b.owner), 0);
	KT_ASSERT_EQ(ts_uuid_cmp(&a.group, &b.group), 0);
	KT_ASSERT_EQ(b.mode, 0640);
	KT_ASSERT_EQ(b.attr, OB_A_PERM);
	KT_ASSERT_EQ(b.nacl, 1);
	KT_ASSERT_EQ(b.acl[0].ops, OB_OP_READ | OB_OP_ATRWR);
	KT_ASSERT_EQ(b.nrmask, 1);
	KT_ASSERT_EQ(b.rmask[0].recno, 2);
	KT_ASSERT_EQ(b.rmask[0].ops, OB_OP_READ);
	Kfree(j);
}

/* ---------------------------------------------------------------- memory */

LOCAL void test_memory( void )
{
	T_OBCRE	c;
	T_OBREF	r;
	T_OBREC	rec[4];
	TS_UUID	u, found;
	UB	buf[32];
	SZ	asz = 0;
	INT	recno = -1, cnt = 0;
	ID	key;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.flags = OB_F_GLOBAL;
	c.name = (CONST UB *)"ktest.global";
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);

	/* found by its name; a second one of the name is refused */
	KT_ASSERT_ER(ob_fnd_nam((CONST UB *)"ktest.global", &found), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&u, &found), 0);
	KT_ASSERT_ER(ob_cre_obj(&c, &found), E_OBJ);

	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_STORAGE);
	KT_ASSERT_EQ(r.sub, OB_S_MEMORY);
	KT_ASSERT_EQ(r.flags, OB_F_VOLATILE | OB_F_GLOBAL);

	key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	KT_ASSERT_ER(ob_apd_rec(key, OB_RT_SYSDATA, 3, &recno), E_OK);
	KT_ASSERT_EQ(recno, 0);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 4, "shared", 6, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 10);			/* four zeroes, then what was written */
	KT_ASSERT_EQ(buf[0], 0);
	KT_ASSERT_EQ(buf[4], 's');
	KT_ASSERT_ER(ob_lst_rec(key, rec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);
	KT_ASSERT_EQ(rec[0].rt, OB_RT_SYSDATA);
	KT_ASSERT_EQ(rec[0].sub, 3);
	KT_ASSERT_ER(ob_trn_rec(key, 0, 2), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 2);
	KT_ASSERT_ER(ob_get_atr(key, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT(has(buf, (INT)asz, "ktest.global"));

	/* deleted while open: gone from sight, the key still closes */
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
	KT_ASSERT_ER(ob_fnd_nam((CONST UB *)"ktest.global", &found), E_NOEXS);
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);
}

/* ---------------------------------------------------------------- channels */

LOCAL void test_channel( void )
{
	T_OBCRE	c;
	TS_UUID	u, found;
	UB	buf[16];
	SZ	asz = 0;
	ID	key, nw;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.name = (CONST UB *)"ktest.chan";
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);
	KT_ASSERT_ER(ob_fnd_nam((CONST UB *)"ktest.chan", &found), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&u, &found), 0);

	key = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE);
	nw = ob_opn_obj(&u, OB_OP_READ | OB_O_NOWAIT);
	KT_ASSERT(key > 0 && nw > 0);
	if ( key <= 0 || nw <= 0 ) return;

	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, "one", 3, &asz), E_OK);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, "second", 6, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 3);
	KT_ASSERT_EQ(buf[0], 'o');
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 6);
	KT_ASSERT_EQ(buf[0], 's');
	/* empty: the key that does not wait says so */
	KT_ASSERT_ER(ob_rea_rec(nw, 0, 0, buf, sizeof(buf), &asz), E_TMOUT);
	/* a message too big for it */
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, buf, OB_CH_MSG_MAX + 1, &asz), E_PAR);

	KT_ASSERT_ER(ob_cls_obj(nw), E_OK);
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, (T_OBREF *)buf), E_NOEXS);
}

/* ---------------------------------------------------------------- keys */

LOCAL void test_keys( void )
{
	T_OBCRE	c;
	TS_UUID	u;
	UB	buf[8];
	SZ	asz = 0;
	INT	recno;
	ID	key, ro, theirs;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	KT_ASSERT_ER(ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno), E_OK);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, "abc", 3, &asz), E_OK);

	/* the manager's number rides in the key */
	KT_ASSERT(( key >> 24 ) > 0);

	/* a key that may only read */
	ro = ob_dup_key(key, OB_OP_READ, 0);
	KT_ASSERT(ro > 0);
	KT_ASSERT_ER(ob_rea_rec(ro, 0, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_ER(ob_wri_rec(ro, 0, 0, "x", 1, &asz), E_OACV);
	/* narrowing never widens */
	KT_ASSERT_ER(ob_wri_rec(ob_dup_key(ro, OB_OP_ALL, 0), 0, 0, "x", 1, &asz), E_OACV);

	/* a key handed to another process is not ours to use */
	theirs = ob_dup_key(key, OB_OP_READ, 99);
	KT_ASSERT(theirs > 0);
	KT_ASSERT_ER(ob_rea_rec(theirs, 0, 0, buf, sizeof(buf), &asz), E_OACV);
	KT_ASSERT_ER(ob_cls_obj(theirs), E_OACV);
	knl_ob_prc_end(99);			/* the process went: its keys close */

	/* closing one key leaves the others working */
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);
	KT_ASSERT_ER(ob_rea_rec(ro, 0, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(buf[0], 'a');
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, buf, sizeof(buf), &asz), E_ID);
	KT_ASSERT_ER(ob_cls_obj(ro), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}

/* ---------------------------------------------------------------- files */

LOCAL BOOL store_up = FALSE;

LOCAL void test_file( void )
{
	T_OBCRE	c;
	T_OBREF	r;
	T_OBPRT	p;
	T_OBREC	rec[4];
	TS_UUID	u;
	UB	*buf;
	SZ	asz = 0;
	INT	recno = -1, cnt = 0;
	ID	key;

	if ( ob_att_vol(STORE, 0) < E_OK ) KT_SKIP("no file system for the store");
	buf = (UB *)Kmalloc(OB_META_MAX);
	if ( buf == NULL ) KT_SKIP("no memory");
	store_up = TRUE;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.name = (CONST UB *)"ob file";
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);

	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_FILE);
	KT_ASSERT_EQ(r.name[0], 'o');

	/* made by the system: its owner, rw-r--r-- */
	KT_ASSERT_ER(ob_get_prt(&u, &p), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&p.owner, &ob_user_system), 0);
	KT_ASSERT_EQ(p.mode, 0644);

	key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	KT_ASSERT_ER(ob_apd_rec(key, OB_RT_TAD, 0, &recno), E_OK);
	KT_ASSERT_EQ(recno, 0);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, "<tad/>", 6, &asz), E_OK);
	KT_ASSERT_ER(ob_apd_rec(key, OB_RT_PROG, 355, &recno), E_OK);
	KT_ASSERT_EQ(recno, 1);
	KT_ASSERT_ER(ob_lst_rec(key, rec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);
	KT_ASSERT_EQ(rec[0].rt, OB_RT_TAD);
	KT_ASSERT_EQ(rec[0].size, 6);
	KT_ASSERT_EQ(rec[1].rt, OB_RT_PROG);
	KT_ASSERT_EQ(rec[1].sub, 355);
	KT_ASSERT_ER(ob_get_atr(key, buf, OB_META_MAX - 1, &asz), E_OK);
	KT_ASSERT(has(buf, (INT)asz, "\"records\""));
	KT_ASSERT(has(buf, (INT)asz, "\"access\""));

	/* a picture beside the record: written, listed, read back */
	KT_ASSERT_ER(ob_wri_res(key, (CONST UB *)"_0_0.png", "PNGDATA", 7), E_OK);
	KT_ASSERT_ER(ob_lst_res(key, buf, 256, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);
	KT_ASSERT(has(buf, 8, "_0_0.png"));
	KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)"_0_0.png", 3, buf, 16, &asz), E_OK);
	KT_ASSERT_EQ(asz, 4);
	KT_ASSERT_EQ(buf[0], 'D');
	/* a record, or the metadata, is not a resource */
	KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)"_0.xtad", 0, buf, 16, &asz), E_PAR);
	KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)".json", 0, buf, 16, &asz), E_PAR);
	KT_ASSERT_ER(ob_wri_res(key, (CONST UB *)"_1_0.wav", "x", 1), E_OK);

	/* attributes written without the right to the protection keep the protection */
	{
		CONST char *nw = "{\"name\":\"renamed\",\"tessronos\":{\"access\":{\"mode\":\"rwxrwxrwx\"}}}";
		ID	k2 = ob_dup_key(key, OB_OP_ATRWR | OB_OP_ATRRD, 0);

		KT_ASSERT_ER(ob_set_atr(k2, (CONST UB *)nw, s_len(nw)), E_OK);
		KT_ASSERT_ER(ob_get_prt(&u, &p), E_OK);
		KT_ASSERT_EQ(p.mode, 0644);
		KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
		KT_ASSERT_EQ(r.name[0], 'r');
		KT_ASSERT_ER(ob_cls_obj(k2), E_OK);
	}
	/* two records declared: taking the last away takes its declaration */
	KT_ASSERT_ER(ob_del_rec(key, 1), E_OK);
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);

	/* no writing: even the system cannot open it to write */
	p.attr = OB_A_RONLY;
	KT_ASSERT_ER(ob_set_prt(&u, &p), E_OK);
	KT_ASSERT_EQ(ob_opn_obj(&u, OB_OP_WRITE), E_OACV);
	key = ob_opn_obj(&u, OB_OP_READ);
	KT_ASSERT(key > 0);
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);

	/* counted in and out */
	KT_ASSERT_ER(ob_lnk_obj(&u), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.refcnt, 1);
	KT_ASSERT_ER(ob_del_obj(&u), E_OBJ);	/* still referred to */
	KT_ASSERT_ER(ob_unl_obj(&u), E_OK);

	/* no deleting */
	p.attr = OB_A_PERM;
	KT_ASSERT_ER(ob_set_prt(&u, &p), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OACV);
	p.attr = 0;
	KT_ASSERT_ER(ob_set_prt(&u, &p), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_NOEXS);

	/* deleting it took its resources with it */
	{
		T_FSTAT	st;
		char	us[TS_UUID_STRLEN + 1], path[96];
		INT	n = 0, k;
		CONST char *tail = "_1_0.wav";

		(void)ts_uuid_to_str(&u, us, sizeof(us));
		for ( k = 0; CNF_OB_STORE[k] != 0; k++ ) path[n++] = CNF_OB_STORE[k];
		path[n++] = '/';
		for ( k = 0; us[k] != 0; k++ ) path[n++] = us[k];
		for ( k = 0; tail[k] != 0; k++ ) path[n++] = tail[k];
		path[n] = 0;
		KT_ASSERT_ER(fs_stat(path, &st), EX_NOENT);
	}
	Kfree(buf);
}

/* ---------------------------------------------------------------- listing */

LOCAL void test_list( void )
{
	T_OBCRE	c;
	TS_UUID	a, b, buf[64];
	INT	cnt = 0, i, seen = 0;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	KT_ASSERT_ER(ob_cre_obj(&c, &a), E_OK);
	KT_ASSERT_ER(ob_cre_obj(&c, &b), E_OK);

	/* page by page: the disk may hold more than one page of objects */
	{
		TS_UUID	from;
		BOOL	first = TRUE;

		do {
			KT_ASSERT_ER(ob_lst_obj(OB_T_STORAGE, 0, first ? NULL : &from, buf, 64, &cnt),
				     E_OK);
			for ( i = 0; i < cnt; i++ ) {
				if ( i > 0 ) KT_ASSERT(ts_uuid_cmp(&buf[i - 1], &buf[i]) < 0);
				if ( !first && i == 0 ) KT_ASSERT(ts_uuid_cmp(&from, &buf[0]) < 0);
				if ( ts_uuid_cmp(&buf[i], &a) == 0 || ts_uuid_cmp(&buf[i], &b) == 0 ) seen++;
			}
			if ( cnt > 0 ) from = buf[cnt - 1];
			first = FALSE;
		} while ( cnt == 64 );
	}
	KT_ASSERT_EQ(seen, 2);

	/* continued after one: the other alone of the two follows */
	KT_ASSERT_ER(ob_lst_obj(OB_T_STORAGE, 0, &a, buf, 64, &cnt), E_OK);
	for ( i = 0, seen = 0; i < cnt; i++ ) {
		KT_ASSERT(ts_uuid_cmp(&buf[i], &a) > 0);
		if ( ts_uuid_cmp(&buf[i], &b) == 0 ) seen++;
	}
	KT_ASSERT_EQ(seen, 1);

	KT_ASSERT_ER(ob_del_obj(&a), E_OK);
	KT_ASSERT_ER(ob_del_obj(&b), E_OK);
}

/* ---------------------------------------------------------------- devices */

LOCAL void test_device( void )
{
	TS_UUID	buf[128], disk;
	T_OBREF	r;
	UB	*t;
	SZ	asz = 0;
	INT	cnt = 0, i;
	BOOL	clock = FALSE, found = FALSE;
	ID	key;

	KT_ASSERT_ER(ob_lst_obj(OB_T_DEVICE, 0, NULL, buf, 128, &cnt), E_OK);
	for ( i = 0; i < cnt; i++ ) {
		if ( ts_uuid_cmp(&buf[i], &ob_uuid_clock) == 0 ) clock = TRUE;
		if ( !found && ob_ref_obj(&buf[i], &r) >= E_OK && r.sub == OB_S_DISK ) {
			disk = buf[i];
			found = TRUE;
		}
	}
	KT_ASSERT(clock);
	tm_printf((UB*)"  %d devices\n", cnt);

	/* the clock: its time as ISO 8601 */
	t = (UB *)Kmalloc(512);
	KT_ASSERT(t != NULL);
	if ( t == NULL ) return;
	key = ob_opn_obj(&ob_uuid_clock, OB_OP_READ);
	KT_ASSERT(key > 0);
	KT_ASSERT_ER(ob_rea_rec(key, 1, 0, t, 64, &asz), E_OK);
	KT_ASSERT(asz >= 19);
	KT_ASSERT_EQ(t[0], '2');
	KT_ASSERT_EQ(t[10], 'T');
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);

	/* a disk: its description, and its first block */
	if ( found ) {
		key = ob_opn_obj(&disk, OB_OP_READ | OB_OP_ATRRD);
		KT_ASSERT(key > 0);
		KT_ASSERT_ER(ob_rea_rec(key, 0, 0, t, 511, &asz), E_OK);
		KT_ASSERT(has(t, (INT)asz, "disk"));
		KT_ASSERT_ER(ob_rea_rec(key, 1, 0, t, 512, &asz), E_OK);
		KT_ASSERT_EQ(asz, 512);
		KT_ASSERT_ER(ob_rea_rec(key, 1, 3, t, 512, &asz), E_PAR);	/* whole blocks */
		KT_ASSERT_ER(ob_cls_obj(key), E_OK);

		/* the same UUID when asked again */
		KT_ASSERT_ER(ob_ref_obj(&disk, &r), E_OK);
	}
	Kfree(t);

	/*
	 * The device box, an object on the first volume whichever kind of
	 * store that is, holds their names and UUIDs in its metadata.
	 */
	{
		char	us[TS_UUID_STRLEN + 1];
		UB	*m;
		SZ	ms = 0;

		key = ob_opn_obj(&ob_uuid_devbox, OB_OP_ATRRD);
		if ( key <= 0 ) {
			KT_SKIP("no volume for the device box");
		}
		m = (UB *)Kmalloc(OB_ATR_MAX);
		KT_ASSERT(m != NULL);
		if ( m != NULL ) {
			KT_ASSERT_ER(ob_get_atr(key, m, OB_ATR_MAX, &ms), E_OK);
			KT_ASSERT(has(m, (INT)ms, "\"devices\""));
			if ( found && ts_uuid_to_str(&disk, us, sizeof(us)) >= E_OK ) {
				KT_ASSERT(has(m, (INT)ms, us));
			}
			Kfree(m);
		}
		KT_ASSERT_ER(ob_cls_obj(key), E_OK);
	}
}

/* ---------------------------------------------------------------- processes */

LOCAL ID port_open( CONST char *name, TS_UUID *u );
LOCAL ER take( ID port, T_OBNTM *m );
LOCAL BOOL wait_notice( ID port, UINT event, CONST TS_UUID *u, INT ms );
LOCAL ID start_waiting( CONST TS_UUID *prog, TS_UUID *pu );
LOCAL void end_waiting( ID pid );

LOCAL void test_process( void )
{
	T_OBCRE	c;
	T_OBREF	r;
	T_PSTS	psts;
	TS_UUID	prog, pu;
	UB	*buf;
	SZ	asz;
	UW	arg = 77;
	INT	fd, n, total = 0, recno;
	ID	key, pid;
	CONST char *meta =
		"{\"name\":\"hello\",\"refCount\":0,"
		"\"tessronos\":{\"exec\":{\"record\":0,\"arch\":\"aarch64\"}}}";

	if ( !store_up ) KT_SKIP("no store");
	if ( fs_open("/boot/HELLO.ELF", O_RDONLY) < 0 ) KT_SKIP("no program");

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)meta;
	c.jsonsz = s_len(meta);
	KT_ASSERT_ER(ob_cre_obj(&c, &prog), E_OK);

	key = ob_opn_obj(&prog, OB_OP_R | OB_OP_W);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	KT_ASSERT_ER(ob_apd_rec(key, OB_RT_PROG, 0, &recno), E_OK);
	buf = (UB *)Kmalloc(4096);
	fd = fs_open("/boot/HELLO.ELF", O_RDONLY);
	while ( buf != NULL && fd >= 0 && ( n = fs_read(fd, buf, 4096) ) > 0 ) {
		KT_ASSERT_ER(ob_wri_rec(key, recno, total, buf, n, &asz), E_OK);
		total += n;
	}
	if ( fd >= 0 ) fs_close(fd);
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);

	/* making a process object is starting the program */
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &arg;
	c.argsz = sizeof(arg);
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);

	KT_ASSERT_ER(ob_ref_obj(&pu, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_PROCESS);

	/* its record 0 says what it is doing, and what program it runs */
	key = ob_opn_obj(&pu, OB_OP_READ | OB_OP_ATRRD);
	if ( key > 0 && buf != NULL ) {
		char	us[TS_UUID_STRLEN + 1];

		KT_ASSERT_ER(ob_rea_rec(key, 0, 0, buf, 4095, &asz), E_OK);
		(void)ts_uuid_to_str(&prog, us, sizeof(us));
		KT_ASSERT(has(buf, (INT)asz, "pid "));
		KT_ASSERT(has(buf, (INT)asz, us));
	}

	if ( key > 0 ) (void)ob_cls_obj(key);
	if ( pid > 0 ) {
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, (INT)arg);
	}
	KT_ASSERT_ER(ob_ref_obj(&pu, &r), E_NOEXS);	/* gone with the process */

	/* and its end is told to whoever asked: one that waits to be told
	   to end, so the request is surely made before it does */
	{
		TS_UUID	ch, pw;
		T_OBNTF	req;
		T_OBNTM	m;
		ID	port = port_open("ktest.exit", &ch);
		ID	wpid = start_waiting(&prog, &pw);
		ID	wkey = ( wpid > 0 ) ? ob_opn_obj(&pw, OB_OP_R) : 0;

		knl_memset(&req, 0, sizeof(req));
		req.events = OB_E_EXIT;
		KT_ASSERT(port > 0 && wkey > 0);
		KT_ASSERT(ob_ntf_evt(wkey, OB_REC_ANY, &req, port) > 0);
		end_waiting(wpid);
		/* the notice is posted as the process is cleared away, which may be
		   after its waiter has been let go */
		KT_ASSERT(wait_notice(port, OB_E_EXIT, &pw, 3000));
		(void)m;
		if ( wkey > 0 ) (void)ob_cls_obj(wkey);
		if ( port > 0 ) ob_cls_obj(port);
		(void)ob_del_obj(&ch);
	}

	if ( buf != NULL ) Kfree(buf);
	KT_ASSERT_ER(ob_del_obj(&prog), E_OK);
}

/* ---------------------------------------------------------------- users */

LOCAL void test_user( void )
{
	T_OBCRE	c;
	T_OBCRD	crd;
	T_OBPRT	p;
	TS_UUID	u, forged;
	UB	meta[256];
	INT	n = 0;
	char	gs[TS_UUID_STRLEN + 1];
	TS_UUID	staff;

	if ( !store_up ) KT_SKIP("no store");
	make_uuid(&staff, 0x60);
	(void)ts_uuid_to_str(&staff, gs, sizeof(gs));
	n = knl_oj_put(meta, 0, sizeof(meta),
		       "{\"name\":\"u1\",\"tessronos\":{\"user\":{\"name\":\"u1\",\"groups\":[\"");
	n = knl_oj_put(meta, n, sizeof(meta), gs);
	n = knl_oj_put(meta, n, sizeof(meta), "\"]}}}");

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = meta;
	c.jsonsz = n;
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);

	/* no password yet: nobody logs in */
	KT_ASSERT_ER(knl_ob_login_check(&u, (CONST UB *)"secret", &crd), E_OACV);
	KT_ASSERT_ER(ob_set_pwd(&u, (CONST UB *)"secret"), E_OK);
	KT_ASSERT_ER(knl_ob_login_check(&u, (CONST UB *)"secret", &crd), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&crd.user, &u), 0);
	KT_ASSERT_EQ(crd.ngrp, 1);
	KT_ASSERT_EQ(ts_uuid_cmp(&crd.grp[0], &staff), 0);
	KT_ASSERT_ER(knl_ob_login_check(&u, (CONST UB *)"Secret", &crd), E_OACV);
	/* the kernel is always the system */
	KT_ASSERT_ER(ob_login(&u, (CONST UB *)"secret"), E_OBJ);

	/* a user object someone else made does not count */
	knl_ob_prt_default(&p, NULL);
	make_uuid(&p.owner, 0x70);
	c.prt = &p;
	KT_ASSERT_ER(ob_cre_obj(&c, &forged), E_OK);
	KT_ASSERT_ER(ob_set_pwd(&forged, (CONST UB *)"x"), E_OK);
	KT_ASSERT_ER(knl_ob_login_check(&forged, (CONST UB *)"x", &crd), E_OACV);

	/* the hash is kept, the password is not */
	{
		ID	key = ob_opn_obj(&u, OB_OP_ATRRD);
		SZ	asz = 0;
		UB	*j = (UB *)Kmalloc(OB_META_MAX);

		if ( key > 0 && j != NULL ) {
			KT_ASSERT_ER(ob_get_atr(key, j, OB_META_MAX - 1, &asz), E_OK);
			KT_ASSERT(has(j, (INT)asz, "\"hash\""));
			KT_ASSERT(!has(j, (INT)asz, "secret"));
		}
		if ( key > 0 ) ob_cls_obj(key);
		if ( j != NULL ) Kfree(j);
	}
	KT_ASSERT_ER(ob_del_obj(&forged), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}


/* ---------------------------------------------------------------- records between objects */

LOCAL ER mem_obj( CONST char *name, TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.name = (CONST UB *)name;
	return ob_cre_obj(&c, u);
}

LOCAL void put_rec( ID key, UINT rt, UINT sub, CONST char *text )
{
	SZ	asz = 0;
	INT	recno = -1;

	KT_ASSERT_ER(ob_apd_rec(key, rt, sub, &recno), E_OK);
	KT_ASSERT_ER(ob_wri_rec(key, recno, 0, text, s_len(text), &asz), E_OK);
}

LOCAL void test_transfer( void )
{
	T_OBREC	rec[8];
	T_OBREF	r;
	TS_UUID	a, b, cp;
	UB	buf[32];
	SZ	asz = 0;
	INT	recno = -1, cnt = 0;
	ID	ka, kb, kc;

	KT_ASSERT_ER(mem_obj("ob from", &a), E_OK);
	KT_ASSERT_ER(mem_obj("ob to", &b), E_OK);
	ka = ob_opn_obj(&a, OB_OP_ALL);
	kb = ob_opn_obj(&b, OB_OP_ALL);
	KT_ASSERT(ka > 0 && kb > 0);
	if ( ka <= 0 || kb <= 0 ) return;

	/* a set-up record: text, a link, data of subtype 7, another link */
	put_rec(ka, OB_RT_TAD, 0, "<doc/>");
	put_rec(ka, OB_RT_LINK, 0, "L1");
	put_rec(ka, OB_RT_SYSDATA, 7, "seven");
	put_rec(ka, OB_RT_LINK, 0, "L2");

	/* the links one after another, a subtype through a mask */
	KT_ASSERT_ER(ob_sch_rec(ka, 0, OB_RT_LINK, 0, 0, &recno), E_OK);
	KT_ASSERT_EQ(recno, 1);
	KT_ASSERT_ER(ob_sch_rec(ka, recno + 1, OB_RT_LINK, 0, 0, &recno), E_OK);
	KT_ASSERT_EQ(recno, 3);
	KT_ASSERT_ER(ob_sch_rec(ka, recno + 1, OB_RT_LINK, 0, 0, &recno), E_NOEXS);
	KT_ASSERT_ER(ob_sch_rec(ka, 0, OB_RT_SYSDATA, 7, 0xF, &recno), E_OK);
	KT_ASSERT_EQ(recno, 2);
	KT_ASSERT_ER(ob_sch_rec(ka, 0, OB_RT_SYSDATA, 6, 0xF, &recno), E_NOEXS);

	/* two carried onto the end of an empty object, type and all */
	KT_ASSERT_ER(ob_trs_rec(kb, OB_REC_END, ka, 1, 2), E_OK);
	KT_ASSERT_ER(ob_lst_rec(kb, rec, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);
	KT_ASSERT_EQ(rec[0].rt, OB_RT_LINK);
	KT_ASSERT_EQ(rec[1].rt, OB_RT_SYSDATA);
	KT_ASSERT_EQ(rec[1].sub, 7);
	KT_ASSERT_EQ(rec[1].size, 5);
	KT_ASSERT_ER(ob_rea_rec(kb, 1, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 5);
	KT_ASSERT_EQ(buf[0], 's');

	/* one over a record that is there: its bytes replaced */
	KT_ASSERT_ER(ob_trs_rec(kb, 0, ka, 0, 1), E_OK);
	KT_ASSERT_ER(ob_rea_rec(kb, 0, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 6);
	KT_ASSERT_EQ(buf[1], 'd');
	KT_ASSERT_ER(ob_lst_rec(kb, rec, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);

	KT_ASSERT_ER(ob_trs_rec(kb, 5, ka, 0, 1), E_PAR);		/* a gap */
	KT_ASSERT_ER(ob_trs_rec(kb, OB_REC_END, ka, 9, 1), E_NOEXS);

	/* the whole object copied: every record, and the name */
	KT_ASSERT_ER(ob_cpy_obj(&a, NULL, &cp), E_OK);
	KT_ASSERT(ts_uuid_cmp(&a, &cp) != 0);
	KT_ASSERT_ER(ob_ref_obj(&cp, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_MEMORY);
	KT_ASSERT_EQ(r.nrec, 4);
	KT_ASSERT(has(r.name, s_len((CONST char *)r.name), "ob from"));
	kc = ob_opn_obj(&cp, OB_OP_R);
	KT_ASSERT(kc > 0);
	if ( kc > 0 ) {
		KT_ASSERT_ER(ob_lst_rec(kc, rec, 8, &cnt), E_OK);
		KT_ASSERT_EQ(cnt, 4);
		KT_ASSERT_EQ(rec[2].sub, 7);
		KT_ASSERT_ER(ob_rea_rec(kc, 3, 0, buf, sizeof(buf), &asz), E_OK);
		KT_ASSERT_EQ(asz, 2);
		KT_ASSERT_EQ(buf[1], '2');
		ob_cls_obj(kc);
	}

	ob_cls_obj(ka);
	ob_cls_obj(kb);
	KT_ASSERT_ER(ob_del_obj(&a), E_OK);
	KT_ASSERT_ER(ob_del_obj(&b), E_OK);
	KT_ASSERT_ER(ob_del_obj(&cp), E_OK);
}

/* ---------------------------------------------------------------- notices */

/* A port to take notices at: a channel, read without waiting */
LOCAL ID port_open( CONST char *name, TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.name = (CONST UB *)name;
	if ( ob_cre_obj(&c, u) < E_OK ) {
		return E_SYS;
	}
	return ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
}

LOCAL ER take( ID port, T_OBNTM *m )
{
	SZ	asz = 0;
	ER	er = ob_rea_rec(port, 0, 0, m, sizeof(*m), &asz);

	if ( er >= E_OK && asz != (SZ)sizeof(*m) ) {
		er = E_OBJ;
	}
	return er;
}

LOCAL void test_notice( void )
{
	T_OBNTF	req;
	T_OBNTM	m;
	T_OBREC	rec[4];
	TS_UUID	ch, obj, logu;
	SZ	asz = 0;
	INT	recno = -1, cnt = 0, i;
	ID	port, ko, kl, nid, once;

	port = port_open("ktest.port", &ch);
	KT_ASSERT(port > 0);
	KT_ASSERT_ER(mem_obj("watched", &obj), E_OK);
	ko = ob_opn_obj(&obj, OB_OP_ALL);
	KT_ASSERT(ko > 0);
	if ( port <= 0 || ko <= 0 ) return;

	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE | OB_E_DELETE;
	req.id = 77;
	KT_ASSERT_ER(ob_ntf_evt(ko, OB_REC_ANY, NULL, port), E_PAR);
	nid = ob_ntf_evt(ko, OB_REC_ANY, &req, port);
	KT_ASSERT(nid > 0);

	/* a record added, then written: each is told, with what and where */
	KT_ASSERT_ER(ob_apd_rec(ko, OB_RT_TAD, 0, &recno), E_OK);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.id, 77);
	KT_ASSERT_EQ(m.nid, nid);
	KT_ASSERT_EQ(m.event, OB_E_CHANGE);
	KT_ASSERT_EQ(m.recno, 0);
	KT_ASSERT_EQ(ts_uuid_cmp(&m.uuid, &obj), 0);
	KT_ASSERT_ER(ob_wri_rec(ko, 0, 0, "x", 1, &asz), E_OK);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.recno, 0);
	KT_ASSERT_ER(take(port, &m), E_TMOUT);		/* and nothing else */

	/* one record watched: the others are not told of */
	KT_ASSERT_ER(ob_can_evt(nid), E_OK);
	KT_ASSERT_ER(ob_can_evt(nid), E_ID);
	nid = ob_ntf_evt(ko, 1, &req, port);
	KT_ASSERT(nid > 0);
	KT_ASSERT_ER(ob_apd_rec(ko, OB_RT_SYSDATA, 0, &recno), E_OK);
	KT_ASSERT_ER(ob_wri_rec(ko, 0, 0, "y", 1, &asz), E_OK);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.recno, 1);
	KT_ASSERT_ER(take(port, &m), E_TMOUT);

	/* once: told, and the request is gone */
	req.flags = OB_N_ONCE;
	once = ob_ntf_evt(ko, 0, &req, port);
	KT_ASSERT(once > 0);
	KT_ASSERT_ER(ob_wri_rec(ko, 0, 0, "z", 1, &asz), E_OK);
	KT_ASSERT_ER(ob_wri_rec(ko, 0, 0, "z", 1, &asz), E_OK);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.nid, once);
	KT_ASSERT_ER(take(port, &m), E_TMOUT);
	KT_ASSERT_ER(ob_can_evt(once), E_ID);

	/* a port that is full: what did not go in is counted on the next */
	req.flags = 0;
	KT_ASSERT_ER(ob_can_evt(nid), E_OK);
	nid = ob_ntf_evt(ko, OB_REC_ANY, &req, port);
	for ( i = 0; i < OB_CH_QLEN + 3; i++ ) {
		(void)ob_wri_rec(ko, 0, 0, "f", 1, &asz);
	}
	for ( i = 0; i < OB_CH_QLEN; i++ ) {
		KT_ASSERT_ER(take(port, &m), E_OK);
		KT_ASSERT_EQ(m.lost, 0);
	}
	KT_ASSERT_ER(take(port, &m), E_TMOUT);
	KT_ASSERT_ER(ob_wri_rec(ko, 0, 0, "g", 1, &asz), E_OK);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.lost, 3);

	/* a port that is storage: each notice a record, a log to read back */
	KT_ASSERT_ER(mem_obj("ob log", &logu), E_OK);
	kl = ob_opn_obj(&logu, OB_OP_ALL);
	KT_ASSERT(kl > 0);
	if ( kl > 0 ) {
		KT_ASSERT_ER(ob_can_evt(nid), E_OK);
		nid = ob_ntf_evt(ko, OB_REC_ANY, &req, kl);
		KT_ASSERT(nid > 0);
		KT_ASSERT_ER(ob_wri_rec(ko, 0, 0, "h", 1, &asz), E_OK);
		KT_ASSERT_ER(ob_wri_rec(ko, 1, 0, "i", 1, &asz), E_OK);
		KT_ASSERT_ER(ob_lst_rec(kl, rec, 4, &cnt), E_OK);
		KT_ASSERT_EQ(cnt, 2);
		KT_ASSERT_EQ(rec[1].rt, OB_RT_SYSDATA);
		KT_ASSERT_ER(ob_rea_rec(kl, 1, 0, &m, sizeof(m), &asz), E_OK);
		KT_ASSERT_EQ(m.recno, 1);
		KT_ASSERT_ER(ob_can_evt(nid), E_OK);
		ob_cls_obj(kl);
		(void)ob_del_obj(&logu);
	}

	/* a request goes with the key it was made through */
	{
		ID	k2 = ob_opn_obj(&obj, OB_OP_R);

		KT_ASSERT(ob_ntf_evt(k2, OB_REC_ANY, &req, port) > 0);
		ob_cls_obj(k2);
		KT_ASSERT_ER(ob_wri_rec(ko, 0, 0, "j", 1, &asz), E_OK);
		KT_ASSERT_ER(take(port, &m), E_TMOUT);
	}

	/* and the object going is told */
	nid = ob_ntf_evt(ko, OB_REC_ANY, &req, port);
	KT_ASSERT(nid > 0);
	KT_ASSERT_ER(ob_del_obj(&obj), E_OK);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.event, OB_E_DELETE);
	KT_ASSERT_EQ(m.recno, -1);

	ob_cls_obj(ko);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
}

/* ---------------------------------------------------------------- windows */

/* The first window object of a subtype, other than 'not' */
LOCAL BOOL find_window( UINT sub, CONST TS_UUID *not, TS_UUID *out )
{
	TS_UUID	*list = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	T_OBREF	r;
	INT	cnt = 0, i;
	BOOL	found = FALSE;

	if ( list == NULL ) {
		return FALSE;
	}
	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 256, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && !found; i++ ) {
			if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == sub
			  && ( not == NULL || ts_uuid_cmp(&list[i], not) != 0 ) ) {
				*out = list[i];
				found = TRUE;
			}
		}
	}
	Kfree(list);
	return found;
}

/* The window's number, as its attributes say it */
LOCAL INT number_of( ID key )
{
	UB	j[256];
	SZ	asz = 0;
	D	v = 0;
	INT	p;

	if ( ob_get_atr(key, j, sizeof(j) - 1, &asz) < E_OK ) {
		return -1;
	}
	p = knl_oj_path(j, (INT)asz, "tessronos", "window");
	p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "number") : -1;
	return ( p >= 0 && knl_oj_num(j, (INT)asz, p, &v) ) ? (INT)v : -1;
}

LOCAL CONST char ob_win_json[] = "{\"rect\":[40,50,360,290]}";
LOCAL CONST char ob_win_draw[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>window</p></document></tad>";

LOCAL void test_window( void )
{
	T_OBCRE		c;
	T_OBREF		r;
	T_OBPRT		p;
	T_OBWPOS	wp;
	T_OBNTF		req;
	T_OBNTM		m;
	T_OBREC		rec[8];
	T_WMEV		ev;
	TS_UUID		w, ch, pnl, part1, menu;
	T_WMPANEL	*def;
	UB		buf[128];
	SZ		asz = 0;
	INT		cnt = 0, wid, pid, v = 0;
	ID		kw, port, kp, kpart, kmenu;
	ER		er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"ob window";
	c.json = (CONST UB *)ob_win_json;
	c.jsonsz = sizeof(ob_win_json) - 1;
	er = ob_cre_obj(&c, &w);
	if ( er == E_NOSPT || er == E_NOEXS ) KT_SKIP("no window manager");
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) return;

	KT_ASSERT_ER(ob_ref_obj(&w, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_WINDOW);
	KT_ASSERT_EQ(r.sub, OB_S_WINDOW);
	KT_ASSERT_EQ(r.nrec, OB_WR_KIND + 2);		/* and the drop record, the bars */
	KT_ASSERT(( r.flags & OB_F_VOLATILE ) != 0);
	KT_ASSERT(has(r.name, s_len((CONST char *)r.name), "ob window"));
	KT_ASSERT_ER(ob_get_prt(&w, &p), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&p.owner, &ob_user_system), 0);
	KT_ASSERT_EQ(p.mode, 0644);			/* the creator's default was given */

	kw = ob_opn_obj(&w, OB_OP_ALL);
	KT_ASSERT(kw > 0);
	if ( kw <= 0 ) return;
	wid = number_of(kw);
	KT_ASSERT(wid > 0);

	/* where it is, and moved by writing where it is to be */
	KT_ASSERT_ER(ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(wp));
	KT_ASSERT_EQ(wp.left, 40);
	KT_ASSERT_EQ(wp.bottom, 290);
	KT_ASSERT(( wp.flags & OB_WP_SHOWN ) != 0);
	KT_ASSERT_EQ(wp.z, 0);
	wp.left += 20;
	wp.right += 20;
	KT_ASSERT_ER(ob_wri_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT_EQ(wp.left, 60);
	KT_ASSERT_EQ(wp.right, 380);

	/* drawn by writing xmlTAD to its drawing record, which keeps it */
	wm_obj_painter(tv_paint, tv_shape);
	KT_ASSERT_ER(ob_wri_rec(kw, OB_WR_DRAW, 0, ob_win_draw, sizeof(ob_win_draw) - 1,
				&asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(kw, OB_WR_DRAW, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(ob_win_draw) - 1);
	KT_ASSERT(has(buf, (INT)asz, "<p>window</p>"));
	KT_ASSERT_ER(ob_lst_rec(kw, rec, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, OB_WR_KIND + 2);
	KT_ASSERT_EQ(rec[OB_WR_BARS].size, sizeof(T_OBWBARS));
	KT_ASSERT_EQ(rec[OB_WR_DROP].size, 0);		/* nothing dropped yet */
	KT_ASSERT_EQ(rec[OB_WR_DRAW].rt, OB_RT_TAD);
	KT_ASSERT_EQ(rec[OB_WR_DRAW].size, sizeof(ob_win_draw) - 1);
	KT_ASSERT_ER(ob_trn_rec(kw, OB_WR_DRAW, 0), E_OK);
	KT_ASSERT_ER(ob_rea_rec(kw, OB_WR_DRAW, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);

	/* what happens in it, told at a port */
	port = port_open("ktest.winport", &ch);
	KT_ASSERT(port > 0);
	if ( port <= 0 ) return;
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_PRESS | OB_E_CHANGE | OB_E_DELETE;
	req.id = 5;
	KT_ASSERT(ob_ntf_evt(kw, OB_REC_ANY, &req, port) > 0);
	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_BTN_DOWN;
	ev.wid = wid;
	ev.x = 5;
	ev.y = 6;
	knl_wmobj_event(&ev);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.event, OB_E_PRESS);
	KT_ASSERT_EQ(m.x, 5);
	KT_ASSERT_EQ(m.y, 6);
	KT_ASSERT_EQ(ts_uuid_cmp(&m.uuid, &w), 0);
	/* one press, one notice */
	KT_ASSERT_ER(take(port, &m), E_TMOUT);

	/* links dropped on it: refused while nobody asked for drops */
	{
		T_OBDROPV	dv[2];
		T_OBDROP	*dr = (T_OBDROP *)Kmalloc(sizeof(T_OBDROP));
		T_OBDRANS	an;
		T_OBNTF		rq;
		T_WMWIN		ww;

		KT_ASSERT(dr != NULL && wm_ref(wid, &ww) >= E_OK);
		knl_memset(dv, 0, sizeof(dv));
		dv[0].target = w;			/* any objects will do: themselves */
		dv[1].target = ch;
		dv[0].name[0] = 'a';
		KT_ASSERT_ER(wm_obj_drop(wid, ww.work.left + 7, ww.work.top + 9, 0, NULL, dv, 2), E_NOSPT);
		knl_memset(&rq, 0, sizeof(rq));
		rq.events = OB_E_DROP;
		rq.id = 6;
		KT_ASSERT(ob_ntf_evt(kw, OB_REC_ANY, &rq, port) > 0);
		KT_ASSERT_ER(wm_obj_drop(wid, ww.work.left + 7, ww.work.top + 9, 0, NULL, dv, 2), E_OK);
		KT_ASSERT_ER(take(port, &m), E_OK);
		KT_ASSERT_EQ(m.event, OB_E_DROP);
		KT_ASSERT_EQ(m.id, 6);
		KT_ASSERT_EQ(m.recno, OB_WR_DROP);
		KT_ASSERT_EQ(m.code, 2);
		KT_ASSERT_EQ(m.x, 7);
		KT_ASSERT_EQ(m.y, 9);
		if ( dr != NULL ) {
			/* the record: the drop, the kernel's grants none (it needs none) */
			KT_ASSERT_ER(ob_rea_rec(kw, OB_WR_DROP, 0, dr, sizeof(*dr), &asz), E_OK);
			KT_ASSERT_EQ(asz, sizeof(*dr) - sizeof(T_OBDROPV) * ( OB_DROP_MAX - 2 ));
			KT_ASSERT_EQ(dr->n, 2);
			KT_ASSERT_EQ(dr->x, 7);
			KT_ASSERT_EQ(ts_uuid_cmp(&dr->v[1].target, &ch), 0);
			KT_ASSERT_EQ(dr->v[0].name[0], 'a');
			KT_ASSERT_EQ(dr->v[0].ops, OB_DROP_OPS);	/* the system may do it all */
			/* answered once, and only the drop there is */
			knl_memset(&an, 0, sizeof(an));
			an.seq = dr->seq + 1;
			an.answer = OB_DR_ACCEPT;
			KT_ASSERT_ER(ob_wri_rec(kw, OB_WR_DROP, 0, &an, sizeof(an), &asz), E_OBJ);
			an.seq = dr->seq;
			KT_ASSERT_ER(ob_wri_rec(kw, OB_WR_DROP, 0, &an, sizeof(an), &asz), E_OK);
			KT_ASSERT_ER(ob_wri_rec(kw, OB_WR_DROP, 0, &an, sizeof(an), &asz), E_OBJ);
			Kfree(dr);
		}
		/* a window is on no volume; the first volume is there to be asked of */
		{
			T_OBVOL	vv;
			TS_UUID	none;

			knl_memset(&none, 0x5A, sizeof(none));
			KT_ASSERT_ER(ob_ref_vol(&w, &vv), E_NOSPT);
			KT_ASSERT_ER(ob_ref_vol(&none, &vv), E_NOEXS);
			KT_ASSERT_ER(ob_ref_vol(NULL, &vv), E_OK);
			KT_ASSERT(vv.bsize > 0 && vv.bfree <= vv.blocks);
		}
		/* the answers are writes: told as changes, and nothing else */
		while ( take(port, &m) >= E_OK ) {
			KT_ASSERT_EQ(m.event, OB_E_CHANGE);
		}
	}

	/* its scroll bars: what its owner writes, and the user's working told */
	{
		T_OBWBARS	bs;
		T_WMBAR		wb;
		T_OBNTF		rq;

		knl_memset(&bs, 0, sizeof(bs));
		bs.bar[OB_BAR_R].hi = 3000;
		bs.bar[OB_BAR_R].clo = 600;
		bs.bar[OB_BAR_R].chi = 840;
		bs.bar[OB_BAR_B].hi = 900;
		bs.bar[OB_BAR_B].chi = 320;
		KT_ASSERT_ER(ob_wri_rec(kw, OB_WR_BARS, 0, &bs, 3, &asz), E_PAR);
		KT_ASSERT_ER(ob_wri_rec(kw, OB_WR_BARS, 0, &bs, 2 * sizeof(T_OBWBAR), &asz), E_OK);
		KT_ASSERT_ER(wm_bar(wid, WM_BAR_R, &wb), E_OK);
		KT_ASSERT_EQ(wb.clo, 600);
		KT_ASSERT_EQ(wb.chi, 840);
		knl_memset(&bs, 0, sizeof(bs));
		KT_ASSERT_ER(ob_rea_rec(kw, OB_WR_BARS, 0, &bs, sizeof(bs), &asz), E_OK);
		KT_ASSERT_EQ(asz, sizeof(bs));
		KT_ASSERT_EQ(bs.bar[OB_BAR_R].hi, 3000);
		KT_ASSERT_EQ(bs.bar[OB_BAR_B].chi, 320);
		KT_ASSERT_EQ(bs.bar[OB_BAR_L].hi, 0);
		KT_ASSERT_ER(ob_trn_rec(kw, OB_WR_BARS, 0), E_OK);
		while ( take(port, &m) >= E_OK ) {
			KT_ASSERT_EQ(m.event, OB_E_CHANGE);
		}
		/* worked by the user: told only to one who asked */
		wm_obj_scroll(wid, WM_BAR_R, 840, OB_SCR_PAGE);
		KT_ASSERT_ER(take(port, &m), E_TMOUT);
		knl_memset(&rq, 0, sizeof(rq));
		rq.events = OB_E_SCROLL;
		rq.id = 7;
		KT_ASSERT(ob_ntf_evt(kw, OB_REC_ANY, &rq, port) > 0);
		wm_obj_scroll(wid, WM_BAR_R, 840, OB_SCR_PAGE);
		KT_ASSERT_ER(take(port, &m), E_OK);
		KT_ASSERT_EQ(m.event, OB_E_SCROLL);
		KT_ASSERT_EQ(m.id, 7);
		KT_ASSERT_EQ(m.recno, OB_WR_BARS);
		KT_ASSERT_EQ(m.code, OB_BAR_R);
		KT_ASSERT_EQ(m.x, 840);
		KT_ASSERT_EQ(m.y, OB_SCR_PAGE);
		wm_obj_scroll(wid, WM_BAR_B, 100, OB_SCR_DONE);
		KT_ASSERT_ER(take(port, &m), E_OK);
		KT_ASSERT_EQ(m.code, OB_BAR_B);
		KT_ASSERT_EQ(m.y, OB_SCR_DONE);
		KT_ASSERT_ER(take(port, &m), E_TMOUT);
	}

	/* a panel in it: an object, and each of its parts one */
	def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(def != NULL);
	if ( def == NULL ) return;
	knl_memset(def, 0, sizeof(*def));
	def->num = 1;
	def->r.left = 10;  def->r.top = 10;  def->r.right = 200;  def->r.bottom = 100;
	def->npart = 2;
	def->part[0].type = WM_PT_BUTTON;
	def->part[0].num = 1;
	def->part[0].r.left = 5;  def->part[0].r.top = 5;
	def->part[0].r.right = 80;  def->part[0].r.bottom = 25;
	def->part[0].label[0] = 'O';  def->part[0].label[1] = 'K';
	def->part[1].type = WM_PT_CHECK;
	def->part[1].num = 2;
	def->part[1].r.left = 5;  def->part[1].r.top = 30;
	def->part[1].r.right = 80;  def->part[1].r.bottom = 50;
	def->part[1].label[0] = 'c';  def->part[1].label[1] = 'k';
	pid = wm_panel_open(wid, def);
	Kfree(def);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	KT_ASSERT(find_window(OB_S_PANEL, NULL, &pnl));
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_OK);
	KT_ASSERT_EQ(r.nrec, OB_WR_KIND + 2);
	kp = ob_opn_obj(&pnl, OB_OP_R);
	KT_ASSERT(kp > 0);
	KT_ASSERT_ER(ob_rea_rec(kp, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&wp.parent, &w), 0);	/* it is in the window */
	KT_ASSERT_EQ(wp.left, 10);
	KT_ASSERT_ER(ob_lst_rec(kp, rec, 8, &cnt), E_OK);
	KT_ASSERT_EQ(rec[OB_WR_KIND].rt, OB_RT_LINK);
	KT_ASSERT_ER(ob_rea_rec(kp, OB_WR_KIND + 1, 0, &part1, sizeof(part1), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(TS_UUID));
	ob_cls_obj(kp);

	KT_ASSERT_ER(ob_ref_obj(&part1, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_PART);
	KT_ASSERT_EQ(r.name[0], 'c');
	kpart = ob_opn_obj(&part1, OB_OP_ALL);
	KT_ASSERT(kpart > 0);
	if ( kpart > 0 ) {
		/* its state is the part's value, both ways */
		v = 1;
		KT_ASSERT_ER(ob_wri_rec(kpart, OB_WR_STATE, 0, &v, sizeof(v), &asz), E_OK);
		v = 0;
		KT_ASSERT_ER(wm_panel_get(pid, 2, &v), E_OK);
		KT_ASSERT_EQ(v, 1);
		KT_ASSERT_ER(wm_panel_set(pid, 2, 0), E_OK);
		KT_ASSERT_ER(ob_rea_rec(kpart, OB_WR_STATE, 0, &v, sizeof(v), &asz), E_OK);
		KT_ASSERT_EQ(v, 0);

		/* pressed on the screen: the part is told, not the panel */
		KT_ASSERT(ob_ntf_evt(kpart, OB_REC_ANY, &req, port) > 0);
		knl_memset(&ev, 0, sizeof(ev));
		ev.type = HID_EV_BTN_DOWN;
		ev.wid = wid;
		ev.x = 10 + 20;
		ev.y = 10 + 40;
		KT_ASSERT_ER(wm_panel_event(pid, &ev, NULL), E_OK);
		KT_ASSERT_ER(take(port, &m), E_OK);
		KT_ASSERT_EQ(m.event, OB_E_PRESS);
		KT_ASSERT_EQ(ts_uuid_cmp(&m.uuid, &part1), 0);
		while ( take(port, &m) >= E_OK ) ;	/* and its value, if it turned */
		ob_cls_obj(kpart);
	}
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_NOEXS);
	KT_ASSERT_ER(ob_ref_obj(&part1, &r), E_NOEXS);

	/* a menu: an item a record, its state in the record's subtype */
	{
		T_WMMENU	*md = (T_WMMENU *)Kmalloc(sizeof(T_WMMENU));

		if ( md != NULL ) {
			knl_memset(md, 0, sizeof(*md));
			md->num = 1;
			md->nitem = 3;
			md->item[0].cmd = 1;
			md->item[0].label[0] = 'A';
			md->item[2].cmd = 2;
			md->item[2].label[0] = 'B';
			md->item[2].grey = TRUE;
			md->item[2].tick = TRUE;
			pid = wm_menu_open(wid, md, 20, 20);
			Kfree(md);
			KT_ASSERT(pid > 0);
			if ( pid > 0 && find_window(OB_S_MENU, NULL, &menu) ) {
				kmenu = ob_opn_obj(&menu, OB_OP_R);
				KT_ASSERT(kmenu > 0);
				KT_ASSERT_ER(ob_lst_rec(kmenu, rec, 8, &cnt), E_OK);
				KT_ASSERT_EQ(cnt, OB_WR_KIND + 3);
				KT_ASSERT_EQ(rec[OB_WR_KIND].rt, OB_RT_TAD);
				KT_ASSERT(( rec[OB_WR_KIND + 1].sub & OB_MI_LINE ) != 0);
				KT_ASSERT_EQ(rec[OB_WR_KIND + 2].sub & ( OB_MI_GREY | OB_MI_TICK ),
					     OB_MI_GREY | OB_MI_TICK);
				KT_ASSERT_ER(ob_rea_rec(kmenu, OB_WR_KIND + 2, 0, buf, sizeof(buf),
							&asz), E_OK);
				KT_ASSERT_EQ(asz, 1);
				KT_ASSERT_EQ(buf[0], 'B');
				ob_cls_obj(kmenu);
			} else {
				KT_ASSERT(FALSE);
			}
			if ( pid > 0 ) wm_panel_close(pid);
		}
	}

	/* closed as an object: told once, and gone */
	KT_ASSERT_ER(ob_del_obj(&w), E_OK);
	KT_ASSERT_ER(take(port, &m), E_OK);
	KT_ASSERT_EQ(m.event, OB_E_DELETE);
	KT_ASSERT_EQ(ts_uuid_cmp(&m.uuid, &w), 0);
	KT_ASSERT_ER(take(port, &m), E_TMOUT);
	KT_ASSERT_ER(ob_ref_obj(&w, &r), E_NOEXS);
	ob_cls_obj(kw);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	wm_update();
}

/* A window made as an object, where it is asked to be and with that frame */
LOCAL ER win_obj( CONST char *json, TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"ob events";
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	return ob_cre_obj(&c, u);
}

/* The next notice of one kind for one object, the others passed over */
LOCAL BOOL take_one( ID port, UINT event, CONST TS_UUID *u )
{
	T_OBNTM	m;

	while ( take(port, &m) >= E_OK ) {
		if ( m.event == event && ts_uuid_cmp(&m.uuid, u) == 0 ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL CONST char ob_shape_fig[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
	"<ellipse l_pat=\"1\" f_pat=\"4\" frameLeft=\"0\" frameTop=\"0\" frameRight=\"100\" frameBottom=\"100\"/>"
	"</figure></tad>";

LOCAL void test_window_events( void )
{
	T_OBNTF		req;
	T_OBWPOS	wp;
	T_WMWIN		w;
	T_WMEV		ev;
	TS_UUID		a, b, ch;
	SZ		asz = 0;
	INT		wa, wb, sx, sy, px = -1, py = -1, got = 0;
	UINT		bar = 0;
	ID		ka, kb, port;
	ER		er;

	er = win_obj("{\"rect\":[40,50,360,290],\"attr\":3}", &a);
	if ( er == E_NOSPT || er == E_NOEXS ) KT_SKIP("no window manager");
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_ER(win_obj("{\"rect\":[400,50,600,250]}", &b), E_OK);
	ka = ob_opn_obj(&a, OB_OP_ALL);
	kb = ob_opn_obj(&b, OB_OP_ALL);
	port = port_open("ktest.evport", &ch);
	KT_ASSERT(ka > 0 && kb > 0 && port > 0);
	if ( ka <= 0 || kb <= 0 || port <= 0 ) return;
	wm_obj_painter(tv_paint, tv_shape);
	wa = number_of(ka);
	wb = number_of(kb);

	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_ENTER | OB_E_LEAVE | OB_E_CLOSE | OB_E_REDRAW;
	KT_ASSERT(ob_ntf_evt(ka, OB_REC_ANY, &req, port) > 0);
	KT_ASSERT(ob_ntf_evt(kb, OB_REC_ANY, &req, port) > 0);

	/* the pointer over one window and then the other */
	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_MOVE;
	ev.wid = wa;
	knl_wmobj_event(&ev);
	KT_ASSERT(take_one(port, OB_E_ENTER, &a));
	ev.wid = wb;
	knl_wmobj_event(&ev);
	KT_ASSERT(take_one(port, OB_E_LEAVE, &a));
	KT_ASSERT(take_one(port, OB_E_ENTER, &b));

	/* a press on the pictogram in the band asks it to close */
	KT_ASSERT_ER(wm_ref(wa, &w), E_OK);
	for ( sy = w.outer.top; sy < w.work.top && px < 0; sy++ ) {
		for ( sx = w.outer.left; sx < w.outer.left + 60 && px < 0; sx++ ) {
			if ( wm_part_at(sx, sy, &got, &bar) == WM_PART_PICT && got == wa ) {
				px = sx;
				py = sy;
			}
		}
	}
	KT_ASSERT(px >= 0);
	ev.type = HID_EV_BTN_DOWN;
	ev.wid = wa;
	ev.x = px - w.work.left;
	ev.y = py - w.work.top;
	ev.when = 1000000000ULL;
	knl_wmobj_event(&ev);
	KT_ASSERT(!take_one(port, OB_E_CLOSE, &a));	/* one press alone does not close */
	ev.when += 100000000ULL;			/* the second of a double press does */
	knl_wmobj_event(&ev);
	KT_ASSERT(take_one(port, OB_E_CLOSE, &a));
	/* and the band beside it does not */
	ev.x = px + 60 - w.work.left;
	knl_wmobj_event(&ev);
	KT_ASSERT(!take_one(port, OB_E_CLOSE, &a));

	/* made larger: one its program draws is told; one drawn from its
	   drawing record is drawn again by the manager, and not told */
	KT_ASSERT_ER(ob_rea_rec(kb, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	wp.bottom += 20;
	KT_ASSERT_ER(ob_wri_rec(kb, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT(take_one(port, OB_E_REDRAW, &b));
	wp.bottom -= 20;
	KT_ASSERT_ER(ob_wri_rec(kb, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	(void)take_one(port, OB_E_REDRAW, &b);
	KT_ASSERT_ER(ob_wri_rec(ka, OB_WR_DRAW, 0, ob_win_draw, sizeof(ob_win_draw) - 1,
				&asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(ka, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	wp.right += 30;
	KT_ASSERT_ER(ob_wri_rec(ka, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT(!take_one(port, OB_E_REDRAW, &a));

	/* an outline from the shape record: a press outside it goes past */
	KT_ASSERT_ER(wm_ref(wb, &w), E_OK);
	KT_ASSERT_EQ(wm_at(w.outer.left + 190, w.outer.top + 190), wb);
	KT_ASSERT_ER(ob_wri_rec(kb, OB_WR_SHAPE, 0, ob_shape_fig, sizeof(ob_shape_fig) - 1,
				&asz), E_OK);
	KT_ASSERT_EQ(wm_at(w.outer.left + 50, w.outer.top + 50), wb);
	KT_ASSERT(wm_at(w.outer.left + 190, w.outer.top + 190) != wb);
	KT_ASSERT(wm_at(w.outer.left + 2, w.outer.top + 2) != wb);	/* the corner of the round */
	KT_ASSERT_ER(ob_trn_rec(kb, OB_WR_SHAPE, 0), E_OK);
	KT_ASSERT_EQ(wm_at(w.outer.left + 190, w.outer.top + 190), wb);

	ob_cls_obj(ka);
	ob_cls_obj(kb);
	KT_ASSERT_ER(ob_del_obj(&a), E_OK);
	KT_ASSERT_ER(ob_del_obj(&b), E_OK);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	wm_update();
}

/* ---------------------------------------------------------------- parts made on their own */

LOCAL ER free_obj( UINT sub, CONST char *name, CONST char *json, TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = sub;
	c.name = (CONST UB *)name;
	if ( json != NULL ) {
		c.json = (CONST UB *)json;
		c.jsonsz = s_len(json);
	}
	return ob_cre_obj(&c, u);
}

LOCAL BOOL placed( ID key )
{
	UB	j[256];
	SZ	asz = 0;

	return ( ob_get_atr(key, j, sizeof(j) - 1, &asz) >= E_OK
	      && has(j, (INT)asz, "\"placed\":true") );
}

LOCAL CONST char ob_def_json[] =
	"{\"name\":\"push\",\"tessronos\":{\"part\":{\"kind\":5,\"rect\":[0,0,70,22]}}}";
LOCAL CONST char ob_look_off[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
	"<rect l_pat=\"1\" f_pat=\"4\" left=\"0\" top=\"0\" right=\"70\" bottom=\"22\"/>"
	"</figure></tad>";

LOCAL void test_parts( void )
{
	T_OBCRE		cc;
	T_OBREF		r;
	T_OBMAP		m;
	T_OBWPOS	wp;
	T_OBNTF		req;
	T_OBREC		rec[8];
	T_WMEV		ev;
	T_WMPART	pt;
	TS_UUID		w, pnl, def, part, menu, got, ch;
	UB		buf[64];
	SZ		asz = 0;
	INT		wid, v, recno = -1, cnt = 0, mp, pop;
	UW		px = 0;
	ID		kw, kp, kd, kpart, km, port;
	ER		er;

	er = win_obj("{\"rect\":[40,40,440,340]}", &w);
	if ( er == E_NOSPT || er == E_NOEXS ) KT_SKIP("no window manager");
	KT_ASSERT_ER(er, E_OK);
	kw = ob_opn_obj(&w, OB_OP_ALL);
	port = port_open("ktest.partport", &ch);
	KT_ASSERT(kw > 0 && port > 0);
	if ( kw <= 0 || port <= 0 ) return;
	wid = number_of(kw);
	wm_obj_painter(tv_paint, tv_shape);
	knl_memset(&m, 0, sizeof(m));
	knl_memset(&req, 0, sizeof(req));

	/* a panel made on its own, then placed on the window */
	KT_ASSERT_ER(free_obj(OB_S_PANEL, "pnl", "{\"rect\":[0,0,300,200]}", &pnl), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_PANEL);
	kp = ob_opn_obj(&pnl, OB_OP_ALL);
	KT_ASSERT(kp > 0);
	if ( kp <= 0 ) return;
	KT_ASSERT(!placed(kp));
	m.x = 20;
	m.y = 20;
	KT_ASSERT_ER(ob_map_rec(kp, 0, kw, &m), E_OK);
	KT_ASSERT(placed(kp));
	KT_ASSERT_ER(ob_rea_rec(kp, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&wp.parent, &w), 0);
	KT_ASSERT_EQ(wp.left, 20);
	KT_ASSERT_EQ(wp.right, 320);

	/* a part from its definition, a storage object */
	knl_memset(&cc, 0, sizeof(cc));
	cc.type = OB_T_STORAGE;
	cc.sub = OB_S_MEMORY;
	cc.name = (CONST UB *)"push";
	cc.json = (CONST UB *)ob_def_json;
	cc.jsonsz = sizeof(ob_def_json) - 1;
	KT_ASSERT_ER(ob_cre_obj(&cc, &def), E_OK);
	kd = ob_opn_obj(&def, OB_OP_ALL);
	knl_memset(&cc, 0, sizeof(cc));
	cc.type = OB_T_WINDOW;
	cc.sub = OB_S_PART;
	KT_ASSERT_ER(ob_cpy_obj(&def, &cc, &part), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&part, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_WINDOW);
	KT_ASSERT_EQ(r.sub, OB_S_PART);
	KT_ASSERT(has(r.name, s_len((CONST char *)r.name), "push"));
	kpart = ob_opn_obj(&part, OB_OP_ALL);
	KT_ASSERT(kpart > 0);
	if ( kpart <= 0 ) return;
	KT_ASSERT_ER(ob_get_atr(kpart, buf, sizeof(buf) - 1, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(kpart, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT_EQ(wp.right, 70);			/* the size the definition gave */

	/* how it looks released and pressed: two records, and no more */
	KT_ASSERT_ER(ob_apd_rec(kpart, OB_RT_TAD, 0, &recno), E_OK);
	KT_ASSERT_EQ(recno, OB_WR_OFF);
	KT_ASSERT_ER(ob_apd_rec(kpart, OB_RT_TAD, 0, &recno), E_OK);
	KT_ASSERT_EQ(recno, OB_WR_ON);
	KT_ASSERT_ER(ob_apd_rec(kpart, OB_RT_TAD, 0, &recno), E_LIMIT);
	KT_ASSERT_ER(ob_wri_rec(kpart, OB_WR_OFF, 0, ob_look_off, sizeof(ob_look_off) - 1,
				&asz), E_OK);
	KT_ASSERT_ER(ob_wri_rec(kpart, OB_WR_ON, 0, ob_look_off, sizeof(ob_look_off) - 1,
				&asz), E_OK);

	/* placed on the panel: one of its links, and drawn in its looks */
	m.x = 10;
	m.y = 10;
	KT_ASSERT_ER(ob_map_rec(kpart, 0, kp, &m), E_OK);
	KT_ASSERT_ER(ob_map_rec(kpart, 0, kp, &m), E_OBJ);	/* it is on already */
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_OK);
	KT_ASSERT_EQ(r.nrec, OB_WR_KIND + 1);
	KT_ASSERT_ER(ob_rea_rec(kp, OB_WR_KIND, 0, &got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &part), 0);
	KT_ASSERT_ER(dp_get_argb(wm_gid(wid), 20 + 10 + 35, 20 + 10 + 11, &px, 1, 1, 1), E_OK);
	KT_ASSERT(( px & 0xFFFFFF ) != ( wm_look(WM_LOOK_PART_FACE) & 0xFFFFFF ));

	/* pressed in the window: the part is told, not the window's owner */
	req.events = OB_E_PRESS | OB_E_RELEASE;
	KT_ASSERT(ob_ntf_evt(kpart, OB_REC_ANY, &req, port) > 0);
	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_BTN_DOWN;
	ev.wid = wid;
	ev.x = 20 + 10 + 5;
	ev.y = 20 + 10 + 5;
	knl_wmobj_event(&ev);
	KT_ASSERT(take_one(port, OB_E_PRESS, &part));
	ev.type = HID_EV_BTN_UP;
	knl_wmobj_event(&ev);
	KT_ASSERT(take_one(port, OB_E_RELEASE, &part));

	/* its value goes with it when it is taken off */
	v = 1;
	KT_ASSERT_ER(ob_wri_rec(kpart, OB_WR_STATE, 0, &v, sizeof(v), &asz), E_OK);
	KT_ASSERT_ER(ob_unm_rec(kpart, 0, kp), E_OK);
	KT_ASSERT(!placed(kpart));
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_OK);
	KT_ASSERT_EQ(r.nrec, OB_WR_KIND);
	v = 0;
	KT_ASSERT_ER(ob_rea_rec(kpart, OB_WR_STATE, 0, &v, sizeof(v), &asz), E_OK);
	KT_ASSERT_EQ(v, 1);
	m.x = 50;
	KT_ASSERT_ER(ob_map_rec(kpart, 0, kp, &m), E_OK);
	KT_ASSERT_ER(ob_rea_rec(kpart, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);
	KT_ASSERT_EQ(wp.left, 50);
	KT_ASSERT_EQ(ts_uuid_cmp(&wp.parent, &pnl), 0);

	/* a menu: its items added as records, the state in the subtype */
	KT_ASSERT_ER(free_obj(OB_S_MENU, "m", NULL, &menu), E_OK);
	km = ob_opn_obj(&menu, OB_OP_ALL);
	KT_ASSERT(km > 0);
	if ( km <= 0 ) return;
	KT_ASSERT_ER(ob_apd_rec(km, OB_RT_TAD, 0, &recno), E_OK);
	KT_ASSERT_ER(ob_wri_rec(km, recno, 0, "Alpha", 5, &asz), E_OK);
	KT_ASSERT_ER(ob_apd_rec(km, OB_RT_TAD, OB_MI_LINE, &recno), E_OK);
	KT_ASSERT_ER(ob_apd_rec(km, OB_RT_TAD, OB_MI_GREY, &recno), E_OK);
	KT_ASSERT_ER(ob_wri_rec(km, recno, 0, "Gamma", 5, &asz), E_OK);
	KT_ASSERT_ER(ob_lst_rec(km, rec, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, OB_WR_KIND + 3);
	KT_ASSERT_EQ(rec[OB_WR_KIND + 1].sub, OB_MI_LINE);
	KT_ASSERT_ER(ob_rea_rec(km, OB_WR_KIND + 2, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, 5);

	/* placed on the window, an item chosen: told on the item's record,
	   and the menu is taken down */
	req.events = OB_E_RELEASE;
	KT_ASSERT(ob_ntf_evt(km, OB_WR_KIND, &req, port) > 0);
	m.x = 30;
	m.y = 30;
	KT_ASSERT_ER(ob_map_rec(km, 0, kw, &m), E_OK);
	KT_ASSERT(placed(km));
	mp = number_of(km);
	pop = wm_panel_wid(mp);
	KT_ASSERT(mp > 0 && pop > 0);
	if ( mp > 0 && knl_pn_part(mp, 0, &pt) >= E_OK ) {
		knl_memset(&ev, 0, sizeof(ev));
		ev.wid = pop;
		ev.x = ( pt.r.left + pt.r.right ) / 2;
		ev.y = ( pt.r.top + pt.r.bottom ) / 2;
		ev.type = HID_EV_MOVE;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_DOWN;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_UP;
		knl_wmobj_event(&ev);
		KT_ASSERT(take_one(port, OB_E_RELEASE, &menu));
		KT_ASSERT(!placed(km));
		KT_ASSERT_ER(ob_ref_obj(&menu, &r), E_OK);	/* taken down, not gone */
	}

	/* a copy of a definition is what the definition says: here a panel */
	{
		CONST char	*pj = "{\"name\":\"p2\",\"tessronos\":{\"part\":"
				      "{\"sub\":\"panel\",\"rect\":[0,0,50,40]}}}";
		TS_UUID		d2, p2;

		knl_memset(&cc, 0, sizeof(cc));
		cc.type = OB_T_STORAGE;
		cc.sub = OB_S_MEMORY;
		cc.name = (CONST UB *)"p2";
		cc.json = (CONST UB *)pj;
		cc.jsonsz = s_len(pj);
		KT_ASSERT_ER(ob_cre_obj(&cc, &d2), E_OK);
		knl_memset(&cc, 0, sizeof(cc));
		cc.type = OB_T_WINDOW;
		KT_ASSERT_ER(ob_cpy_obj(&d2, &cc, &p2), E_OK);
		KT_ASSERT_ER(ob_ref_obj(&p2, &r), E_OK);
		KT_ASSERT_EQ(r.sub, OB_S_PANEL);
		KT_ASSERT_ER(ob_del_obj(&p2), E_OK);
		KT_ASSERT_ER(ob_del_obj(&d2), E_OK);
	}

	/* a placed part deleted comes off its panel */
	KT_ASSERT_ER(ob_del_obj(&part), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_OK);
	KT_ASSERT_EQ(r.nrec, OB_WR_KIND);
	/* a panel taken off its window stays an object */
	KT_ASSERT_ER(ob_unm_rec(kp, 0, kw), E_OK);
	KT_ASSERT(!placed(kp));
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_OK);

	ob_cls_obj(kpart);
	ob_cls_obj(km);
	ob_cls_obj(kp);
	ob_cls_obj(kd);
	KT_ASSERT_ER(ob_del_obj(&menu), E_OK);
	KT_ASSERT_ER(ob_del_obj(&pnl), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&pnl, &r), E_NOEXS);
	(void)ob_del_obj(&def);
	ob_cls_obj(kw);
	(void)ob_del_obj(&w);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	wm_update();
}

/* ---------------------------------------------------------------- a program's own window, a medium */

/*
 * A notice asked of a window that has already gone is refused. A program
 * whose window is taken away before it has asked to be told would
 * otherwise wait for a notice that was sent to nobody, and go on drawing
 * into whatever window gets the number next.
 */
LOCAL void test_notice_gone( void )
{
	T_OBCRE	c;
	T_OBNTF	req;
	TS_UUID	u, ch;
	ID	key, port;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"gone";
	c.json = (CONST UB *)"{\"rect\":[10,10,110,110],\"attr\":0}";
	c.jsonsz = s_len((CONST char *)c.json);
	if ( ob_cre_obj(&c, &u) < E_OK ) KT_SKIP("no window manager");
	key = ob_opn_obj(&u, OB_OP_ALL);
	port = port_open("ktest.goneport", &ch);
	KT_ASSERT(key > 0 && port > 0);
	if ( key <= 0 || port <= 0 ) return;

	/* taken away before anything was asked of it */
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE;
	KT_ASSERT_ER(ob_ntf_evt(key, OB_REC_ANY, &req, port), E_NOEXS);

	ob_cls_obj(key);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
}

LOCAL void test_window_program( void )
{
	T_DPRECT	o;
	T_OBNTF		req;
	T_WMWIN		w;
	TS_UUID		u, ch, shown;
	UB		j[256];
	SZ		asz = 0;
	INT		wid;
	ID		key, port;

	o.left = 50;  o.top = 60;  o.right = 250;  o.bottom = 200;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "prog");
	if ( wid < 0 ) KT_SKIP("no window manager");
	KT_ASSERT_ER(wm_obj_uuid(wid, &u), E_OK);
	make_uuid(&shown, 0x40);
	KT_ASSERT_ER(wm_obj_shows(wid, &shown), E_OK);
	key = ob_opn_obj(&u, OB_OP_ALL);
	port = port_open("ktest.progport", &ch);
	KT_ASSERT(key > 0 && port > 0);
	if ( key <= 0 || port <= 0 ) return;
	KT_ASSERT_ER(ob_get_atr(key, j, sizeof(j) - 1, &asz), E_OK);
	KT_ASSERT(has(j, (INT)asz, "\"shows\":\"40414243-"));

	/* deleted as an object, it is only asked: its program closes it */
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE;
	KT_ASSERT(ob_ntf_evt(key, OB_REC_ANY, &req, port) > 0);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT(take_one(port, OB_E_CLOSE, &u));
	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT(take_one(port, OB_E_DELETE, &u));

	ob_cls_obj(key);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	wm_update();
}

/* Whether a volume of that disk, or of a unit of it, is mounted */
LOCAL BOOL mounted_under( CONST UB *name )
{
	T_FSMNT	*m = (T_FSMNT *)Kmalloc(sizeof(T_FSMNT) * FS_MAX_MOUNT);
	INT	n, i, k;
	BOOL	yes = FALSE;

	if ( m == NULL ) {
		return TRUE;
	}
	n = fs_mounts(m, FS_MAX_MOUNT);
	for ( i = 0; i < n && i < FS_MAX_MOUNT && !yes; i++ ) {
		for ( k = 0; name[k] != 0 && m[i].dev[k] == name[k]; k++ ) ;
		if ( name[k] == 0 ) {
			for ( ; m[i].dev[k] >= '0' && m[i].dev[k] <= '9'; k++ ) ;
			yes = ( m[i].dev[k] == 0 );
		}
	}
	Kfree(m);
	return yes;
}

LOCAL void test_media( void )
{
	TS_UUID		list[16], ch, dev;
	T_OBREF		r;
	T_OBNTF		req;
	T_OBNTM		m;
	INT		cnt = 0, i;
	BOOL		found = FALSE;
	ID		key, port;

	KT_ASSERT_ER(ob_lst_obj(OB_T_DEVICE, OB_S_DISK, NULL, list, 16, &cnt), E_OK);
	for ( i = 0; i < cnt && !found; i++ ) {
		/* a medium said to go takes its mounts with it: one with none (design 12.2.3) */
		if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_DISK && !mounted_under(r.name) ) {
			dev = list[i];
			found = TRUE;
		}
	}
	if ( !found ) KT_SKIP("no disk");
	key = ob_opn_obj(&dev, OB_OP_ATRRD);
	port = port_open("ktest.media", &ch);
	KT_ASSERT(key > 0 && port > 0);
	if ( key <= 0 || port <= 0 ) return;
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_ATTACH | OB_E_DETACH;
	KT_ASSERT(ob_ntf_evt(key, OB_REC_ANY, &req, port) > 0);

	/* a driver says the medium went, and came back */
	knl_obdev_media(r.name, FALSE);
	KT_ASSERT(take_one(port, OB_E_DETACH, &dev));
	knl_obdev_media(r.name, TRUE);
	KT_ASSERT(take_one(port, OB_E_ATTACH, &dev));
	/* nothing came or went: nothing is told */
	knl_obdev_changed();
	KT_ASSERT_ER(take(port, &m), E_TMOUT);

	ob_cls_obj(key);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
}

/* ---------------------------------------------------------------- shared memory */

/* A program on the file system made a program object */
LOCAL ER make_prog( CONST char *path, TS_UUID *prog );

/* The test program as a program object, from the file system */
LOCAL ER make_hello( TS_UUID *prog )
{
	return make_prog("/boot/HELLO.ELF", prog);
}

/* A program taken in from a file by the common module: its executable in record 1 */
LOCAL ER make_prog( CONST char *path, TS_UUID *prog )
{
	return xf_import_path(path, "boot", NULL, NULL, NULL, prog);
}

/* A process of it that waits for a message before it ends */
LOCAL ID start_waiting( CONST TS_UUID *prog, TS_UUID *pu )
{
	T_OBCRE	c;
	UW	arg[2] = { 9, 1 };

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = *prog;
	c.arg = arg;
	c.argsz = sizeof(arg);
	if ( ob_cre_obj(&c, pu) < E_OK ) return 0;
	return knl_prc_of_uuid(pu);
}

LOCAL void end_waiting( ID pid )
{
	T_TSMSG	msg;
	T_PSTS	psts;

	knl_memset(&msg, 0, sizeof(msg));
	msg.type = 1;
	msg.size = 1;
	msg.body[0] = 9;
	(void)ts_snd_msg(pid, &msg, 1000);
	(void)ts_wai_prc(pid, &psts, 8000);
}

LOCAL void test_shared( void )
{
	TS_UUID	prog, p1, p2, shm;
	T_OBMAP	m;
	SZ	asz = 0;
	INT	recno = -1;
	UD	pa;
	UBINT	va;
	ID	pid1, pid2, k1, k2, km, kro;
	CONST char *text = "shared!";

	if ( !store_up ) KT_SKIP("no store");
	if ( fs_open("/boot/HELLO.ELF", O_RDONLY) < 0 ) KT_SKIP("no program");
	KT_ASSERT_ER(make_hello(&prog), E_OK);
	KT_ASSERT_ER(mem_obj("ob shm", &shm), E_OK);
	km = ob_opn_obj(&shm, OB_OP_ALL);
	KT_ASSERT(km > 0);
	KT_ASSERT_ER(ob_apd_rec(km, OB_RT_SYSDATA, 0, &recno), E_OK);
	KT_ASSERT_ER(ob_wri_rec(km, 0, 0, text, s_len(text), &asz), E_OK);

	pid1 = start_waiting(&prog, &p1);
	pid2 = start_waiting(&prog, &p2);
	KT_ASSERT(pid1 > 0 && pid2 > 0);
	k1 = ob_opn_obj(&p1, OB_OP_ALL);
	k2 = ob_opn_obj(&p2, OB_OP_ALL);
	KT_ASSERT(k1 > 0 && k2 > 0);
	if ( km <= 0 || k1 <= 0 || k2 <= 0 ) return;

	/* mapped into one process: an address of the shared window */
	knl_memset(&m, 0, sizeof(m));
	m.flags = OB_M_WRITE;
	m.size = 8192;
	KT_ASSERT_ER(ob_map_rec(km, 0, k1, &m), E_OK);
	va = (UBINT)m.addr;
	KT_ASSERT(va >= SHM_WINDOW_BASE && va < SHM_WINDOW_END);
	KT_ASSERT_EQ(m.size, 8192);
	pa = knl_prc_lookup(pid1, va);
	KT_ASSERT(pa != ~(UD)0);
	if ( pa == ~(UD)0 ) return;
	KT_ASSERT(has((CONST UB *)PA2VA(pa), 8, "shared!"));

	/* into another: the same address, the same pages */
	KT_ASSERT_ER(ob_map_rec(km, 0, k2, &m), E_OK);
	KT_ASSERT_EQ((UBINT)m.addr, va);
	KT_ASSERT_EQ(knl_prc_lookup(pid2, va), pa);
	KT_ASSERT_EQ(knl_prc_lookup(pid2, va + 4096), pa + 4096);

	/* written through the object: what the processes see changes */
	KT_ASSERT_ER(ob_wri_rec(km, 0, 0, "again", 5, &asz), E_OK);
	KT_ASSERT(has((CONST UB *)PA2VA(pa), 8, "again"));
	/* and it keeps to its pages */
	KT_ASSERT_ER(ob_wri_rec(km, 0, 8192, "x", 1, &asz), E_LIMIT);

	/* writable only with a key that may write */
	kro = ob_opn_obj(&shm, OB_OP_R);
	KT_ASSERT_ER(ob_map_rec(kro, 0, k1, &m), E_OACV);
	m.flags = 0;
	KT_ASSERT_ER(ob_map_rec(kro, 0, k1, &m), E_OK);
	ob_cls_obj(kro);
	/* the kernel has no space of its own to map into */
	KT_ASSERT_ER(ob_map_rec(km, 0, 0, &m), E_OBJ);

	/* taken out of one: gone there, still in the other */
	KT_ASSERT_ER(ob_unm_rec(km, 0, k1), E_OK);
	KT_ASSERT_EQ(knl_prc_lookup(pid1, va), ~(UD)0);
	KT_ASSERT_EQ(knl_prc_lookup(pid2, va), pa);
	KT_ASSERT_ER(ob_unm_rec(km, 0, k1), E_OBJ);

	/* a process ending does not take the shared pages with it */
	ob_cls_obj(k2);
	end_waiting(pid2);
	KT_ASSERT(has((CONST UB *)PA2VA(pa), 8, "again"));
	KT_ASSERT_ER(ob_wri_rec(km, 0, 0, "after", 5, &asz), E_OK);
	KT_ASSERT(has((CONST UB *)PA2VA(pa), 8, "after"));

	ob_cls_obj(k1);
	end_waiting(pid1);
	ob_cls_obj(km);
	KT_ASSERT_ER(ob_del_obj(&shm), E_OK);
	(void)ob_del_obj(&prog);
}

/* ---------------------------------------------------------------- the store's directory, from fs_ */

LOCAL void test_fs_guard( void )
{
	T_OBCRD	user;
	BOOL	boot_store;

	knl_memset(&user, 0, sizeof(user));
	make_uuid(&user.user, 0x50);
	user.ngrp = 1;
	make_uuid(&user.grp[0], 0x60);

	/* the system's store on the boot file system: its objects' files
	   are closed to a user, in any case of letters. With the system on
	   a native volume the same files on /boot are only files */
	boot_store = (BOOL)( knl_tsfs_guards("/boot/019a1132-762b-7b02-ba2a-a918a9b37c39.json") );
	KT_ASSERT_EQ(!knl_fs_path_let("/boot/019a1132-762b-7b02-ba2a-a918a9b37c39.json", &user), boot_store);
	KT_ASSERT_EQ(!knl_fs_path_let("/boot/019A1132-762B-7B02-BA2A-A918A9B37C39_0.XTAD", &user), boot_store);
	KT_ASSERT_EQ(!knl_fs_path_let("/BOOT/019a1132-762b-7b02-ba2a-a918a9b37c39_0_1.png", &user), boot_store);
	/* and not reached round about */
	KT_ASSERT(!knl_fs_path_let("/boot/./019a1132-762b-7b02-ba2a-a918a9b37c39.json", &user));
	KT_ASSERT(!knl_fs_path_let("/boot/x/../019a1132-762b-7b02-ba2a-a918a9b37c39.json", &user));
	KT_ASSERT(!knl_fs_path_let("//boot/HELLO.ELF", &user));
	/* what is beside them is open */
	KT_ASSERT(knl_fs_path_let("/boot/HELLO.ELF", &user));
	KT_ASSERT(knl_fs_path_let("/boot", &user));
	KT_ASSERT(knl_fs_path_let("/boot/sub/019a1132-762b-7b02-ba2a-a918a9b37c39.json", &user));
	KT_ASSERT(knl_fs_path_let("/boot/019a1132.json", &user));
	/* an administrator may go in */
	KT_ASSERT(knl_fs_path_let("/boot/019a1132-762b-7b02-ba2a-a918a9b37c39.json",
				  &knl_ob_crd_system));
	/* a store attached later is closed the same way */
	if ( store_up ) {
		KT_ASSERT(!knl_fs_path_let(STORE "/019a1132-762b-7b02-ba2a-a918a9b37c39.json",
					   &user));
	}
}

/* ---------------------------------------------------------------- a native volume */

#define NATDEV	kt_scratch()

LOCAL void test_native( void )
{
	T_OBCRE	c;
	TS_UUID	fixed, got;
	UB	*data, *back, names[256];
	SZ	asz = 0;
	INT	cnt = 0, i;
	ID	key;

	if ( NATDEV == NULL ) KT_SKIP(KT_NO_SCRATCH);
	if ( ob_att_vol(NATDEV, TSFS_STORE_BLK) < E_OK ) {
		if ( ts_format_blk(NATDEV, "OBJECTS") < E_OK
		  || ob_att_vol(NATDEV, TSFS_STORE_BLK) < E_OK ) {
			KT_SKIP("no native volume");
		}
	}
	data = (UB *)Kmalloc(3000);
	back = (UB *)Kmalloc(3000);
	if ( data == NULL || back == NULL ) KT_SKIP("no memory");
	for ( i = 0; i < 3000; i++ ) data[i] = (UB)( i * 7 );
	/* the first 16 bytes an icon's head, for ".ico" */
	data[0] = 0;  data[1] = 0;  data[2] = 1;  data[3] = 0;

	/* made under a fixed identity, and not twice */
	make_uuid(&fixed, 0x70);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.vol = NATDEV;
	c.name = (CONST UB *)"native fixed";
	c.uuid = fixed;
	(void)ob_del_obj(&fixed);			/* a run before may have left it */
	KT_ASSERT_ER(ob_cre_obj(&c, &got), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &fixed), 0);
	KT_ASSERT_ER(ob_cre_obj(&c, &got), E_OBJ);

	/* resources by every kind of name */
	key = ob_opn_obj(&fixed, OB_OP_ALL);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_wri_res(key, (CONST UB *)"_0_1.png", data, 3000), E_OK);
		KT_ASSERT_ER(ob_wri_res(key, (CONST UB *)".ico", data, 16), E_OK);
		KT_ASSERT_ER(ob_wri_res(key, (CONST UB *)"_0_bgm.mp3", data, 32), E_OK);
		KT_ASSERT_ER(ob_lst_res(key, names, sizeof(names), &cnt), E_OK);
		KT_ASSERT_EQ(cnt, 3);
		KT_ASSERT(has(names, sizeof(names), "_0_1.png"));
		KT_ASSERT(has(names, sizeof(names), ".ico"));
		KT_ASSERT(has(names, sizeof(names), "_0_bgm.mp3"));

		/* read from a place in it */
		KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)"_0_1.png", 1000, back, 100, &asz), E_OK);
		KT_ASSERT_EQ(asz, 100);
		KT_ASSERT_EQ(back[0], data[1000]);
		KT_ASSERT_EQ(back[99], data[1099]);
		KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)"_0_bgm.mp3", 0, back, 3000, &asz), E_OK);
		KT_ASSERT_EQ(asz, 32);

		KT_ASSERT_ER(ob_del_res(key, (CONST UB *)"_0_bgm.mp3"), E_OK);
		KT_ASSERT_ER(ob_lst_res(key, names, sizeof(names), &cnt), E_OK);
		KT_ASSERT_EQ(cnt, 2);
		KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)"_0_bgm.mp3", 0, back, 10, &asz), E_NOEXS);
		/* a name too long to keep */
		KT_ASSERT_ER(ob_wri_res(key, (CONST UB *)"_0_a-very-long-resource-name.bin", data, 4),
			     E_PAR);
		ob_cls_obj(key);
	}
	KT_ASSERT_ER(ob_del_obj(&fixed), E_OK);
	(void)ob_det_vol(NATDEV);
	Kfree(data);
	Kfree(back);
}

/* ---------------------------------------------------------------- the icon */

/* An icon file's head and a little of it: 1 picture of 16 by 16 */
LOCAL CONST UB ico_a[24] = { 0, 0, 1, 0, 1, 0, 16, 16, 0, 0, 1, 0, 32, 0,
			     'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A' };
LOCAL CONST UB png_b[16] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
			     'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B' };

/*
 * The icon is a part of the object beside its attributes: made with it,
 * read and replaced under the attributes' rights, told of as they are,
 * carried by a copy, gone with it; the name ".ico" reaches the same.
 */
LOCAL void icon_on( CONST char *what, T_OBCRE *c )
{
	TS_UUID	u, cp;
	T_OBNTF	req;
	T_OBNTM	m;
	TS_UUID	ch;
	UB	back[64];
	SZ	asz = 0;
	ID	key, ro, port;

	(void)what;
	c->icon = ico_a;
	c->iconsz = sizeof(ico_a);
	KT_ASSERT_ER(ob_cre_obj(c, &u), E_OK);
	key = ob_opn_obj(&u, OB_OP_ALL);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;

	/* made with it */
	KT_ASSERT_ER(ob_get_ico(key, NULL, 0, &asz), E_OK);
	KT_ASSERT_EQ(asz, (SZ)sizeof(ico_a));
	KT_ASSERT_ER(ob_get_ico(key, back, sizeof(back), &asz), E_OK);
	KT_ASSERT_EQ(back[14], 'A');

	/* the rights: reading it is reading the attributes, writing it writing them
	   (a file open for writing is had by one key at a time) */
	ob_cls_obj(key);
	ro = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE);
	KT_ASSERT(ro > 0);
	if ( ro > 0 ) {
		KT_ASSERT_ER(ob_get_ico(ro, back, sizeof(back), &asz), E_OACV);
		KT_ASSERT_ER(ob_set_ico(ro, png_b, sizeof(png_b)), E_OACV);
		ob_cls_obj(ro);
	}
	ro = ob_opn_obj(&u, OB_OP_ATRRD);
	KT_ASSERT(ro > 0);
	if ( ro > 0 ) {
		KT_ASSERT_ER(ob_get_ico(ro, back, sizeof(back), &asz), E_OK);
		KT_ASSERT_ER(ob_set_ico(ro, png_b, sizeof(png_b)), E_OACV);
		ob_cls_obj(ro);
	}
	key = ob_opn_obj(&u, OB_OP_ALL);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;

	/* only an ICO or a PNG, and not too large */
	KT_ASSERT_ER(ob_set_ico(key, (CONST UB *)"not an icon at all", 18), E_PAR);
	KT_ASSERT_ER(ob_set_ico(key, png_b, OB_ICO_MAX + 1), E_PAR);

	/* replaced, and told of as a change of the attributes */
	port = port_open("ktest.icon", &ch);
	KT_ASSERT(port > 0);
	if ( port > 0 ) {
		knl_memset(&req, 0, sizeof(req));
		req.events = OB_E_CHANGE;
		KT_ASSERT(ob_ntf_evt(key, OB_REC_ANY, &req, port) > 0);
	}
	KT_ASSERT_ER(ob_set_ico(key, png_b, sizeof(png_b)), E_OK);
	if ( port > 0 ) {
		KT_ASSERT_ER(take(port, &m), E_OK);
		KT_ASSERT_EQ(m.event, OB_E_CHANGE);
		KT_ASSERT_EQ(m.recno, -1);
	}
	KT_ASSERT_ER(ob_get_ico(key, back, sizeof(back), &asz), E_OK);
	KT_ASSERT_EQ(asz, (SZ)sizeof(png_b));
	KT_ASSERT_EQ(back[8], 'B');

	/* the name ".ico" is the same thing; no other "." name is */
	KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)".ico", 8, back, sizeof(back), &asz), E_OK);
	KT_ASSERT_EQ(asz, 8);
	KT_ASSERT_EQ(back[0], 'B');
	KT_ASSERT_ER(ob_wri_res(key, (CONST UB *)".png", png_b, sizeof(png_b)), E_PAR);

	/* a copy carries it */
	ob_cls_obj(key);
	if ( ob_cpy_obj(&u, NULL, &cp) >= E_OK ) {
		ID	kc = ob_opn_obj(&cp, OB_OP_ATRRD);

		KT_ASSERT(kc > 0);
		if ( kc > 0 ) {
			KT_ASSERT_ER(ob_get_ico(kc, back, sizeof(back), &asz), E_OK);
			KT_ASSERT_EQ(asz, (SZ)sizeof(png_b));
			ob_cls_obj(kc);
		}
		(void)ob_del_obj(&cp);
	} else {
		KT_ASSERT(FALSE);
	}

	/* taken away */
	key = ob_opn_obj(&u, OB_OP_ALL);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	KT_ASSERT_ER(ob_set_ico(key, NULL, 0), E_OK);
	KT_ASSERT_ER(ob_get_ico(key, back, sizeof(back), &asz), E_NOEXS);
	KT_ASSERT_ER(ob_del_res(key, (CONST UB *)".ico"), E_NOEXS);
	ob_cls_obj(key);
	if ( port > 0 ) {
		ob_cls_obj(port);
		(void)ob_del_obj(&ch);
	}
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}

LOCAL void test_icon( void )
{
	T_OBCRE	c;

	/* on a volume of the native store */
	if ( NATDEV != NULL && ( ob_att_vol(NATDEV, TSFS_STORE_BLK) >= E_OK
	  || ( ts_format_blk(NATDEV, "OBJECTS") >= E_OK
	    && ob_att_vol(NATDEV, TSFS_STORE_BLK) >= E_OK ) ) ) {
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.vol = NATDEV;
		c.name = (CONST UB *)"with an icon";
		icon_on("native", &c);
		(void)ob_det_vol(NATDEV);
	}

	/* in memory */
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.name = (CONST UB *)"icon in memory";
	icon_on("memory", &c);
}

/* ---------------------------------------------------------------- protection domains */

LOCAL void test_domain( void )
{
	T_OBCRE	c;
	T_OBPRT	p, d, got;
	T_OBCRD	user;
	T_OBREF	r;
	TS_UUID	u, *list;
	INT	cnt = 0, i;
	UINT	ops;
	BOOL	seen = FALSE;

	if ( !store_up ) KT_SKIP("no store");
	knl_memset(&user, 0, sizeof(user));
	make_uuid(&user.user, 0x50);
	user.ngrp = 1;
	make_uuid(&user.grp[0], 0x60);

	/* an object anyone may read and write */
	knl_memset(&p, 0, sizeof(p));
	p.owner = ob_user_system;
	p.group = ob_group_admin;
	p.mode = 0666;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.vol = STORE;
	c.name = (CONST UB *)"in a domain";
	c.prt = &p;
	(void)ob_set_dom(STORE, NULL);
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);
	ops = knl_ob_permit_on(&u, &user);
	KT_ASSERT_EQ(ops & ( OB_OP_READ | OB_OP_WRITE ), OB_OP_READ | OB_OP_WRITE);
	KT_ASSERT_ER(ob_get_dom(STORE, &got), E_NOEXS);

	/* a domain only its owner may enter: the user may do nothing */
	knl_memset(&d, 0, sizeof(d));
	d.owner = ob_user_system;
	d.group = ob_group_admin;
	d.mode = 0700;
	KT_ASSERT_ER(ob_set_dom(STORE, &d), E_OK);
	KT_ASSERT_ER(ob_get_dom(STORE, &got), E_OK);
	KT_ASSERT_EQ(got.mode, 0700);
	KT_ASSERT_EQ(knl_ob_permit_on(&u, &user), 0);
	KT_ASSERT_EQ(knl_ob_permit_on(&u, &knl_ob_crd_system), OB_OP_ALL);

	/* one that lets others read: reading, and no more */
	d.mode = 0704;
	KT_ASSERT_ER(ob_set_dom(STORE, &d), E_OK);
	ops = knl_ob_permit_on(&u, &user);
	KT_ASSERT(( ops & OB_OP_READ ) != 0);
	KT_ASSERT_EQ(ops & OB_OP_WRITE, 0);

	/* an owner inside it keeps the right to change the protection */
	d.mode = 0700;
	KT_ASSERT_ER(ob_set_dom(STORE, &d), E_OK);
	p.owner = user.user;
	KT_ASSERT_ER(ob_set_prt(&u, &p), E_OK);
	KT_ASSERT_EQ(knl_ob_permit_on(&u, &user), OB_OP_PROT);

	/* the domain itself is no object anyone sees */
	KT_ASSERT_ER(ob_ref_obj(&ob_uuid_domain, &r), E_NOEXS);
	list = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	if ( list != NULL ) {
		KT_ASSERT_ER(ob_lst_obj(OB_T_STORAGE, OB_S_FILE, NULL, list, 256, &cnt), E_OK);
		for ( i = 0; i < cnt; i++ ) {
			if ( ts_uuid_cmp(&list[i], &ob_uuid_domain) == 0 ) seen = TRUE;
		}
		KT_ASSERT(!seen);
		Kfree(list);
	}

	/* it is kept on the volume: attached again, it is there again */
	KT_ASSERT_ER(ob_det_vol(STORE), E_OK);
	KT_ASSERT_ER(ob_att_vol(STORE, 0), E_OK);
	KT_ASSERT_ER(ob_get_dom(STORE, &got), E_OK);
	KT_ASSERT_EQ(got.mode, 0700);

	/* taken away: the object's own protection is all there is */
	KT_ASSERT_ER(ob_set_dom(STORE, NULL), E_OK);
	KT_ASSERT_ER(ob_get_dom(STORE, &got), E_NOEXS);
	ops = knl_ob_permit_on(&u, &user);
	KT_ASSERT(( ops & OB_OP_WRITE ) != 0);

	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}

/* ---------------------------------------------------------------- TADjs plugins */

#define PLUG_BASE	"01a0d6d1-0901-7aaa-8bbb-0123456789ab"

LOCAL ER put_file( CONST char *path, CONST char *text, INT len )
{
	INT	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC), n;

	if ( fd < 0 ) return E_IO;
	n = fs_write(fd, text, len);
	fs_close(fd);
	return ( n == len ) ? E_OK : E_IO;
}

LOCAL void test_plugin( void )
{
	CONST char	*pj =
		"{\"id\":\"ktest-plugin\",\"name\":\"試験のプラグイン\",\"version\":\"1.2.0\","
		"\"type\":\"accessory\",\"basefile\":{\"json\":\"" PLUG_BASE ".json\","
		"\"xmltad\":\"" PLUG_BASE "_0.xtad\",\"ico\":\"" PLUG_BASE ".ico\"},"
		"\"main\":\"index.html\"}";
	CONST char	*bj = "{\"name\":\"試験の原紙\",\"refCount\":1,\"recordCount\":1}";
	CONST char	*bx = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>base</p></document></tad>";
	CONST char	*fj =
		"{\"id\":\"basic-figure-editor\",\"name\":\"基本図形編集\",\"type\":\"base\"}";
	CONST DTPROG	*pp;
	TS_UUID		prog, again, base;
	T_OBREF		r;
	UB		*rec, names[64];
	SZ		size = 0;
	INT		cnt = 0;
	ID		key;

	if ( fs_open("/boot/HELLO.ELF", O_RDONLY) < 0 ) KT_SKIP("no file system");
	(void)fs_mkdir("/boot/PLUGTEST");
	(void)fs_mkdir("/boot/PLUGFIG");
	KT_ASSERT_ER(put_file("/boot/PLUGTEST/plugin.json", pj, s_len(pj)), E_OK);
	KT_ASSERT_ER(put_file("/boot/PLUGTEST/" PLUG_BASE ".json", bj, s_len(bj)), E_OK);
	KT_ASSERT_ER(put_file("/boot/PLUGTEST/" PLUG_BASE "_0.xtad", bx, s_len(bx)), E_OK);
	KT_ASSERT_ER(put_file("/boot/PLUGTEST/" PLUG_BASE ".ico", "\0\0\1\0\1\0ICONICONIC", 16), E_OK);
	KT_ASSERT_ER(put_file("/boot/PLUGFIG/plugin.json", fj, s_len(fj)), E_OK);

	(void)dt_prog_start();			/* the program box it goes into */

	/* taken in: a program the registry knows by its id, with its template */
	KT_ASSERT_ER(dt_plugin_import("/boot/PLUGTEST", &prog), E_OK);
	pp = dt_prog_find((CONST UB *)"ktest-plugin");
	KT_ASSERT(pp != NULL);
	if ( pp != NULL ) {
		KT_ASSERT_EQ(ts_uuid_cmp(&pp->uuid, &prog), 0);
		KT_ASSERT_EQ(pp->kind, DT_PK_ACCESSORY);
		KT_ASSERT(!pp->builtin);
		KT_ASSERT_EQ(pp->nbase, 1);
		KT_ASSERT_ER(ts_str_to_uuid(PLUG_BASE, &base), E_OK);
		KT_ASSERT_EQ(ts_uuid_cmp(&pp->base[0], &base), 0);
	}
	KT_ASSERT_ER(ts_str_to_uuid(PLUG_BASE, &base), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&base, &r), E_OK);
	rec = om_obj_record(&base, 0, &size);
	KT_ASSERT(rec != NULL && has(rec, (INT)size, "<p>base</p>"));
	if ( rec != NULL ) Kfree(rec);
	key = ob_opn_obj(&base, OB_OP_R);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_lst_res(key, names, sizeof(names), &cnt), E_OK);
		KT_ASSERT_EQ(cnt, 1);
		KT_ASSERT(has(names, sizeof(names), ".ico"));
		ob_cls_obj(key);
	}

	/* taken in again: the same program, nothing made twice */
	KT_ASSERT_ER(dt_plugin_import("/boot/PLUGTEST", &again), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&again, &prog), 0);

	/* one the desktop carries itself: its own program answers */
	KT_ASSERT_ER(dt_plugin_import("/boot/PLUGFIG", &again), E_OK);
	pp = dt_prog_find((CONST UB *)"basic-figure-editor");
	KT_ASSERT(pp != NULL && pp->builtin);
	if ( pp != NULL ) {
		KT_ASSERT_EQ(ts_uuid_cmp(&again, &pp->uuid), 0);
	}
	KT_ASSERT_ER(dt_plugin_import("/boot/NOPLUGIN", &again), E_NOEXS);
}

/* ---------------------------------------------------------------- the system's faces and pictures */

LOCAL void test_sysres( void )
{
	TS_UUID	box, ids[40];
	UB	*buf;
	SZ	size = 0;
	INT	before, n;
	ID	was = fn_system();		/* put back after: the tests after draw with it */

	if ( fs_open("/boot/HELLO.ELF", O_RDONLY) < 0 ) KT_SKIP("no file system");
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_FONT_BOX, &box), E_OK);
	buf = om_obj_record(&box, 0, &size);
	KT_ASSERT(buf != NULL);
	before = ( buf != NULL ) ? om_store_links(buf, size, ids, 40) : 0;
	if ( buf != NULL ) Kfree(buf);
	if ( before == 0 ) KT_SKIP("no face on the disk");

	/* the box's links as the object layer lists them are the same */
	{
		TS_UUID	l[40];
		INT	k, cnt = -1;

		KT_ASSERT_ER(ob_lst_lnk(&box, NULL, l, 40, &cnt), E_OK);
		KT_ASSERT_EQ(cnt, before);
		for ( k = 0; k < cnt && k < 40; k++ ) {
			KT_ASSERT_EQ(ts_uuid_cmp(&l[k], &ids[k]), 0);
		}
	}

	/* each face an object of the system's, made from the file it names */
	for ( n = 0; n < before && n < 40; n++ ) {
		T_OBPRT	prt;
		UB	name[64];

		KT_ASSERT_ER(ob_get_prt(&ids[n], &prt), E_OK);
		KT_ASSERT_EQ(ts_uuid_cmp(&prt.owner, &ob_user_system), 0);
		KT_ASSERT_ER(xf_name(&ids[n], name, sizeof(name)), E_OK);
		KT_ASSERT_EQ(xf_data_rec(&ids[n]), 1);
	}

	/* the faces open from the objects, and one of them is the system's */
	n = dt_res_faces();
	KT_ASSERT(n >= 1 && n <= before);
	KT_ASSERT(fn_system() > 0);

	/* the ground's picture, from the 壁紙箱 */
	KT_ASSERT_ER(dt_res_wall(WM_WALL_FIT), E_OK);
	(void)wm_load_wall(NULL, WM_WALL_FIT);		/* the plain ground the tests after expect */
	(void)fn_set_system(was);
}

/* ---------------------------------------------------------------- the links of a box, by name */

/* A link to u in xmlTAD, as a box has it */
LOCAL INT put_link( UB *t, INT n, CONST TS_UUID *u, CONST char *extra )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	i;
	CONST char *parts[] = { "<link ", extra, "id=\"", us, "_0.xtad\" vobjid=\"x\"/>", NULL };

	(void)ts_uuid_to_str(u, us, sizeof(us));
	for ( i = 0; parts[i] != NULL; i++ ) {
		INT	k;

		for ( k = 0; parts[i][k] != 0; k++ ) t[n++] = (UB)parts[i][k];
	}
	t[n] = 0;
	return n;
}

/*
 * ob_fnd_lnk and ob_lst_lnk over a box of three links: an object known
 * by its object name, one known by the file name in its metadata, and
 * one that is not there; then the 書体箱, whose faces are found by the
 * same names xf_find knows them by.
 */
LOCAL void test_box_links( void )
{
	CONST char	meta_b[] = "{\"name\":\"b obj\",\"tessronos\":{\"file\":{\"name\":\"Beta.TXT\"}}}";
	T_OBCRE		c;
	TS_UUID		box, a, b, gone, got, ids[8], fbox;
	UB		*t;
	SZ		asz = 0;
	INT		n = 0, cnt = -1;
	ID		key;

	t = (UB *)Kmalloc(1024);
	if ( t == NULL ) KT_SKIP("no memory");
	KT_ASSERT_ER(mem_obj("Alpha", &a), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.name = (CONST UB *)"b obj";
	c.json = (CONST UB *)meta_b;
	c.jsonsz = s_len(meta_b);
	KT_ASSERT_ER(ob_cre_obj(&c, &b), E_OK);
	make_uuid(&gone, 0x5a);
	KT_ASSERT_ER(mem_obj("links box", &box), E_OK);

	/* the attributes before id are passed over; an id after them is still the link's */
	n = knl_oj_put(t, 0, 1024, "<tad version=\"1.0\"><document><p>");
	n = put_link(t, n, &a, "");
	n = put_link(t, n, &b, "vobjleft=\"1\" ");
	n = put_link(t, n, &gone, "");
	n = put_link(t, n, &a, "");
	n = knl_oj_put(t, n, 1024, "<linkx id=\"00000000-0000-0000-0000-000000000000\"/></p></document></tad>");
	key = ob_opn_obj(&box, OB_OP_R | OB_OP_W);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		INT	recno = -1;

		KT_ASSERT_ER(ob_apd_rec(key, OB_RT_TAD, 0, &recno), E_OK);
		KT_ASSERT_ER(ob_wri_rec(key, recno, 0, t, n, &asz), E_OK);
		ob_cls_obj(key);
	}

	/* every link, in order, one for each */
	KT_ASSERT_ER(ob_lst_lnk(&box, NULL, ids, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 4);
	KT_ASSERT_EQ(ts_uuid_cmp(&ids[0], &a), 0);
	KT_ASSERT_EQ(ts_uuid_cmp(&ids[1], &b), 0);
	KT_ASSERT_EQ(ts_uuid_cmp(&ids[2], &gone), 0);
	KT_ASSERT_EQ(ts_uuid_cmp(&ids[3], &a), 0);
	cnt = -1;
	KT_ASSERT_ER(ob_lst_lnk(&box, (CONST UB *)"", ids, 2, &cnt), E_OK);	/* fewer asked for */
	KT_ASSERT_EQ(cnt, 4);
	cnt = -1;
	KT_ASSERT_ER(ob_lst_lnk(&box, NULL, NULL, 0, &cnt), E_OK);		/* only how many */
	KT_ASSERT_EQ(cnt, 4);

	/* by the object's name, or the file's, without case, the extension left off or not */
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"alpha", &got), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &a), 0);
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"beta", &got), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &b), 0);
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"BETA.txt", &got), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &b), 0);
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"B OBJ", &got), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &b), 0);
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"beta.dat", &got), E_NOEXS);
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"bet", &got), E_NOEXS);
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"alphabet", &got), E_NOEXS);
	KT_ASSERT_ER(ob_fnd_lnk(&box, NULL, &got), E_OK);			/* the first */
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &a), 0);
	cnt = -1;
	KT_ASSERT_ER(ob_lst_lnk(&box, (CONST UB *)"Alpha", ids, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);
	cnt = -1;
	KT_ASSERT_ER(ob_lst_lnk(&box, (CONST UB *)"gamma", ids, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 0);

	/* what makes no sense is refused; a box that is not there is not there */
	KT_ASSERT_ER(ob_lst_lnk(&box, NULL, ids, 8, NULL), E_PAR);
	KT_ASSERT_ER(ob_lst_lnk(&box, NULL, NULL, 2, &cnt), E_PAR);
	KT_ASSERT_ER(ob_fnd_lnk(&box, (CONST UB *)"alpha", NULL), E_PAR);
	KT_ASSERT_ER(ob_fnd_lnk(&gone, (CONST UB *)"alpha", &got), E_NOEXS);
	KT_ASSERT_ER(ob_fnd_lnk(&a, (CONST UB *)"alpha", &got), E_NOEXS);	/* no links at all */

	/* the 書体箱: each face found by the name xf_find knows it by */
	if ( ts_str_to_uuid(SYSDEF_FONT_BOX, &fbox) >= E_OK
	  && ob_lst_lnk(&fbox, NULL, ids, 8, &cnt) >= E_OK && cnt > 0 ) {
		INT	i;

		for ( i = 0; i < cnt && i < 8; i++ ) {
			UB	name[64];

			if ( xf_name(&ids[i], name, sizeof(name)) < E_OK ) continue;
			KT_ASSERT_ER(ob_fnd_lnk(&fbox, name, &got), E_OK);
			KT_ASSERT_ER(xf_find(&fbox, name, &b), E_OK);
			KT_ASSERT_EQ(ts_uuid_cmp(&got, &b), 0);
		}
	}

	(void)ob_del_obj(&box);
	(void)ob_del_obj(&a);
	(void)ob_del_obj(&b);
	Kfree(t);
}

/* ---------------------------------------------------------------- a sound device as an object */

/* A notice of one kind for one object, waited for up to ms */
LOCAL BOOL wait_notice( ID port, UINT event, CONST TS_UUID *u, INT ms )
{
	T_OBNTM	m;

	for ( ; ms > 0; ms -= 50 ) {
		while ( take(port, &m) >= E_OK ) {
			if ( m.event == event && ts_uuid_cmp(&m.uuid, u) == 0 ) {
				return TRUE;
			}
		}
		tk_dly_tsk(50);
	}
	return FALSE;
}

LOCAL void test_sound_object( void )
{
	TS_UUID	list[32], snd;
	T_OBREF	r;
	T_OBPRT	p;
	T_OBREC	rec[OB_SND_NREC + 2];
	UB	buf[512], pcm[1024];
	SZ	asz = 0;
	INT	cnt = 0, i;
	BOOL	found = FALSE;
	ID	key;

	KT_ASSERT_ER(ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, list, 32, &cnt), E_OK);
	for ( i = 0; i < cnt && !found; i++ ) {
		if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_SOUND
		  && r.name[0] == 's' && r.name[4] == 0 ) {
			snd = list[i];
			found = TRUE;
		}
	}
	if ( !found ) KT_SKIP("no sound device");
	KT_ASSERT_EQ(r.nrec, OB_SND_NREC);

	/* everyone's to play: each opening is a channel of its own */
	KT_ASSERT_ER(ob_get_prt(&snd, &p), E_OK);
	KT_ASSERT_EQ(p.mode, 0666);

	key = ob_opn_obj(&snd, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	KT_ASSERT_ER(ob_lst_rec(key, rec, OB_SND_NREC + 2, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, OB_SND_NREC);
	KT_ASSERT(rec[OB_SND_INFO].size > 0);

	/* an attribute is read whole, and what was read can be written back */
	KT_ASSERT_ER(ob_rea_rec(key, OB_SND_INFO, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ((UD)asz, rec[OB_SND_INFO].size);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SND_INFO, 4, buf, sizeof(buf), &asz), E_PAR);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SND_MODE, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT(asz > 0);
	KT_ASSERT_ER(ob_wri_rec(key, OB_SND_MODE, 0, buf, asz, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SND_STAT, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT(asz > 0);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SND_NREC, 0, buf, sizeof(buf), &asz), E_NOEXS);

	/* sound written to record 1 is played */
	knl_memset(pcm, 0, sizeof(pcm));
	KT_ASSERT_ER(ob_wri_rec(key, OB_SND_PCM, 0, pcm, sizeof(pcm), &asz), E_OK);

	/* the USB audio device taken out and put back: the sound device's
	   object is told its medium went and came, as a disk's is. Only
	   the QEMU machine has a monitor to do that with */
#ifndef RPI5
	{
		TS_UUID	ch;
		T_OBNTF	req;
		ID	port = port_open("ktest.sndport", &ch);

		knl_memset(&req, 0, sizeof(req));
		req.events = OB_E_ATTACH | OB_E_DETACH;
		KT_ASSERT(port > 0 && ob_ntf_evt(key, OB_REC_ANY, &req, port) > 0);
		tm_printf((UB*)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"aud1\"}}\n");
		if ( wait_notice(port, OB_E_DETACH, &snd, 5000) ) {
			tm_printf((UB*)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":"
				  "{\"driver\":\"usb-audio\",\"bus\":\"xhci.0\",\"port\":\"4.1\","
				  "\"audiodev\":\"snd0\",\"id\":\"aud1\"}}\n");
			KT_ASSERT(wait_notice(port, OB_E_ATTACH, &snd, 5000));
		} else {
			tm_printf((UB*)"  no QEMU monitor: the audio device stayed\n");
		}
		if ( port > 0 ) ob_cls_obj(port);
		(void)ob_del_obj(&ch);
	}
#endif
	ob_cls_obj(key);
}

/* ---------------------------------------------------------------- a program with a window of its own */

/* A window object of a name, waited for up to ms */
LOCAL BOOL wait_window( CONST char *name, TS_UUID *out, INT ms )
{
	TS_UUID	*list = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 128);
	T_OBREF	r;
	INT	cnt, i;
	BOOL	found = FALSE;

	for ( ; list != NULL && ms > 0 && !found; ms -= 100 ) {
		cnt = 0;
		if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 128, &cnt) >= E_OK ) {
			for ( i = 0; i < cnt && !found; i++ ) {
				if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_WINDOW
				  && has(r.name, s_len((CONST char *)r.name), name) ) {
					*out = list[i];
					found = TRUE;
				}
			}
		}
		if ( !found ) tk_dly_tsk(100);
	}
	if ( list != NULL ) Kfree(list);
	return found;
}

LOCAL void test_app_process( void )
{
	T_OBCRE	c;
	T_PSTS	psts;
	T_WMWIN	w;
	T_WMEV	ev;
	T_OBREF	r;
	TS_UUID	prog, pu, win;
	UW	frame = 0, inside = 0;
	INT	wid, gid, sx, sy, px = -1, py = -1, got = 0;
	UINT	bar = 0;
	ID	pid, kw;

	if ( !store_up ) KT_SKIP("no store");
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_CLOCK, &prog), E_OK);
	if ( ob_ref_obj(&prog, &r) < E_OK ) KT_SKIP("no clock program");

	/* started as a process: it makes its window, and draws in it from EL0 */
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	KT_ASSERT(wait_window("時計", &win, 5000));
	kw = ob_opn_obj(&win, OB_OP_R);
	KT_ASSERT(kw > 0);
	if ( kw <= 0 || pid <= 0 ) return;
	wid = number_of(kw);
	KT_ASSERT(wid > 0);
	tk_dly_tsk(600);				/* a turn of its loop: it has drawn */
	gid = wm_gid(wid);
	KT_ASSERT_ER(dp_get_argb(gid, 9, 30, &frame, 1, 1, 1), E_OK);
	KT_ASSERT_ER(dp_get_argb(gid, 30, 16, &inside, 1, 1, 1), E_OK);
	KT_ASSERT_EQ(frame & 0xFFFFFF, 0x606060);	/* its frame */
	KT_ASSERT_EQ(inside & 0xFFFFFF, 0xFFFFFF);	/* its paper */

	/* the pictogram pressed: it is asked to close, and it does, and ends */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	for ( sy = w.outer.top; sy < w.work.top && px < 0; sy++ ) {
		for ( sx = w.outer.left; sx < w.outer.left + 60 && px < 0; sx++ ) {
			if ( wm_part_at(sx, sy, &got, &bar) == WM_PART_PICT && got == wid ) {
				px = sx;
				py = sy;
			}
		}
	}
	KT_ASSERT(px >= 0);
	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_BTN_DOWN;
	ev.wid = wid;
	ev.x = px - w.work.left;
	ev.y = py - w.work.top;
	ev.when = 3000000000ULL;
	knl_wmobj_event(&ev);
	ev.when += 100000000ULL;			/* a double press on it */
	knl_wmobj_event(&ev);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_ER(ob_ref_obj(&win, &r), E_NOEXS);	/* it took its window away */
	ob_cls_obj(kw);
	wm_update();
}

/* The clock as the desktop knows it: a program in the box, started from there */
LOCAL void test_app_from_box( void )
{
	CONST DTPROG	*pp;
	TS_UUID		win;
	T_OBREF		r;
	TS_UUID		clk;

	if ( ts_str_to_uuid(SYSDEF_PROG_CLOCK, &clk) < E_OK || ob_ref_obj(&clk, &r) < E_OK ) {
		KT_SKIP("no clock program");
	}
	(void)dt_prog_start();
	pp = dt_prog_find((CONST UB *)"clock");
	KT_ASSERT(pp != NULL);
	if ( pp == NULL ) return;
	KT_ASSERT_EQ(pp->kind, DT_PK_ACCESSORY);
	KT_ASSERT(pp->menu);
	KT_ASSERT(!pp->builtin);
	KT_ASSERT_ER(dt_prog_run(NULL, pp, NULL), E_OK);
	KT_ASSERT(wait_window("時計", &win, 5000));

	/* its window taken away by someone else: it sees that, and ends */
	KT_ASSERT_ER(ob_del_obj(&win), E_OK);
	tk_dly_tsk(1000);
	KT_ASSERT_ER(ob_ref_obj(&win, &r), E_NOEXS);
	KT_ASSERT(!wait_window("時計", &win, 300));
	wm_update();
}

/*
 * The system as an object (obsys.c): a virtual device of fixed UUID,
 * among the devices listed, whose records are lines of text. The
 * system's own record names it; the scheme's says which is in use and
 * is changed by writing a number; the power's is not a place for
 * anything else; the description and a record past the last are not
 * written.
 */
LOCAL void test_system_object( void )
{
	T_OBREF	r;
	T_OBREC	recs[OB_SYS_NREC + 2];
	UB	*t;
	char	v[64];
	ID	key;
	SZ	asz = 0;
	INT	cnt = 0, was;

	KT_ASSERT_ER(ob_ref_obj(&ob_uuid_system, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_DEVICE);
	KT_ASSERT_EQ(r.sub, OB_S_SYSTEM);
	KT_ASSERT_EQ(r.nrec, OB_SYS_NREC);
	key = ob_opn_obj(&ob_uuid_system, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	t = (UB *)Kmalloc(4096);
	KT_ASSERT(t != NULL);
	if ( t == NULL ) {
		ob_cls_obj(key);
		return;
	}
	KT_ASSERT_ER(ob_lst_rec(key, recs, OB_SYS_NREC + 2, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, OB_SYS_NREC);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SYS_INFO, 0, t, 4096, &asz), E_OK);
	KT_ASSERT(cf_get(t, (INT)asz, "NAME", 0, v, sizeof(v)) && knl_strcmp(v, "TessronOS") == 0);
	KT_ASSERT(cf_get(t, (INT)asz, "CPUS", 0, v, sizeof(v)) && cf_num(v, 0) >= 1);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SYS_SCHEME, 0, t, 4096, &asz), E_OK);
	KT_ASSERT(cf_get(t, (INT)asz, "SCHEME", 0, v, sizeof(v)));
	was = cf_num(v, 0);
	KT_ASSERT(cf_get(t, (INT)asz, "NAME", 1, v, sizeof(v)));	/* more than one there */
	KT_ASSERT_ER(ob_wri_rec(key, OB_SYS_SCHEME, 0, "1", 1, &asz), E_OK);
	KT_ASSERT_EQ((INT)wm_scheme(), 1);
	v[0] = (char)( '0' + was );
	KT_ASSERT_ER(ob_wri_rec(key, OB_SYS_SCHEME, 0, v, 1, &asz), E_OK);
	KT_ASSERT_EQ((INT)wm_scheme(), was);
	KT_ASSERT_ER(ob_wri_rec(key, OB_SYS_POWER, 0, "SOON", 4, &asz), ( ob_rea_rec(key, OB_SYS_POWER, 0, t, 4096, &asz) >= E_OK && asz > 5 ) ? E_PAR : E_NOSPT);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, "x", 1, &asz), E_RONLY);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SYS_NREC, 0, t, 4096, &asz), E_NOEXS);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SYS_NET, 0, t, 4096, &asz), E_OK);
	KT_ASSERT(cf_get(t, (INT)asz, "BUSY", 0, v, sizeof(v)));
	Kfree(t);
	ob_cls_obj(key);
}

/* ---------------------------------------------------------------- packages (dtpkg.c) */

#define PK_N	5

LOCAL void pk_le32( UB *p, UW v )
{
	p[0] = (UB)v;  p[1] = (UB)( v >> 8 );  p[2] = (UB)( v >> 16 );  p[3] = (UB)( v >> 24 );
}

/* A package made in memory, as tools/mktpk.py makes one: its bytes, the caller's to Kfree */
LOCAL UB *pk_make( CONST char *const *names, UB *const *data, CONST SZ *size,
		   CONST UW *rt, INT n, SZ *p_len )
{
	SZ	len = 16 + 64 * (SZ)n, at;
	UB	*b;
	INT	i, k;

	for ( i = 0; i < n; i++ ) len += size[i];
	b = (UB *)Kmalloc(len);
	if ( b == NULL ) return NULL;
	knl_memset(b, 0, 16 + 64 * (SZ)n);
	b[0] = 'T';  b[1] = 'S';  b[2] = 'P';  b[3] = 'K';
	b[4] = 1;  b[6] = (UB)n;
	at = 16 + 64 * (SZ)n;
	for ( i = 0; i < n; i++ ) {
		UB	*d = b + 16 + 64 * i;

		for ( k = 0; names[i][k] != 0 && k < 47; k++ ) d[k] = (UB)names[i][k];
		pk_le32(d + 48, (UW)at);
		pk_le32(d + 52, (UW)size[i]);
		pk_le32(d + 56, rt[i]);
		knl_memcpy(b + at, data[i], size[i]);
		at += size[i];
	}
	*p_len = len;
	return b;
}

/* A new object holding bytes in record 1, as ファイル変換 takes a file in */
LOCAL ER pk_object( CONST UB *bytes, SZ len, TS_UUID *p_u )
{
	CONST char	*j = "{\"name\":\"時計のパッケージ\",\"refCount\":0,\"recordCount\":2}";
	CONST char	*x = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>clock.tpk</p></document></tad>";
	T_OBCRE		c;
	ER		er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)j;
	c.jsonsz = (SZ)s_len(j);
	er = ob_cre_obj(&c, p_u);
	if ( er >= E_OK ) er = om_obj_record_put(p_u, 0, OB_RT_TAD, 0, (CONST UB *)x, (SZ)s_len(x));
	if ( er >= E_OK ) er = om_obj_record_put(p_u, 1, OB_RT_SYSDATA, 0, bytes, len);
	return er;
}

/*
 * An application from a package: the clock, packed as it is, is not
 * newer than itself and goes on only when told to; packed as R1.001 it
 * updates; deleted, the registry no longer knows it and its object is
 * gone; installed again from the package, it is back at R1.001 and
 * runs. What is not a package, and a program of the desktop's own, are
 * refused.
 */
LOCAL void test_package( void )
{
	TS_UUID		clk, pkg, bad, got, sp;
	CONST DTPROG	*pp;
	T_OBREF		r;
	char		us[TS_UUID_STRLEN + 1], nm[PK_N][48], man[256];
	CONST char	*names[PK_N];
	UB		*data[PK_N], *raw;
	SZ		size[PK_N], len = 0;
	UW		rt[PK_N] = { OB_RT_SYSDATA, OB_RT_SYSDATA, OB_RT_TAD, OB_RT_PROG, OB_RT_SYSDATA };
	UINT		how = 0;
	INT		i, k;

	if ( ts_str_to_uuid(SYSDEF_PROG_CLOCK, &clk) < E_OK || ob_ref_obj(&clk, &r) < E_OK ) {
		KT_SKIP("no clock program");
	}
	(void)dt_prog_start();
	pp = dt_prog_find((CONST UB *)"clock");
	KT_ASSERT(pp != NULL);
	if ( pp == NULL ) return;
	KT_ASSERT(s_len((CONST char *)pp->version) == 6 && pp->version[0] == 'R');
	KT_ASSERT_EQ((INT)dt_ver_num((CONST UB *)"R12.005"), 12005);
	KT_ASSERT_EQ((INT)dt_ver_num((CONST UB *)"R1.00"), -1);
	KT_ASSERT_EQ((INT)dt_ver_num((CONST UB *)"1.000"), -1);

	/* the clock's files, as the package carries them */
	(void)ts_uuid_to_str(&clk, us, sizeof(us));
	for ( i = 0; i < PK_N; i++ ) names[i] = nm[i];
	knl_strcpy(nm[0], "package.json");
	knl_strcpy(nm[1], us);  knl_strcat(nm[1], ".json");
	knl_strcpy(nm[2], us);  knl_strcat(nm[2], "_0.xtad");
	knl_strcpy(nm[3], us);  knl_strcat(nm[3], "_1.bin");
	knl_strcpy(nm[4], us);  knl_strcat(nm[4], ".ico");
	data[1] = om_obj_meta(&clk, &size[1]);
	data[2] = om_obj_record(&clk, 0, &size[2]);
	data[3] = om_obj_record(&clk, 1, &size[3]);
	data[4] = om_obj_icon(&clk, &size[4]);
	KT_ASSERT(data[1] != NULL && data[2] != NULL && data[3] != NULL && data[4] != NULL);
	if ( data[1] == NULL || data[2] == NULL || data[3] == NULL || data[4] == NULL ) goto out;
	knl_strcpy(man, "{\"format\":1,\"program\":\"");
	knl_strcat(man, us);
	knl_strcat(man, "\",\"id\":\"clock\",\"name\":\"時計\",\"version\":\"R1.000\",\"objects\":[\"");
	knl_strcat(man, us);
	knl_strcat(man, "\"]}");
	data[0] = (UB *)man;
	size[0] = (SZ)s_len(man);

	/*
	 * R1.000 put on when told to (the disk may hold what an earlier run
	 * left), and then, the same version, not without being told to
	 */
	for ( k = 0; k + 6 < (INT)size[1]; k++ ) {
		if ( data[1][k] == 'R' && data[1][k + 1] == '1' && data[1][k + 2] == '.'
		  && data[1][k + 3] == '0' && data[1][k + 4] == '0' ) {
			data[1][k + 5] = '0';
		}
	}
	raw = pk_make(names, data, size, rt, PK_N, &len);
	KT_ASSERT(raw != NULL);
	if ( raw == NULL ) goto out;
	KT_ASSERT_ER(pk_object(raw, len, &pkg), E_OK);
	Kfree(raw);
	KT_ASSERT_ER(dt_pkg_install(&pkg, TRUE, &got, &how), E_OK);
	KT_ASSERT_EQ((INT)how, DT_PKG_UPDATED);
	KT_ASSERT(ts_uuid_cmp(&got, &clk) == 0);
	pp = dt_prog_find((CONST UB *)"clock");
	KT_ASSERT(pp != NULL && dt_ver_num(pp->version) == 1000);
	KT_ASSERT_ER(dt_pkg_install(&pkg, FALSE, &got, &how), E_LIMIT);
	(void)ob_del_obj(&pkg);

	/* R1.001, in the package and in the program's metadata */
	for ( i = 0; man[i] != 0; i++ ) {
		if ( man[i] == 'R' && man[i + 1] == '1' && man[i + 2] == '.' ) man[i + 5] = '1';
	}
	for ( k = 0; k + 6 < (INT)size[1]; k++ ) {
		if ( data[1][k] == 'R' && data[1][k + 1] == '1' && data[1][k + 2] == '.'
		  && data[1][k + 3] == '0' && data[1][k + 4] == '0' && data[1][k + 5] == '0' ) {
			data[1][k + 5] = '1';
		}
	}
	raw = pk_make(names, data, size, rt, PK_N, &len);
	KT_ASSERT(raw != NULL);
	if ( raw == NULL ) goto out;
	KT_ASSERT_ER(pk_object(raw, len, &pkg), E_OK);
	Kfree(raw);
	KT_ASSERT_ER(dt_pkg_install(&pkg, FALSE, &got, &how), E_OK);
	KT_ASSERT_EQ((INT)how, DT_PKG_UPDATED);
	pp = dt_prog_find((CONST UB *)"clock");
	KT_ASSERT(pp != NULL && dt_ver_num(pp->version) == 1001);

	/* deleted: the registry does not know it, and its object is gone */
	KT_ASSERT_ER(dt_prog_remove(&clk), E_OK);
	KT_ASSERT(dt_prog_find((CONST UB *)"clock") == NULL);
	KT_ASSERT_ER(ob_ref_obj(&clk, &r), E_NOEXS);

	/* from the package again: new, at R1.001, and it runs */
	KT_ASSERT_ER(dt_pkg_install(&pkg, FALSE, &got, &how), E_OK);
	KT_ASSERT_EQ((INT)how, DT_PKG_NEW);
	pp = dt_prog_find((CONST UB *)"clock");
	KT_ASSERT(pp != NULL && dt_ver_num(pp->version) == 1001 && !pp->builtin);
	if ( pp != NULL ) {
		KT_ASSERT_ER(dt_prog_run(NULL, pp, NULL), E_OK);
		KT_ASSERT(wait_window("時計", &sp, 5000));
		(void)ob_del_obj(&sp);
		tk_dly_tsk(500);
	}
	(void)ob_del_obj(&pkg);

	/* not a package; the desktop's own */
	KT_ASSERT_ER(pk_object((CONST UB *)"not a package at all", 20, &bad), E_OK);
	KT_ASSERT_ER(dt_pkg_install(&bad, FALSE, &got, &how), E_OBJ);
	(void)ob_del_obj(&bad);
	pp = dt_prog_find((CONST UB *)"basic-text-editor");
	KT_ASSERT(pp != NULL && pp->builtin);
	if ( pp != NULL ) KT_ASSERT_ER(dt_prog_remove(&pp->uuid), E_PAR);
	wm_update();
out:
	for ( i = 1; i < PK_N; i++ ) {
		if ( data[i] != NULL ) Kfree(data[i]);
	}
}

EXPORT void ktest_ob( void )
{
	KT_RUN(test_hash);
	KT_RUN(test_json);
	KT_RUN(test_permit);
	KT_RUN(test_prt_text);
	KT_RUN(test_memory);
	KT_RUN(test_channel);
	KT_RUN(test_keys);
	KT_RUN(test_file);
	KT_RUN(test_list);
	KT_RUN(test_device);
	KT_RUN(test_process);
	KT_RUN(test_user);
	KT_RUN(test_transfer);
	KT_RUN(test_box_links);
	KT_RUN(test_notice);
	KT_RUN_SCREEN(test_window);
	KT_RUN_SCREEN(test_window_events);
	KT_RUN_SCREEN(test_parts);
	KT_RUN_SCREEN(test_window_program);
	KT_RUN(test_notice_gone);
	KT_RUN(test_media);
	KT_RUN(test_sound_object);
	KT_RUN(test_shared);
	KT_RUN_SCREEN(test_app_process);
	KT_RUN_SCREEN(test_app_from_box);
	KT_RUN(test_fs_guard);
	KT_RUN(test_native);
	KT_RUN(test_icon);
	KT_RUN(test_domain);
	KT_RUN_SCREEN(test_plugin);
	KT_RUN_SCREEN(test_sysres);
	KT_RUN_SCREEN(test_system_object);
	KT_RUN_SCREEN(test_package);

	if ( store_up ) {
		(void)ob_det_vol(STORE);
		store_up = FALSE;
	}
}
