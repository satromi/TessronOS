/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tsfscut.c
 *	Work for the power-cut test (design 11.14.6). It runs only when it
 *	is named (make KTONLY=tsfscut): it makes a native volume on vblka0
 *	and changes it without end, saying "PCUT n" after each round, until
 *	tools/tsfs_powercut.py kills the machine at a moment of its choosing
 *	and hands the image to fsck.tsfs.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsobj.h>

#define CUTDEV		kt_scratch()
#define CUT_OBJS	16
#define CUT_BIG		20000

LOCAL UW	cut_seed = 12345;

LOCAL UW cut_rand( void )
{
	cut_seed = cut_seed * 1103515245U + 12345U;
	return cut_seed >> 8;
}

/* A text of `n` links to objects of the list, filled out to `size` */
LOCAL INT cut_doc( UB *out, CONST TS_UUID *to, INT nto, INT n, INT size )
{
	char	u[40];
	TS_UUID	v;
	INT	k = 0, i, m;
	CONST char *p;

	for ( p = "<document><p>"; *p != '\0'; p++ ) out[k++] = (UB)*p;
	for ( i = 0; i < n && nto > 0; i++ ) {
		for ( p = "<link id=\""; *p != '\0'; p++ ) out[k++] = (UB)*p;
		ts_uuid_to_str(&to[cut_rand() % (UW)nto], u, sizeof(u));
		for ( m = 0; u[m] != '\0'; m++ ) out[k++] = (UB)u[m];
		for ( p = "_0.xtad\" vobjid=\""; *p != '\0'; p++ ) out[k++] = (UB)*p;
		ts_gen_uuid(&v);
		ts_uuid_to_str(&v, u, sizeof(u));
		for ( m = 0; u[m] != '\0'; m++ ) out[k++] = (UB)u[m];
		for ( p = "\"/>"; *p != '\0'; p++ ) out[k++] = (UB)*p;
	}
	for ( p = "</p></document>"; *p != '\0'; p++ ) out[k++] = (UB)*p;
	while ( k < size ) out[k++] = (UB)( 'a' + k % 26 );
	return k;
}

EXPORT void ktest_tsfscut( void )
{
	TS_UUID	obj[CUT_OBJS];
	UB	*txt;
	SZ	asize;
	ID	vol;
	INT	n = 0, round, i, len, recno;

	kt_begin("tsfscut");
	txt = (UB *)Kmalloc(CUT_BIG + 8192);
	if ( txt == NULL || ts_format_blk(CUTDEV, "TessronOS cut") < E_OK ) {
		kt_fail(__FILE__, __LINE__, "setup", 0, 0);
		kt_end();
		return;
	}
	vol = ts_obj_mount(CUTDEV);
	tm_printf((UB *)"PCUT READY\n");
	for ( round = 1; vol > 0; round++ ) {
		switch ( cut_rand() % 5 ) {
		case 0:			/* a new document */
		case 1:
			if ( n < CUT_OBJS && ts_obj_create(vol, NULL, 0, &obj[n]) >= E_OK ) {
				if ( ts_obj_apd_rec(vol, &obj[n], TSFS_REC_XTAD, &recno) >= E_OK ) {
					len = cut_doc(txt, obj, n, (INT)( cut_rand() % 4 ), (INT)( cut_rand() % CUT_BIG ));
					ts_obj_wri_rec(vol, &obj[n], 0, 0, txt, len, &asize);
				}
				n++;
			}
			break;
		case 2:			/* one replaced whole */
			if ( n > 0 ) {
				i = (INT)( cut_rand() % (UW)n );
				len = cut_doc(txt, obj, n, (INT)( cut_rand() % 6 ), (INT)( cut_rand() % CUT_BIG ));
				ts_obj_rpl_rec(vol, &obj[i], 0, txt, len);
			}
			break;
		case 3:			/* the links counted again */
			for ( i = 0; i < n; i++ ) {
				ts_obj_relink(vol, &obj[i]);
			}
			break;
		default:		/* one deleted, and the garbage collected */
			if ( n > 0 ) {
				i = (INT)( cut_rand() % (UW)n );
				if ( ts_obj_delete(vol, &obj[i]) >= E_OK ) {
					obj[i] = obj[--n];
				}
				ts_obj_gc_all(vol, NULL);
			}
			break;
		}
		tm_printf((UB *)"PCUT %d\n", round);
	}
	kt_end();
}
