/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tvlist.h
 *	What the programs that show lists of links share: the list made
 *	into a figure, an object's metadata and name read, and the objects
 *	of the store (lib/libtv/tv_list.c, design 11.2)
 *
 *	仮身一覧 and the programs made of its code -- the 原紙箱, the 小物箱,
 *	原紙集め and the system's tools (屑実身操作, 実身/仮身検索,
 *	仮身ネットワーク, ウインドウ一覧) -- show a figure of links. Those whose
 *	list is no object's record make it here, each with the same closed
 *	links laid out the same way, so that a list looks and is worked the
 *	same in every one of them.
 */

#ifndef __TS_TVLIST_H__
#define __TS_TVLIST_H__

#include <tk/typedef.h>
#include <ts/tad.h>
#include <ts/uuid.h>
#include <ts/json.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How the links of a list are laid out: TL_ROWS one to a row, TL_FLOW
 * from the left of a row as far as 'width' allows (one at least), then
 * on the next row. x, y is the corner of the first; gap is between one
 * link and the next, across and down; chsz the letters of each.
 */
#define TL_ROWS		0
#define TL_FLOW		1

typedef struct {
	UINT	how;			/* TL_ROWS, TL_FLOW */
	INT	x, y;
	INT	gap;
	INT	chsz;
	INT	width;			/* TL_FLOW: the width laid out to */
} T_TLLAY;

/* The system's lists of objects (屑実身操作, 検索結果, the 小物箱): one to a row */
IMPORT CONST T_TLLAY	tl_rows;

/*
 * A page that is a list of objects: one closed link to each of ids[n],
 * laid out as lay says, each as wide as its name -- names[i] when given,
 * else the object's own -- and those dim says (when given) with their
 * frame and name in grey. An object no longer in the store is left out.
 * The rows it came to in *p_rows (NULL: not asked). NULL when there is
 * no memory for it.
 */
IMPORT T_TAD *tl_record( CONST char *title, CONST TS_UUID *ids, CONST UB *CONST *names,
			 CONST BOOL *dim, INT n, CONST T_TLLAY *lay, INT *p_rows );

/* Every object kept on the volumes: how many, the list in *p_ids (free it) */
IMPORT INT  tl_objects( TS_UUID **p_ids );

/*
 * An object's metadata parsed: FALSE when the person may not read it or
 * it cannot be (*p_buf, which root points into, to free when TRUE).
 * tl_name: its name, "" when it cannot be read.
 */
IMPORT BOOL tl_meta( CONST TS_UUID *u, UB **p_buf, T_JSON *root );
IMPORT BOOL tl_name( CONST TS_UUID *u, UB *nm, INT max );

/* Words put into an attribute of a record, with what would end it made safe: where it ends */
IMPORT INT  tl_put_x( char *x, INT at, INT max, CONST UB *s );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TVLIST_H__ */
