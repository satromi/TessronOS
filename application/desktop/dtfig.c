/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtfig.c
 *	The figure editor (design 17.13)
 *
 *	A figure whose object opens with the figure editor has every shape
 *	in it worked on, not only its virtual objects: taken, carried,
 *	sized, thrown away, copied, put in front or behind, turned over and
 *	turned round. And shapes are drawn into it with the tools of the
 *	道具パネル, a small window beside it: a line, a rectangle with square
 *	or round corners, an ellipse, a line of several segments -- in the
 *	colour of line and of inside chosen in the panel, and the width of
 *	line chosen there.
 *
 *	What is picked, carried and sized is dtedit.c's, the same code a
 *	cabinet uses; what is done to a shape that is not a virtual object
 *	is the figure editor's code (application/fig/figedit.c), called here
 *	over the window's record, model and picks.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dbox.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/cabmenu.h>
#include <ts/mn.h>
#include <ts/sysdef.h>
#include <ts/fn.h>
#include <ts/look.h>
#include <ts/figedit.h>
#include "desktop.h"

#define KEY_ENTER	0x28
#define KEY_ESC		0x29

/* ---------------------------------------------------------------- over a window */

/*
 * What the editor works on in a window, for the figure editor's code
 * (application/fig/figedit.c): the window's record, model and picks. The
 * window keeps them; each call of the table hands the picks the code
 * counted back to the window first, does what the desktop does, and
 * takes the window's as they are after.
 */
LOCAL CONST T_FGHOST	fe_desk;

LOCAL void fe_of( DTWIN *d, T_FGED *e )
{
	e->rec = d->rec;
	e->fig = d->fig;
	e->pick = d->pick;
	e->npick = d->npick;
	e->usable = (BOOL)( d->figed && !d->sealed );
	e->host = &fe_desk;
	e->arg = d;
}

/* What the code counted handed back to the window */
LOCAL void fe_back( T_FGED *e )
{
	( (DTWIN *)e->arg )->npick = e->npick;
}

LOCAL void h_before( T_FGED *e )
{
	fe_back(e);
	ed_before((DTWIN *)e->arg);
	fe_of((DTWIN *)e->arg, e);
}

LOCAL void h_changed( T_FGED *e )
{
	fe_back(e);
	ed_changed((DTWIN *)e->arg);
	fe_of((DTWIN *)e->arg, e);
}

LOCAL void h_model( T_FGED *e )
{
	fe_back(e);
	ed_model((DTWIN *)e->arg);
	fe_of((DTWIN *)e->arg, e);
}

LOCAL void h_pick_none( T_FGED *e )
{
	fe_back(e);
	ed_pick_none((DTWIN *)e->arg);
	fe_of((DTWIN *)e->arg, e);
}

LOCAL void h_pick_add( T_FGED *e, INT i )
{
	fe_back(e);
	ed_pick_add((DTWIN *)e->arg, i);
	fe_of((DTWIN *)e->arg, e);
}

LOCAL INT h_first_pick( T_FGED *e )
{
	fe_back(e);
	return ed_first_pick((DTWIN *)e->arg);
}

LOCAL void h_no_memory( T_FGED *e )
{
	dt_no_memory((DTWIN *)e->arg);
}

LOCAL BOOL h_align_form( T_FGED *e, INT *p_h, INT *p_v )
{
	return dt_align_form((DTWIN *)e->arg, p_h, p_v);
}

LOCAL CONST T_FGHOST	fe_desk = {
	h_before, h_changed, h_model, h_pick_none, h_pick_add, h_first_pick,
	h_no_memory, h_align_form
};

/* The figure editor's state over a window, and what the code counted handed back (figtool.c's views) */
EXPORT void df_ed_of( DTWIN *d, T_FGED *e )
{
	fe_of(d, e);
}

EXPORT void df_ed_back( T_FGED *e )
{
	fe_back(e);
}

EXPORT void df_attr_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT v )
{
	fe_attr_put(rec, nd, name, v);
}

EXPORT void df_points_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPPOINT *pt,
			   INT n )
{
	fe_points_put(rec, nd, pt, n);
}

EXPORT BOOL df_is( CONST DTWIN *d )
{
	return (BOOL)( d != NULL && d->figed && d->fig != NULL && !d->sealed );
}

EXPORT BOOL df_takeable( CONST DTWIN *d, INT i )
{
	T_FGED	e;

	if ( d == NULL ) return FALSE;
	fe_of((DTWIN *)d, &e);
	return fe_takeable(&e, i);
}

/* One call of the figure editor's code over a window, the picks handed back after */
#define FE_OVER(d, call)	do { T_FGED e_; fe_of((d), &e_); call; fe_back(&e_); } while ( 0 )

EXPORT void df_whole_groups( DTWIN *d )		{ FE_OVER(d, fe_whole_groups(&e_)); }
EXPORT void df_shift( DTWIN *d, INT i, INT dx, INT dy )	{ FE_OVER(d, fe_shift(&e_, i, dx, dy)); }
EXPORT void df_resize( DTWIN *d, INT i, CONST T_DPRECT *nb )	{ FE_OVER(d, fe_resize(&e_, i, nb)); }
EXPORT void df_resize_flip( DTWIN *d, INT i, CONST T_DPRECT *nb, UINT flip )	{ FE_OVER(d, fe_resize_flip(&e_, i, nb, flip)); }
EXPORT void df_delete_picked( DTWIN *d )	{ FE_OVER(d, fe_delete_picked(&e_)); }
EXPORT void df_turn( DTWIN *d, INT how )	{ FE_OVER(d, fe_turn(&e_, how)); }
EXPORT void df_order( DTWIN *d, INT how )	{ FE_OVER(d, fe_order(&e_, how)); }
EXPORT void df_dup_picked( DTWIN *d )		{ FE_OVER(d, fe_dup_picked(&e_)); }
EXPORT void df_adopt( DTWIN *d, T_TADNODE *nd )	{ FE_OVER(d, fe_adopt(&e_, nd)); }
EXPORT void df_pick_pasted( DTWIN *d )		{ FE_OVER(d, fe_pick_pasted(&e_)); }
LOCAL void group_picked( DTWIN *d )		{ FE_OVER(d, fe_group(&e_)); }
LOCAL void ungroup_picked( DTWIN *d )		{ FE_OVER(d, fe_ungroup(&e_)); }
LOCAL void align_picked( DTWIN *d )		{ FE_OVER(d, fe_align(&e_)); }
LOCAL void invert_picked( DTWIN *d )		{ FE_OVER(d, fe_invert(&e_)); }

EXPORT INT df_picked( CONST DTWIN *d )
{
	T_FGED	e;

	fe_of((DTWIN *)d, &e);
	return fe_picked(&e);
}

EXPORT INT df_copy_into( DTWIN *d, T_TAD *frag )
{
	INT	n = 0;

	FE_OVER(d, n = fe_copy_into(&e_, frag));
	return n;
}

EXPORT INT df_frag_into( T_TAD *rec, T_TADNODE *parent, CONST T_TAD *frag )
{
	return fe_frag_into(rec, parent, frag);
}

EXPORT INT df_paste_frag( DTWIN *d, CONST T_TAD *frag, INT off )
{
	INT	n = 0;

	FE_OVER(d, n = fe_paste_frag(&e_, frag, off));
	return n;
}

#define TS_FLIP_H	FE_FLIP_H
#define TS_FLIP_V	FE_FLIP_V
#define TS_ROT_L	FE_ROT_L
#define TS_ROT_R	FE_ROT_R
#define TO_FRONT	FE_TO_FRONT
#define TO_BACK		FE_TO_BACK
#define TO_FORWARD	FE_TO_FORWARD
#define TO_BACKWARD	FE_TO_BACKWARD

/* ---------------------------------------------------------------- the menu */

/*
 * The figure editor's own commands. The items it shares with the
 * cabinet (saving, the view, the clipboard, protection) are the
 * cabinet's commands and are done by its code.
 */
#define FC_PANEL	601
#define FC_ALL		602
#define FC_FRONT	603
#define FC_BACK		604
#define FC_FWD		605
#define FC_BWD		606
#define FC_FLIP_H	607
#define FC_FLIP_V	608
#define FC_ROT_L	609
#define FC_ROT_R	610
#define FC_GROUP	611
#define FC_UNGROUP	612
#define FC_ALIGN	613
#define FC_INVERT	614
#define FC_REALSIZE	615
#define FC_PATTERN	616
#define FC_TRANSFORM	617
#define FC_BURN		618
#define FC_PAPER	619
#define FC_PAGE_SETUP	620
#define FC_PRINT	621

typedef struct {
	CONST char	*code;
	INT		cmd;
} FIGCMD;

LOCAL CONST FIGCMD fig_cmds[] = {
	{ "panel",	FC_PANEL },
	{ "realsize",	FC_REALSIZE },
	{ "pattern",	FC_PATTERN },
	{ "group",	FC_GROUP },
	{ "ungroup",	FC_UNGROUP },
	{ "selectall",	FC_ALL },
	{ "front",	FC_FRONT },
	{ "back",	FC_BACK },
	{ "forward",	FC_FWD },
	{ "backward",	FC_BWD },
	{ "align",	FC_ALIGN },
	{ "transform",	FC_TRANSFORM },
	{ "invert",	FC_INVERT },
	{ "burn",	FC_BURN },
	{ "flip.h",	FC_FLIP_H },
	{ "flip.v",	FC_FLIP_V },
	{ "rot.l",	FC_ROT_L },
	{ "rot.r",	FC_ROT_R },
	{ "paper",	FC_PAPER },
	{ "pagesetup",	FC_PAGE_SETUP },
	{ "print",	FC_PRINT },
};

#define NFIGCMD	( (INT)( sizeof(fig_cmds) / sizeof(fig_cmds[0]) ) )

/* The items that act on what is taken, grey when nothing is */
LOCAL CONST char * CONST fig_on_pick[] = {
	"copy", "cut", "delete", "ungroup", "front", "back", "forward", "backward",
	"transform", "invert", "burn", "flip.h", "flip.v", "rot.l", "rot.r"
};

/* The figure editor's menu (FIGMENU.DEF), set to what the window is */
EXPORT ER df_menu_make( DTWIN *d, ID *p_mid )
{
	TS_UUID	def;
	BOOL	none = (BOOL)( d->npick == 0 );
	ID	mid;
	ER	er;
	INT	i;

	er = ts_str_to_uuid(SYSDEF_MENU_FIG, &def);
	if ( er >= E_OK ) {
		er = mn_cre_men(&def, &mid);
	}
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < (INT)( sizeof(fig_on_pick) / sizeof(fig_on_pick[0]) ); i++ ) {
		(void)mn_chg_atr(mid, NULL, fig_on_pick[i], none ? MN_GREY : 0);
	}
	/* 保護: by what it can change of the links taken */
	i = d->sealed ? 0 : (INT)fe_prot_can(d->rec, d->fig, d->pick);
	fe_prot_grey(mid, (UINT)i);
#ifdef USE_UITEST
	tm_printf((UB *)"desktop: protect menu %x\n", (UINT)i);	/* for the tests */
#endif
	(void)mn_chg_atr(mid, NULL, "undo", ( d->nundo == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "group", ( d->npick < 2 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "align", ( d->npick < 2 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "save.new", d->has_parent ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "hidden", d->show_hidden ? MN_TICK : 0);
	i = ( !d->sealed && dt_tray_kind(NULL) != DT_TRAY_NONE ) ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "paste", (UINT)i);
	(void)mn_chg_atr(mid, NULL, "moveback", (UINT)i);
	*p_mid = mid;

	return E_OK;
}

EXPORT void df_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			CONST T_VOBJ *v )
{
	INT	i, cmd = 0;

	for ( i = 0; i < NFIGCMD && cmd == 0; i++ ) {
		if ( mn_is(sel, fig_cmds[i].code) ) {
			cmd = fig_cmds[i].cmd;
		}
	}
	switch ( cmd ) {
	case FC_PANEL:		df_panel_toggle(d);		break;
	case FC_ALL:
		ed_pick_all(d);
		dt_draw(d);
		break;
	case FC_FRONT:		df_order(d, TO_FRONT);		break;
	case FC_BACK:		df_order(d, TO_BACK);		break;
	case FC_FWD:		df_order(d, TO_FORWARD);	break;
	case FC_BWD:		df_order(d, TO_BACKWARD);	break;
	case FC_FLIP_H:		df_turn(d, TS_FLIP_H);		break;
	case FC_FLIP_V:		df_turn(d, TS_FLIP_V);		break;
	case FC_ROT_L:		df_turn(d, TS_ROT_L);		break;
	case FC_ROT_R:		df_turn(d, TS_ROT_R);		break;
	case FC_GROUP:		group_picked(d);		break;
	case FC_UNGROUP:	ungroup_picked(d);		break;
	case FC_ALIGN:		align_picked(d);		break;
	case FC_INVERT:		invert_picked(d);		break;
	case FC_REALSIZE:	df_realsize_toggle(d);		break;
	case FC_PATTERN:	df_pattern_edit(d, df_fill_pattern());	break;
	case FC_TRANSFORM:	df_transform(d);		break;
	case FC_BURN:		df_burn(d);			break;
	case FC_PAPER:		df_paper_toggle(d);		break;
	case FC_PAGE_SETUP:	df_page_setup(d);		break;
	case FC_PRINT:		dt_print_window(d);		break;
	default:
		/* the items shared with the cabinet */
		cmd = cab_menu_cmd(sel);
		if ( cmd != 0 ) {
			dt_cab_command(d, cmd, on_vobj, v);
		}
		break;
	}
	wm_update();
}

EXPORT void df_front( DTWIN *d, BOOL front )
{
	df_order(d, front ? TO_FRONT : TO_BACK);
}
