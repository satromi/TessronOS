/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dc.h
 *	基本文章編集 as a program running as a process: what its parts share
 *	(application/doc/dc_main.c, dc_host.c, design 11.3)
 *
 *	The document is record g_recno of the object opened, read and parsed
 *	into the program's memory (g_rec, tvapp.h) and made into a model
 *	(g_doc) to draw and to ask where things are. What is done to it is
 *	done by the editor of text (application/doc/docedit.c) on g_de, whose
 *	table of what it asks of the one showing the text is dc_host.c's.
 */

#ifndef __DC_H__
#define __DC_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/ui.h>
#include <ts/dtreq.h>
#include <ts/docedit.h>
#include <ts/tvapp.h>

/* The panels of the program's data box (SYSDEF_BOX_DOC) */
#define PN_TELL		2		/* what could not be done, two lines */

#define DC_PAPER_W	DE_PAPER_W	/* A4, when a document names no paper */
#define DC_PAPER_H	DE_PAPER_H

/* ---------------------------------------------------------------- what it holds (dc_main.c) */

extern T_DDED	g_de;			/* the editor's, over what is below */
extern T_TVDOC	*g_doc;			/* the model made from g_rec, or from g_xrec in 原稿 */
extern T_TAD	*g_xrec;		/* 原稿: the record as text */
extern INT	g_sx, g_sy;		/* where it is scrolled to */
extern INT	g_height;		/* how tall (縦書き: how wide) the text came to */
extern UINT	g_kind;
extern BOOL	g_xml_view, g_detail_view, g_sealed, g_used, g_settling;
extern BOOL	g_nowrap;		/* not wrapped to the window: to the paper */
extern BOOL	g_paper_frame;		/* 用紙枠 */
extern BOOL	g_show_hidden;
extern BOOL	g_caret_hidden;		/* the caret's turn to be hidden, when it blinks */
extern BOOL	g_banded;		/* a part of the work area to be drawn again, g_band, */
extern T_DPRECT	g_band;			/* when the whole is not (g_dirty) */
void dc_band( CONST T_DPRECT *r );	/* that part made to hold r too */

IMPORT void tell( CONST char *l1, CONST char *l2 );
IMPORT void dc_model( void );
IMPORT void dc_work_rect( T_DPRECT *r );
IMPORT INT  dc_look( UINT num, INT dflt );

/* ---------------------------------------------------------------- taking back (dc_host.c) */

IMPORT void dc_before( void );
IMPORT BOOL dc_undo( void );
IMPORT INT  dc_undos( void );
IMPORT void dc_undo_clear( void );
IMPORT T_TAD *dc_tray_frag( void );	/* the set in hand, freed by the caller; NULL none */

IMPORT CONST T_DDHOST	dc_host;

#endif /* __DC_H__ */
