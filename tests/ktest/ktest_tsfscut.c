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
 *	Work for the power-cut test (design 11.14.6, 7.3). It runs only when
 *	it is named (make KTONLY=tsfscut): it makes a native volume on the
 *	scratch partition and changes it without end, saying "PCUT n" after
 *	each round, until tools/tsfs_powercut.py kills the machine at a
 *	moment of its choosing and hands the image to fsck.tsfs.
 *
 *	Besides documents written, replaced, relinked and deleted, a round
 *	may write a large metadata text, or build an object whose records
 *	lie in many runs (two records grown a block at a time in turn) and
 *	then replace, truncate or delete one of them. On a volume of version
 *	5 the budget of a transaction is made small (TSFSJ_FAULT_BUDGET), so
 *	that these operations are split over many transactions with the
 *	spare place (OB_SPARE) between them, and a cut falls between them.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>
#include <ts/tsfsobj.h>

#define CUTDEV		kt_scratch()
#define CUT_OBJS	16
#define CUT_BIG		20000
#define CUT_BUDGET	16		/* copies of a transaction on a volume of version 5 */
#define CUT_META5	( 600 * 1024 )	/* a metadata text past what version 4 takes */
#define CUT_META4	( 96 * 1024 )
#define CUT_PIECES	200		/* blocks each of the two interleaved records gets */
#define CUT_PIECE	4096

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
	for ( ; k < size; k++ ) out[k] = (UB)( 'a' + k % 26 );
	return k;
}

/* A metadata text of `size` bytes: a name and a long member */
LOCAL INT cut_meta( UB *out, INT size )
{
	CONST char *p;
	INT	k = 0;

	for ( p = "{\"name\":\"cut\",\"blob\":\""; *p != '\0'; p++ ) out[k++] = (UB)*p;
	for ( ; k < size - 2; k++ ) out[k] = (UB)( 'A' + k % 26 );
	out[k++] = '"';
	out[k++] = '}';
	return k;
}

/*
 * Two records grown a block at a time in turn, so that each lies in
 * many runs (a run tree on version 5, a tree of two levels on 4).
 */
LOCAL void cut_runs( ID vol, CONST TS_UUID *u, UB *buf )
{
	INT	r0, r1, i;
	SZ	asize;

	if ( ts_obj_apd_rec(vol, u, TSFS_REC_BIN, &r0) < E_OK ) return;
	if ( ts_obj_apd_rec(vol, u, TSFS_REC_BIN, &r1) < E_OK ) return;
	for ( i = 0; i < CUT_PIECES; i++ ) {
		knl_memset(buf, (INT)( i & 0xFF ), CUT_PIECE);
		if ( ts_obj_wri_rec(vol, u, r0, (D)i * CUT_PIECE, buf, CUT_PIECE, &asize) < E_OK ) return;
		if ( ts_obj_wri_rec(vol, u, r1, (D)i * CUT_PIECE, buf, CUT_PIECE, &asize) < E_OK ) return;
	}
}

EXPORT void ktest_tsfscut( void )
{
	TS_UUID	obj[CUT_OBJS];
	T_TSFSLIM lim;
	UB	*txt;
	SZ	asize;
	ID	vol;
	BOOL	v5 = FALSE;
	INT	n = 0, round, i, len, recno, metamax;

	kt_begin("tsfscut");
	txt = (UB *)Kmalloc(CUT_META5 + 8192);
	if ( txt == NULL || ts_format_blk(CUTDEV, "TessronOS cut") < E_OK ) {
		kt_fail(__FILE__, __LINE__, "setup", 0, 0);
		kt_end();
		return;
	}
	vol = ts_obj_mount(CUTDEV);
	if ( vol > 0 && ts_blk_lim(vol, &lim) >= E_OK && lim.max_rec == 0 ) {
		v5 = TRUE;		/* no limit of the format: version 5 */
		(void)ts_jrnl_fault(vol, TSFSJ_FAULT_BUDGET | CUT_BUDGET);
	}
	metamax = v5 ? CUT_META5 : CUT_META4;
	tm_printf((UB *)"PCUT READY v%d\n", v5 ? 5 : 4);
	for ( round = 1; vol > 0; round++ ) {
		switch ( cut_rand() % 9 ) {
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
		case 4:			/* a large metadata text */
			if ( n > 0 ) {
				i = (INT)( cut_rand() % (UW)n );
				len = cut_meta(txt, metamax / 2 + (INT)( cut_rand() % (UW)( metamax / 2 ) ));
				ts_obj_set_meta(vol, &obj[i], txt, (SZ)len);
			}
			break;
		case 5:			/* records in many runs */
			if ( n < CUT_OBJS && ts_obj_create(vol, NULL, 0, &obj[n]) >= E_OK ) {
				cut_runs(vol, &obj[n], txt);
				n++;
			}
			break;
		case 6:			/* a record of many runs replaced, or truncated */
			if ( n > 0 ) {
				i = (INT)( cut_rand() % (UW)n );
				if ( cut_rand() % 2 == 0 ) {
					len = (INT)( cut_rand() % CUT_BIG );
					knl_memset(txt, 'r', (SZ)len);
					ts_obj_rpl_rec(vol, &obj[i], 0, txt, len);
				} else {
					ts_obj_trn_rec(vol, &obj[i], 0, (UD)( cut_rand() % CUT_PIECE ));
				}
			}
			break;
		case 7:			/* a record deleted */
			if ( n > 0 ) {
				i = (INT)( cut_rand() % (UW)n );
				ts_obj_del_rec(vol, &obj[i], 0);
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
