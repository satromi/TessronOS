/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	st.h
 *	The system's tools as programs running as processes: what they
 *	share (application/systool/st_list.c, design 11.2.4)
 *
 *	屑実身操作 (tr_main.c), 実身/仮身検索 (sr_main.c), 仮身ネットワーク
 *	(nw_main.c) and ウインドウ一覧 (wl_main.c) are each a program of its
 *	own, with its program object and its executable, made of 仮身一覧's
 *	code (application/fig with the cabinet's ways, FG_TOOL) and of its
 *	own file, which makes the list the window shows. Each list is a
 *	figure of links the program makes (FG_MADE): drawn, pressed, opened
 *	from and carried from as any cabinet's, and nothing in it changed or
 *	saved. The window stands for the tool's program object and opens
 *	where the tool says each time. What only the desktop can do -- throw
 *	an object away for good, count the links again, list the windows of
 *	others -- is asked of it (DT_RQ_PURGE, DT_RQ_RECOUNT, DT_RQ_WIN_LIST),
 *	and the desktop tells the tool when its list is to be made again or
 *	when it is run again while it is open (DT_RQ_TOOL_TELL).
 */

#ifndef __ST_H__
#define __ST_H__

#include "../fig/fg.h"
#include <ts/json.h>
#include <ts/sysdef.h>

/* The panels of the tools' data box (SYSDEF_BOX_SYSTOOL) */
#define ST_PN_TELL	1		/* two lines and 了解 */
#define ST_PN_ASK	2		/* two lines, the button that does nothing and the one that goes ahead */
#define ST_PN_SEARCH	3		/* 実身/仮身検索's question */
#define ST_P_NO		101
#define ST_P_YES	102

/* A tool started: on its program object, its window where the tool says (at_x...) */
IMPORT ER   st_start( T_TAHOST *host, CONST char *prog, T_OBREF *p_ref );

/* A list of objects as the system's lists are laid out, one to a row (tvlist.h) */
#define st_list_record( title, ids, names, n )	tl_record(( title ), ( ids ), ( names ), NULL, ( n ), &tl_rows, NULL)

/* Panels of the tools' data box: told, asked (TRUE for the button that goes ahead) */
#define st_tell( l1, l2 )		ta_tell_in(SYSDEF_BOX_SYSTOOL, ST_PN_TELL, ( l1 ), ( l2 ))
#define st_ask( l1, l2, no, yes )	ta_ask_in(SYSDEF_BOX_SYSTOOL, ST_PN_ASK, ( l1 ), ( l2 ), ( no ), ( yes ))

/* Words */
IMPORT INT  st_cat( UB *buf, INT at, INT max, CONST char *s );
IMPORT INT  st_cat_n( UB *buf, INT at, INT max, INT v );

#endif /* __ST_H__ */
