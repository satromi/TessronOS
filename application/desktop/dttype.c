/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dttype.c
 *	Letters a program types into the window the keys went to before
 *	(DT_RQ_TYPE of ts/dtreq.h, docs/tessronos/11-desktop.md 11.19)
 *
 *	A program that offers letters to be typed -- 文字検索 -- has the
 *	keys itself while it is worked, so the window meant is the one that
 *	had them before it. The desktop keeps the last few windows the keys
 *	went to, newest first, and types into the newest that is still there
 *	and is not the asking program's own:
 *
 *	  a window with a panel whose box has the keys: into the box at its
 *	  caret, as the panel's own conversion commits (wm_panel_type)
 *
 *	Any other window -- the desktop's own windows hold no text to type
 *	into -- answers E_NOEXS, and the program does what it does instead
 *	(文字検索 puts the letters in the tray).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include "desktop.h"

#define DT_KEYS_SEEN	4

LOCAL INT	dt_keys_seen[DT_KEYS_SEEN];	/* the windows the keys went to, newest first */

EXPORT void dt_type_poll( void )
{
	INT	f = wm_focused(), i, k;

	if ( f <= 0 || f == dt_keys_seen[0] ) return;
	for ( i = 0; i < DT_KEYS_SEEN - 1 && dt_keys_seen[i] != f; i++ ) ;
	for ( k = i; k > 0; k-- ) dt_keys_seen[k] = dt_keys_seen[k - 1];
	dt_keys_seen[0] = f;
}

/* The window to type into for the window asker: the newest the keys went to but it */
LOCAL INT type_target( INT asker )
{
	T_WMWIN	w;
	INT	i;

	dt_type_poll();
	for ( i = 0; i < DT_KEYS_SEEN; i++ ) {
		INT	wid = dt_keys_seen[i];

		if ( wid > 0 && wid != asker && wm_ref(wid, &w) >= E_OK ) return wid;
	}
	return 0;
}

EXPORT ER dt_type_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	SZ		n;
	CONST UB	*s = dt_rq_word(rq, DT_TAIL_NAME, &n);
	INT		wid;

	if ( n == 0 ) return E_PAR;
	wid = type_target(wm_obj_wid(&rq->win));
	if ( wid <= 0 ) return E_NOEXS;
	if ( dt_win_of(wid) == NULL && wm_panel_type(wid, s, (INT)n) ) {
		wm_update();
		an->how = DT_HOW_BOX;
		tm_printf((UB *)"desktop: typed %s into a box of window %d\n", s, wid);
		return E_OK;
	}
	return E_NOEXS;
}
