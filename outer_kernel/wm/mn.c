/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mn.c
 *	Menu manager: menus made from menu definitions (design 18.15)
 *
 *	A definition is a real object whose record 0 is xmlTAD with a <menu>
 *	element: <item> (its text the words, a <link> in it the definition
 *	that opens beside it), <line/>, <list code=""/> for what the program
 *	fills, and <include><link/></include> for another definition's items
 *	in its place.
 *
 *	A menu is a set of lists (nodes). The first is the one that opens;
 *	an item that opens another list names that node. Reading a
 *	definition fills a node with its items, puts the items of a
 *	definition it takes in in their place, and reads the definition an
 *	item opens into a node of its own. Each item
 *	remembers the definition it came from, so an item taken in from the
 *	system's definitions is still the system's when it is chosen.
 *
 *	Shown, the nodes become the window layer's menu lists (part.h):
 *	each is put in the data box under a number of this menu's own, and
 *	an item's command says which node and which item it is.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/ob.h>
#include <ts/part.h>
#include <ts/dbox.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/mn.h>
#include <ts/wm.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>

#define MN_DEPTH	6		/* definitions within definitions */

/* Where the lists of menu m are kept, and what an item's command is */
#define MN_LIST(m, k)	( 0x4000 + (m) * MN_NODE_MAX + (k) )
#define MN_CMD_BASE	0x100000
#define MN_CMD(k, i)	( MN_CMD_BASE + ( (k) << 8 ) + (i) + 1 )

/* An item's own marks, besides its state (MN_GREY, MN_TICK, MN_HIDE) */
#define MN_LINE		0x0100		/* a line between groups */
#define MN_LIST_HOLE	0x0200		/* where the program's list goes */

typedef struct {
	TS_UUID	def;
	UB	code[MN_CODE_MAX];
	UB	label[WM_LABEL_MAX];
	UB	key[WM_KEY_MAX];
	INT	sub;			/* the node it opens, or -1 */
	UINT	atr;
	INT	index;
	D	value;
} MNITEM;

typedef struct {
	INT	nitem;
	MNITEM	item[WM_ITEM_MAX];
} MNNODE;

typedef struct {
	BOOL	used;
	TS_UUID	def;			/* the definition the menu was made from */
	INT	nnode;
	MNNODE	*node[MN_NODE_MAX];
	INT	row[WM_ITEM_MAX];	/* the row each root item was shown on, or -1 */
} MNMENU;

LOCAL MNMENU	mn_tab[MN_MAX];

/*
 * The item each menu was last left on, so that it opens with that item
 * under the pointer again (part.h, T_WMMENU.cur).
 */
typedef struct {
	TS_UUID	def;
	INT	cur;
} MNLAST;

LOCAL MNLAST	mn_last[MN_MAX];
LOCAL INT	mn_nlast = 0;

/* ---------------------------------------------------------------- the table */

LOCAL MNMENU *menu_of( ID mid )
{
	if ( mid <= 0 || mid > MN_MAX || !mn_tab[mid - 1].used ) {
		return NULL;
	}
	return &mn_tab[mid - 1];
}

/* The table of menus, taken from tasks on every processor */
LOCAL T_SPLOCK	mn_tab_lock;

LOCAL ID menu_take( void )
{
	ID	mid = E_LIMIT;
	INT	i;
	UINT	imask;

	ISpinLock(&mn_tab_lock, &imask);
	for ( i = 0; i < MN_MAX; i++ ) {
		if ( !mn_tab[i].used ) {
			knl_memset(&mn_tab[i], 0, sizeof(MNMENU));
			mn_tab[i].used = TRUE;
			mid = i + 1;
			break;
		}
	}
	ISpinUnlock(&mn_tab_lock, &imask);

	return mid;
}

LOCAL INT str_cpy( UB *dst, INT max, CONST UB *src )
{
	INT	i;

	for ( i = 0; src[i] != 0 && i < max - 1; i++ ) {
		dst[i] = src[i];
	}
	dst[i] = 0;
	return i;
}

LOCAL BOOL str_same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

/* ---------------------------------------------------------------- reading */

LOCAL BOOL name_is( CONST T_TADNODE *nd, CONST char *name )
{
	return (BOOL)( nd->kind == TAD_ND_ELEM && nd->name != NULL
		       && str_same(nd->name, name) );
}

/* The object a <link> inside an element points at: its id is "{uuid}_0.xtad" */
LOCAL BOOL link_in( CONST T_TADNODE *nd, TS_UUID *p_uuid )
{
	CONST T_TADNODE	*c;
	CONST UB	*id;
	char		s[TS_UUID_STRLEN + 1];
	INT		i;

	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( !name_is(c, "link") || ( id = tad_attr(c, "id") ) == NULL ) {
			continue;
		}
		for ( i = 0; i < TS_UUID_STRLEN && id[i] != 0; i++ ) {
			s[i] = (char)id[i];
		}
		s[i] = 0;
		return (BOOL)( i == TS_UUID_STRLEN && ts_str_to_uuid(s, p_uuid) >= E_OK );
	}
	return FALSE;
}

/* An attribute copied out, or an empty string */
LOCAL void attr_of( CONST T_TADNODE *nd, CONST char *name, UB *out, INT max )
{
	CONST UB *v = tad_attr(nd, name);

	out[0] = 0;
	if ( v != NULL ) {
		(void)str_cpy(out, max, v);
	}
}

LOCAL BOOL attr_true( CONST T_TADNODE *nd, CONST char *name )
{
	CONST UB *v = tad_attr(nd, name);

	return (BOOL)( v != NULL && str_same(v, "true") );
}

LOCAL D attr_num( CONST T_TADNODE *nd, CONST char *name )
{
	CONST UB *v = tad_attr(nd, name);
	D	x = 0;
	BOOL	neg = FALSE;

	if ( v == NULL ) {
		return 0;
	}
	if ( *v == '-' ) {
		neg = TRUE;
		v++;
	}
	while ( *v >= '0' && *v <= '9' ) {
		x = x * 10 + ( *v++ - '0' );
	}
	return neg ? -x : x;
}

/* The words of an item: its text, the <link> in it left out */
LOCAL void label_of( CONST T_TADNODE *nd, UB *out, INT max )
{
	CONST T_TADNODE	*c;
	INT		n = 0;

	for ( c = nd->first; c != NULL && n < max - 1; c = c->next ) {
		if ( c->kind == TAD_ND_TEXT && c->text != NULL ) {
			n += str_cpy(out + n, max - n, c->text);
		}
	}
	out[n] = 0;
}

LOCAL INT node_new( MNMENU *m )
{
	MNNODE	*n;

	if ( m->nnode >= MN_NODE_MAX ) {
		return -1;
	}
	n = (MNNODE *)Kmalloc(sizeof(MNNODE));
	if ( n == NULL ) {
		return -1;
	}
	knl_memset(n, 0, sizeof(MNNODE));
	m->node[m->nnode] = n;

	return m->nnode++;
}

LOCAL ER read_def( MNMENU *m, INT k, CONST TS_UUID *def, INT depth );

/* One element of a definition's <menu> into node k */
LOCAL ER read_item( MNMENU *m, INT k, CONST TS_UUID *def, CONST T_TADNODE *it,
		    INT depth )
{
	MNNODE	*n = m->node[k];
	MNITEM	*e;
	TS_UUID	u;
	INT	sub;

	if ( it->kind != TAD_ND_ELEM ) {
		return E_OK;			/* the blanks between the elements */
	}
	/* another definition's items, here in this one's */
	if ( name_is(it, "include") ) {
		return link_in(it, &u) ? read_def(m, k, &u, depth + 1) : E_OBJ;
	}
	if ( !name_is(it, "item") && !name_is(it, "line") && !name_is(it, "list") ) {
		return E_OK;			/* what this reader does not know */
	}
	if ( n->nitem >= WM_ITEM_MAX ) {
		return E_LIMIT;
	}
	e = &n->item[n->nitem++];
	knl_memset(e, 0, sizeof(MNITEM));
	e->def = *def;
	e->sub = -1;
	if ( name_is(it, "line") ) {
		e->atr = MN_LINE;
		return E_OK;
	}
	attr_of(it, "code", e->code, MN_CODE_MAX);
	if ( name_is(it, "list") ) {
		e->atr = MN_LIST_HOLE | MN_HIDE;
		return E_OK;
	}
	label_of(it, e->label, WM_LABEL_MAX);
	attr_of(it, "key", e->key, WM_KEY_MAX);
	e->value = attr_num(it, "value");
	if ( attr_true(it, "grey") ) e->atr |= MN_GREY;
	if ( attr_true(it, "tick") ) e->atr |= MN_TICK;

	/* the definition it opens beside it */
	if ( link_in(it, &u) ) {
		sub = node_new(m);
		if ( sub < 0 ) {
			return E_LIMIT;
		}
		e->sub = sub;
		return read_def(m, sub, &u, depth + 1);
	}
	return E_OK;
}

/* A definition's items (the <menu> of its record 0), added to node k */
LOCAL ER read_def( MNMENU *m, INT k, CONST TS_UUID *def, INT depth )
{
	CONST T_TADNODE	*menu = NULL, *c;
	T_TAD		*doc = NULL;
	UB		*rec;
	SZ		size = 0;
	ER		er;

	if ( depth > MN_DEPTH ) {
		return E_LIMIT;			/* a definition that takes itself in */
	}
	rec = om_obj_record(def, 0, &size);
	if ( rec == NULL ) {
		return E_NOEXS;
	}
	er = tad_parse(rec, size, NULL, &doc);
	if ( er >= E_OK ) {
		for ( c = tad_root(doc)->first; c != NULL && menu == NULL; c = c->next ) {
			if ( name_is(c, "menu") ) {
				menu = c;
			}
		}
		er = ( menu != NULL ) ? E_OK : E_OBJ;
	}
	for ( c = ( menu != NULL ) ? menu->first : NULL; er >= E_OK && c != NULL; c = c->next ) {
		er = read_item(m, k, def, c, depth);
	}
	if ( doc != NULL ) tad_free(doc);
	Kfree(rec);

	return er;
}

EXPORT ER mn_cre_men( CONST TS_UUID *def, ID *p_mid )
{
	MNMENU	*m;
	ID	mid;
	ER	er;

	if ( def == NULL || p_mid == NULL ) {
		return E_PAR;
	}
	mid = menu_take();
	if ( mid < 0 ) {
		return (ER)mid;
	}
	m = &mn_tab[mid - 1];
	m->def = *def;
	er = ( node_new(m) == 0 ) ? read_def(m, 0, def, 0) : E_NOMEM;
	if ( er < E_OK ) {
		(void)mn_del_men(mid);
		return er;
	}
	*p_mid = mid;

	return E_OK;
}

EXPORT ER mn_del_men( ID mid )
{
	MNMENU	*m = menu_of(mid);
	INT	k;

	if ( m == NULL ) {
		return E_ID;
	}
	for ( k = 0; k < m->nnode; k++ ) {
		(void)db_del(DB_MENU, MN_LIST(mid - 1, k), 0);
		Kfree(m->node[k]);
	}
	m->used = FALSE;

	return E_OK;
}

/* ---------------------------------------------------------------- state */

LOCAL BOOL item_is( CONST MNITEM *e, CONST TS_UUID *def, CONST char *code )
{
	if ( ( e->atr & MN_LINE ) != 0 || !str_same(e->code, code) ) {
		return FALSE;
	}
	return (BOOL)( def == NULL || ts_uuid_cmp(&e->def, def) == 0 );
}

EXPORT INT mn_chg_atr( ID mid, CONST TS_UUID *def, CONST char *code, UINT atr )
{
	MNMENU	*m = menu_of(mid);
	INT	k, i, n = 0;

	if ( m == NULL ) {
		return E_ID;
	}
	if ( code == NULL ) {
		return E_PAR;
	}
	for ( k = 0; k < m->nnode; k++ ) {
		for ( i = 0; i < m->node[k]->nitem; i++ ) {
			MNITEM	*e = &m->node[k]->item[i];

			if ( item_is(e, def, code) && ( e->atr & MN_LIST_HOLE ) == 0 ) {
				e->atr = ( e->atr & ~( MN_GREY | MN_TICK | MN_HIDE ) )
				       | ( atr & ( MN_GREY | MN_TICK | MN_HIDE ) );
				n++;
			}
		}
	}
	return n;
}

EXPORT INT mn_get_atr( ID mid, CONST TS_UUID *def, CONST char *code )
{
	MNMENU	*m = menu_of(mid);
	INT	k, i;

	if ( m == NULL ) {
		return E_ID;
	}
	if ( code == NULL ) {
		return E_PAR;
	}
	for ( k = 0; k < m->nnode; k++ ) {
		for ( i = 0; i < m->node[k]->nitem; i++ ) {
			MNITEM	*e = &m->node[k]->item[i];

			if ( item_is(e, def, code) && ( e->atr & MN_LIST_HOLE ) == 0 ) {
				return (INT)( e->atr & ( MN_GREY | MN_TICK | MN_HIDE ) );
			}
		}
	}
	return E_NOEXS;
}

EXPORT ER mn_chg_idx( ID mid, CONST TS_UUID *def, CONST char *code, INT index,
		      UINT atr )
{
	MNMENU	*m = menu_of(mid);
	INT	k, i;

	if ( m == NULL ) {
		return E_ID;
	}
	if ( code == NULL ) {
		return E_PAR;
	}
	for ( k = 0; k < m->nnode; k++ ) {
		for ( i = 0; i < m->node[k]->nitem; i++ ) {
			MNITEM	*e = &m->node[k]->item[i];

			if ( item_is(e, def, code) && ( e->atr & MN_LIST_HOLE ) == 0
			  && e->index == index ) {
				e->atr = ( e->atr & ~( MN_GREY | MN_TICK | MN_HIDE ) )
				       | ( atr & ( MN_GREY | MN_TICK | MN_HIDE ) );
				return E_OK;
			}
		}
	}
	return E_NOEXS;
}

EXPORT ER mn_set_lst( ID mid, CONST TS_UUID *def, CONST char *code,
		      CONST UB *labels, INT stride, INT n )
{
	MNMENU	*m = menu_of(mid);
	MNNODE	*nd;
	MNITEM	hole;
	INT	k, i, j, at = -1;

	if ( m == NULL ) {
		return E_ID;
	}
	if ( code == NULL || n < 0 || ( n > 0 && ( labels == NULL || stride <= 0 ) ) ) {
		return E_PAR;
	}
	for ( k = 0; k < m->nnode && at < 0; k++ ) {
		for ( i = 0; i < m->node[k]->nitem; i++ ) {
			MNITEM	*e = &m->node[k]->item[i];

			if ( ( e->atr & MN_LIST_HOLE ) != 0 && item_is(e, def, code) ) {
				at = i;
				break;
			}
		}
	}
	if ( at < 0 ) {
		return E_NOEXS;
	}
	nd = m->node[--k];
	hole = nd->item[at];
	if ( nd->nitem - 1 + n > WM_ITEM_MAX ) {
		n = WM_ITEM_MAX - ( nd->nitem - 1 );
	}
	/* the items after the hole move to make room, the hole goes */
	for ( j = nd->nitem - 1; j > at; j-- ) {
		nd->item[j + n - 1] = nd->item[j];
	}
	for ( j = 0; j < n; j++ ) {
		MNITEM	*e = &nd->item[at + j];

		*e = hole;
		e->atr &= ~( MN_LIST_HOLE | MN_HIDE );
		e->index = j;
		(void)str_cpy(e->label, WM_LABEL_MAX, labels + j * stride);
	}
	nd->nitem += n - 1;

	return E_OK;
}

/* ---------------------------------------------------------------- showing */

/* Whether a node has anything that can be chosen, or opened */
LOCAL BOOL node_live( MNMENU *m, INT k, INT depth )
{
	MNNODE	*n = m->node[k];
	INT	i;

	for ( i = 0; i < n->nitem; i++ ) {
		MNITEM	*e = &n->item[i];

		if ( ( e->atr & ( MN_LINE | MN_HIDE | MN_GREY ) ) != 0 ) {
			continue;
		}
		if ( e->sub < 0 || ( depth < MN_DEPTH && node_live(m, e->sub, depth + 1) ) ) {
			return TRUE;
		}
	}
	return FALSE;
}

/* Node k as the window layer's list; rows, when given, gets each item's row */
LOCAL void node_list( MNMENU *m, ID mid, INT k, T_WMMENU *w, INT *rows )
{
	MNNODE	*n = m->node[k];
	INT	i;
	BOOL	prev_line = TRUE;

	for ( i = 0; rows != NULL && i < WM_ITEM_MAX; i++ ) {
		rows[i] = -1;
	}

	knl_memset(w, 0, sizeof(T_WMMENU));
	w->num = MN_LIST(mid - 1, k);
	w->owner = 0;
	for ( i = 0; i < n->nitem && w->nitem < WM_ITEM_MAX; i++ ) {
		MNITEM		*e = &n->item[i];
		T_WMITEM	*it;

		if ( ( e->atr & MN_HIDE ) != 0 ) {
			continue;
		}
		if ( ( e->atr & MN_LINE ) != 0 && prev_line ) {
			continue;			/* no line at the head, none twice */
		}
		if ( rows != NULL ) {
			rows[i] = w->nitem;
		}
		it = &w->item[w->nitem++];
		prev_line = (BOOL)( ( e->atr & MN_LINE ) != 0 );
		if ( prev_line ) {
			continue;			/* a line: command 0, no words */
		}
		it->cmd = MN_CMD(k, i);
		(void)str_cpy(it->label, WM_LABEL_MAX, e->label);
		(void)str_cpy(it->key, WM_KEY_MAX, e->key);
		it->grey = (BOOL)( ( e->atr & MN_GREY ) != 0 );
		it->tick = (BOOL)( ( e->atr & MN_TICK ) != 0 );
		if ( e->sub >= 0 ) {
			if ( !it->grey && node_live(m, e->sub, 0) ) {
				it->sub = MN_LIST(mid - 1, e->sub);
			} else {
				it->grey = TRUE;	/* a list with nothing to choose */
			}
		}
	}
	if ( w->nitem > 0 && w->item[w->nitem - 1].label[0] == 0 ) {
		w->nitem--;				/* no line at the foot */
	}
}

LOCAL MNLAST *last_of( CONST TS_UUID *def, BOOL make )
{
	INT	i;

	for ( i = 0; i < mn_nlast; i++ ) {
		if ( ts_uuid_cmp(&mn_last[i].def, def) == 0 ) {
			return &mn_last[i];
		}
	}
	if ( !make ) {
		return NULL;
	}
	i = ( mn_nlast < MN_MAX ) ? mn_nlast++ : MN_MAX - 1;
	mn_last[i].def = *def;
	mn_last[i].cur = 0;
	return &mn_last[i];
}

EXPORT INT mn_opn_men( ID mid, INT wid, INT x, INT y )
{
	MNMENU		*m = menu_of(mid);
	T_WMMENU	*w;
	MNLAST		*l;
	INT		k, pid;
	ER		er = E_OK;

	if ( m == NULL ) {
		return E_ID;
	}
	w = (T_WMMENU *)Kmalloc(sizeof(T_WMMENU));
	if ( w == NULL ) {
		return E_NOMEM;
	}
	for ( k = 1; k < m->nnode && er >= E_OK; k++ ) {
		node_list(m, mid, k, w, NULL);
		er = db_put(DB_MENU, w->num, 0, w, sizeof(T_WMMENU));
	}
	if ( er < E_OK ) {
		Kfree(w);
		return er;
	}
	node_list(m, mid, 0, w, m->row);
	l = last_of(&m->def, FALSE);
	if ( l != NULL && l->cur < w->nitem ) {
		w->cur = l->cur;
	}
	pid = wm_menu_open(wid, w, x, y);
	Kfree(w);

	return pid;
}

LOCAL ER sel_of( MNMENU *m, INT k, INT i, T_MNSEL *sel )
{
	MNITEM	*e;

	if ( k < 0 || k >= m->nnode || i < 0 || i >= m->node[k]->nitem ) {
		return E_NOEXS;
	}
	e = &m->node[k]->item[i];
	sel->def = e->def;
	(void)str_cpy(sel->code, MN_CODE_MAX, e->code);
	(void)str_cpy(sel->label, WM_LABEL_MAX, e->label);
	sel->index = e->index;
	sel->value = e->value;

	return E_OK;
}

EXPORT ER mn_get_sel( ID mid, INT cmd, T_MNSEL *sel )
{
	MNMENU	*m = menu_of(mid);
	INT	c, k, i, j;
	ER	er;

	if ( m == NULL ) {
		return E_ID;
	}
	if ( sel == NULL ) {
		return E_PAR;
	}
	if ( cmd < MN_CMD_BASE ) {
		return E_NOEXS;				/* nothing was chosen */
	}
	c = cmd - MN_CMD_BASE;
	k = c >> 8;
	i = ( c & 0xFF ) - 1;
	er = sel_of(m, k, i, sel);

	/* the root's row the choice was made under, for the next time it opens */
	while ( er >= E_OK && k != 0 ) {
		INT	pk = -1, pi = -1, a;

		for ( j = 0; j < m->nnode && pk < 0; j++ ) {
			for ( a = 0; a < m->node[j]->nitem; a++ ) {
				if ( m->node[j]->item[a].sub == k ) {
					pk = j;
					pi = a;
					break;
				}
			}
		}
		if ( pk < 0 ) {
			break;
		}
		k = pk;
		i = pi;
	}
	if ( er >= E_OK && k == 0 && i < WM_ITEM_MAX && m->row[i] >= 0 ) {
		last_of(&m->def, TRUE)->cur = m->row[i];
	}
	return er;
}

EXPORT ER mn_fnd_key( ID mid, UB key, T_MNSEL *sel )
{
	MNMENU	*m = menu_of(mid);
	INT	k, i;

	if ( m == NULL ) {
		return E_ID;
	}
	if ( key >= 'a' && key <= 'z' ) {
		key = (UB)( key - 'a' + 'A' );
	}
	for ( k = 0; k < m->nnode; k++ ) {
		for ( i = 0; i < m->node[k]->nitem; i++ ) {
			MNITEM	*e = &m->node[k]->item[i];

			if ( e->key[0] == key && e->key[1] == 0
			  && ( e->atr & ( MN_GREY | MN_HIDE | MN_LINE ) ) == 0 ) {
				return sel_of(m, k, i, sel);
			}
		}
	}
	return E_NOEXS;
}

EXPORT BOOL mn_is( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( sel != NULL && code != NULL && str_same(sel->code, code) );
}

/* ---------------------------------------------------------------- ウインドウ */

/*
 * The system's ウインドウ item (SYSDEF_MENU_WINLIST): its list filled
 * with the windows shown, nearest first, when the menu is about to
 * open, and a window chosen from it brought to the front with the keys.
 * The windows are the ones the list was filled with, kept here, so that
 * the one chosen is the one that was shown there.
 */
#define MN_WIN_MAX	24

LOCAL INT	mn_wins[MN_WIN_MAX];
LOCAL INT	mn_nwin;

LOCAL BOOL winlist_def( TS_UUID *u )
{
	return (BOOL)( ts_str_to_uuid(SYSDEF_MENU_WINLIST, u) >= E_OK );
}

EXPORT void mn_winlist_fill( ID mid )
{
	UB	(*names)[WM_LABEL_MAX];
	TS_UUID	wd;
	INT	i, k;
	CONST char *none = "（名前のないウインドウ）";

	if ( !winlist_def(&wd) ) return;
	names = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * MN_WIN_MAX);
	if ( names == NULL ) return;
	mn_nwin = wm_list(mn_wins, MN_WIN_MAX);
	for ( i = 0; i < mn_nwin; i++ ) {
		if ( wm_title(mn_wins[i], (char *)names[i], WM_LABEL_MAX) <= 0 ) {
			for ( k = 0; none[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) names[i][k] = (UB)none[k];
			names[i][k] = 0;
		}
	}
	(void)mn_set_lst(mid, &wd, "win.item", (CONST UB *)names, WM_LABEL_MAX, mn_nwin);
	Kfree(names);
}

EXPORT BOOL mn_winlist_do( CONST T_MNSEL *sel )
{
	TS_UUID	wd;

	if ( sel == NULL || !winlist_def(&wd) || ts_uuid_cmp(&sel->def, &wd) != 0
	  || !mn_is(sel, "win.item") ) {
		return FALSE;
	}
	if ( sel->index >= 0 && sel->index < mn_nwin ) {
		(void)wm_raise(mn_wins[sel->index]);
		(void)wm_focus(mn_wins[sel->index]);
		(void)wm_composite();
	}
	return TRUE;
}
