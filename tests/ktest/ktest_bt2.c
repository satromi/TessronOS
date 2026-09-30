/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_bt2.c
 *	Every resource is a real object (design 18.19)
 *
 *	The kernel's own tables of resources walked one by one, and for each
 *	entry the object that stands for it looked for through the name
 *	manager: its kind has to be the one the resource is, and it has to
 *	be in the list of objects of that kind. A resource added to the
 *	system gets its table walked here in the same change.
 */

#include "kernel.h"
#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/wm.h>
#include <ts/tray.h>
#include <ts/sysdef.h>
#include <ts/om.h>
#include <ts/json.h>
#include <ts/hid.h>
#include <ts/usb.h>
#include <ts/disp.h>
#include "../../outer_kernel/wm/wmobj.h"

#define LIST_MAX	1024

LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (BOOL)( a[i] == b[i] );
}

/* Whether an object of a kind is in the name manager's list of that kind */
LOCAL BOOL listed( UINT type, CONST TS_UUID *u )
{
	TS_UUID	*buf;
	INT	cnt = 0, i;
	BOOL	found = FALSE;

	buf = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * LIST_MAX);
	if ( buf == NULL ) {
		return FALSE;
	}
	if ( ob_lst_obj(type, 0, NULL, buf, LIST_MAX, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < LIST_MAX && !found; i++ ) {
			found = (BOOL)( ts_uuid_cmp(&buf[i], u) == 0 );
		}
	}
	Kfree(buf);
	return found;
}

/* The object of a UUID, of the kind given, known and listed */
LOCAL BOOL is_object( CONST TS_UUID *u, UINT type )
{
	T_OBREF	r;

	return (BOOL)( ob_ref_obj(u, &r) >= E_OK && r.type == type && listed(type, u) );
}

/* every process in the process table */
LOCAL void test_processes( void )
{
	ID	pids[16];
	TS_UUID	u;
	INT	n, i;

	n = knl_prc_list(pids, 16);
	KT_ASSERT(n >= 1);			/* init at least */
	for ( i = 0; i < n && i < 16; i++ ) {
		TS_UUID	nil;

		KT_ASSERT_ER(knl_prc_uuid(pids[i], &u, NULL), E_OK);
		knl_memset(&nil, 0, sizeof(nil));
		if ( ts_uuid_cmp(&u, &nil) == 0 ) {
			/* started before the clock was set: no UUID, so no object (design 11.4) */
			tm_printf((UB *)"  pid %d started before the clock was set: not an object\n",
				  (INT)pids[i]);
			continue;
		}
		KT_ASSERT(is_object(&u, OB_T_PROCESS));
	}
}

/*
 * every object of the core a process made (design 9.15): not an object
 * itself but a name inside its process, so it has to belong to a
 * process in the table -- none is left behind by one that has gone
 */
LOCAL void test_process_core_objects( void )
{
	ID	pids[16];
	INT	n, i, kind, sum;

	n = knl_prc_list(pids, 16);
	for ( kind = TK_OWN_TSK; kind <= TK_OWN_MBF; kind++ ) {
		for ( i = 0, sum = 0; i < n && i < 16; i++ ) {
			sum += knl_own_list(kind, pids[i], NULL, 0);
		}
		KT_ASSERT_EQ(knl_own_list(kind, TK_OWN_ANY, NULL, 0), sum);
	}
}

/* every device the device management knows, and every subunit of one */
LOCAL void test_devices( void )
{
	T_LDEV	ld[16];
	TS_UUID	*buf;
	T_OBREF	r;
	UB	want[L_DEVNM + 4];
	INT	n, i, k, s, cnt = 0;
	BOOL	found;

	buf = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * LIST_MAX);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(ob_lst_obj(OB_T_DEVICE, 0, NULL, buf, LIST_MAX, &cnt), E_OK);
	n = tk_lst_dev(ld, 0, 16);
	KT_ASSERT(n >= 1);
	for ( i = 0; i < n && i < 16; i++ ) {
		for ( s = -1; s < ld[i].nsub && s < 10; s++ ) {
			for ( k = 0; k < L_DEVNM && ld[i].devnm[k] != 0; k++ ) want[k] = ld[i].devnm[k];
			if ( s >= 0 ) want[k++] = (UB)( '0' + s );
			want[k] = 0;
			found = FALSE;
			for ( k = 0; k < cnt && k < LIST_MAX && !found; k++ ) {
				found = (BOOL)( ob_ref_obj(&buf[k], &r) >= E_OK && same_name(r.name, want) );
			}
			if ( !found ) {
				tm_printf((UB *)"  no object for device %s\n", want);
			}
			KT_ASSERT(found);
		}
	}
	Kfree(buf);
}

/*
 * the devices outside the device management: the random source, the
 * input, the screen, every keyboard and pointer and every device on the
 * USB buses
 */
LOCAL void test_devices_added( void )
{
	T_USBDEV	*d;
	T_HIDINFO	h[16];
	T_DISPSPEC	s;
	TS_UUID		u;
	INT		n, i;

	KT_ASSERT(is_object(&ob_uuid_random, OB_T_DEVICE));
	KT_ASSERT(is_object(&ob_uuid_input, OB_T_DEVICE));
	if ( ts_disp_ref(&s) >= E_OK ) {
		KT_ASSERT(is_object(&ob_uuid_display, OB_T_DEVICE));
	}
	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		KT_ASSERT_ER(knl_obinput_uuid(h[i].id, &u), E_OK);
		KT_ASSERT(is_object(&u, OB_T_DEVICE));
	}
	d = (T_USBDEV *)Kmalloc(sizeof(T_USBDEV) * 32);
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	n = ts_usb_lst_dev(d, 32);
	for ( i = 0; i < n && i < 32; i++ ) {
		KT_ASSERT_ER(knl_obusb_uuid(d[i].dev, &u), E_OK);
		KT_ASSERT(is_object(&u, OB_T_DEVICE));
	}
	Kfree(d);
}

/* every window open, and the panels and menus on them */
LOCAL void test_windows( void )
{
	T_WMWIN	w;
	TS_UUID	u;
	INT	wid, n = 0;

	for ( wid = 1; wid <= WM_MAX_WIN; wid++ ) {
		if ( wm_ref(wid, &w) < E_OK ) {
			continue;
		}
		n++;
		KT_ASSERT_ER(wm_obj_uuid(wid, &u), E_OK);
		KT_ASSERT(is_object(&u, OB_T_WINDOW));
	}
	if ( n == 0 ) KT_SKIP("no window open");
}

/* the tray, once it has been used */
LOCAL void test_tray( void )
{
	T_TRSTS	*sts = (T_TRSTS *)Kmalloc(sizeof(T_TRSTS));
	TS_UUID	u;

	KT_ASSERT(sts != NULL);
	if ( sts == NULL ) return;
	KT_ASSERT_ER(tr_get_sts(sts), E_OK);
	Kfree(sts);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_TRAY, &u), E_OK);
	KT_ASSERT(is_object(&u, OB_T_STORAGE));
}

/* the definitions the code knows by UUID */
LOCAL void test_definitions( void )
{
	CONST char * CONST ids[] = {
		SYSDEF_BOX, SYSDEF_MENU_WINDOW, SYSDEF_MENU_OBJECT, SYSDEF_MENU_TRAY,
		SYSDEF_MENU_CAB, SYSDEF_MENU_DOC, SYSDEF_MENU_FIG
	};
	TS_UUID	u;
	T_OBREF	r;
	INT	i;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_BOX, &u), E_OK);
	if ( ob_ref_obj(&u, &r) < E_OK ) KT_SKIP("no definitions in the store");
	for ( i = 0; i < (INT)( sizeof(ids) / sizeof(ids[0]) ); i++ ) {
		KT_ASSERT_ER(ts_str_to_uuid(ids[i], &u), E_OK);
		KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
		KT_ASSERT_EQ(r.type, OB_T_STORAGE);
	}
}

/* The links of an object's record 0 */
LOCAL INT links_of( CONST char *id, TS_UUID *ids, INT max )
{
	TS_UUID	u;
	UB	*rec;
	SZ	size = 0;
	INT	n;

	if ( ts_str_to_uuid(id, &u) < E_OK || ( rec = om_obj_record(&u, 0, &size) ) == NULL ) {
		return -1;
	}
	n = om_store_links(rec, size, ids, max);
	Kfree(rec);
	return n;
}

/*
 * The system's boxes: each one reached from the システム箱 by a link, and
 * owned by the system
 */
LOCAL void test_system_boxes( void )
{
	CONST char * CONST boxes[] = {
		SYSDEF_BOX, SYSDEF_PROG_BOX, SYSDEF_ACC_BOX, SYSDEF_FONT_BOX,
		SYSDEF_WALL_BOX, SYSDEF_LEARN_BOX, SYSDEF_DICT_BOX
	};
	TS_UUID	ids[16], u;
	T_OBPRT	prt;
	INT	n, i, k;

	n = links_of(SYSDEF_SYSTEM_BOX, ids, 16);
	if ( n < 0 ) KT_SKIP("no system box in the store");
	for ( i = 0; i < (INT)( sizeof(boxes) / sizeof(boxes[0]) ); i++ ) {
		KT_ASSERT_ER(ts_str_to_uuid(boxes[i], &u), E_OK);
		for ( k = 0; k < n && ts_uuid_cmp(&ids[k], &u) != 0; k++ ) ;
		KT_ASSERT(k < n);
		KT_ASSERT_ER(ob_get_prt(&u, &prt), E_OK);
		KT_ASSERT_EQ(ts_uuid_cmp(&prt.owner, &ob_user_system), 0);
	}
}

/*
 * The programs: every link of the program box an object whose metadata
 * says what program it is, and one that runs as a process has its
 * executable in the record its metadata names, of type 9
 */
LOCAL void test_programs( void )
{
	TS_UUID	ids[32];
	T_JSON	root, tf, g, ex;
	T_OBREC	rec[4];
	UB	*meta;
	SZ	size = 0;
	INT	n, i, k, cnt, execs = 0;
	ID	key;

	n = links_of(SYSDEF_PROG_BOX, ids, 32);
	if ( n < 0 ) KT_SKIP("no program box in the store");
	KT_ASSERT(n > 0);
	for ( i = 0; i < n && i < 32; i++ ) {
		meta = om_obj_meta(&ids[i], &size);
		KT_ASSERT(meta != NULL);
		if ( meta == NULL ) continue;
		KT_ASSERT(js_parse(meta, (INT)size, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
			  && js_get(&tf, "program", &g) >= E_OK);
		if ( !js_get_bool(&g, "builtin", FALSE) && js_get(&tf, "exec", &ex) >= E_OK ) {
			D	r = js_get_num(&ex, "record", -1);

			key = ob_opn_obj(&ids[i], OB_OP_R);
			KT_ASSERT(key > 0);
			cnt = 0;
			if ( key > 0 && ob_lst_rec(key, rec, 4, &cnt) >= E_OK ) {
				for ( k = 0; k < cnt && rec[k].recno != r; k++ ) ;
				KT_ASSERT(k < cnt && rec[k].rt == OB_RT_PROG && rec[k].size > 0);
			}
			if ( key > 0 ) ob_cls_obj(key);
			execs++;
		}
		Kfree(meta);
	}
	KT_ASSERT(execs > 0);				/* the clock, at least */
}

/*
 * The conversion dictionary: an object in the 辞書箱, owned by the
 * system. With the converter built in, its data set is record 1, of
 * type 12.
 */
LOCAL void test_dictionary( void )
{
	TS_UUID	ids[8], u;
	T_OBPRT	prt;
	T_OBREC	rec[4];
	INT	n, k, cnt = 0;
	ID	key;

	n = links_of(SYSDEF_DICT_BOX, ids, 8);
	if ( n < 0 ) KT_SKIP("no dictionary box in the store");
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_DICT_MOZC, &u), E_OK);
	for ( k = 0; k < n && ts_uuid_cmp(&ids[k], &u) != 0; k++ ) ;
	KT_ASSERT(k < n);
	KT_ASSERT_ER(ob_get_prt(&u, &prt), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&prt.owner, &ob_user_system), 0);
#if USE_MOZC
	key = ob_opn_obj(&u, OB_OP_R);
	KT_ASSERT(key > 0);
	if ( key > 0 && ob_lst_rec(key, rec, 4, &cnt) >= E_OK ) {
		for ( k = 0; k < cnt && rec[k].recno != 1; k++ ) ;
		KT_ASSERT(k < cnt && rec[k].rt == OB_RT_DICT && rec[k].size > 0);
	}
	if ( key > 0 ) ob_cls_obj(key);
#else
	(void)rec; (void)cnt; (void)key;
#endif
}

EXPORT void ktest_bt2( void )
{
	KT_RUN(test_processes);
	KT_RUN(test_process_core_objects);
	KT_RUN(test_devices);
	KT_RUN(test_devices_added);
	KT_RUN_SCREEN(test_windows);
	KT_RUN(test_tray);
	KT_RUN(test_definitions);
	KT_RUN(test_system_boxes);
	KT_RUN(test_programs);
	KT_RUN(test_dictionary);
}
