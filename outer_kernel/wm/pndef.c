/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pndef.c
 *	Panels made from a data box object (design 18.15)
 *
 *	A data box is an object whose record 0 is xmlTAD with a <databox>
 *	element holding numbered <panel>s. A panel says its size, its name
 *	(what the window showing it is called) and what it is for, and holds
 *	its parts in the order they are drawn:
 *
 *	  <tags num rect inner value><tag>＠名前</tag><tag>子名</tag>...</tags>
 *	  <text x y>words</text>			words on the base line y
 *	  <box num rect max jp ascii digits>letters</box>
 *	  <secret num rect max/>			a box whose letters are hidden
 *	  <number num rect value/>
 *	  <serial num rect zero blank><f w lo hi zero left/>.<n w><name/>..</n></serial>
 *	  <switch num rect value>name</switch>		on or off
 *	  <button num rect answer emph>name</button>	pushed
 *	  <select num rect value across nosel double><name/>...</select>
 *	  <list num rect value><name/>...</list>
 *	  <volume num rect lo hi value across emph box/>	box: its number box's num
 *	  <line rect/>
 *
 *	Every part may say sheet="n", the tag it is shown under, and
 *	off="true" when it cannot be worked. A rectangle is "l,t,r,b".
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/ob.h>
#include <ts/part.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/uuid.h>

LOCAL BOOL str_same( CONST UB *a, CONST char *b )
{
	while ( *a != 0 && *a == (UB)*b ) {
		a++;
		b++;
	}
	return (BOOL)( *a == 0 && *b == 0 );
}

LOCAL BOOL is( CONST T_TADNODE *nd, CONST char *name )
{
	return (BOOL)( nd->kind == TAD_ND_ELEM && nd->name != NULL && str_same(nd->name, name) );
}

LOCAL INT num( CONST T_TADNODE *nd, CONST char *name, INT dflt )
{
	CONST UB *v = tad_attr(nd, name);
	INT	x = 0;
	BOOL	neg = FALSE;

	if ( v == NULL ) {
		return dflt;
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

LOCAL BOOL yes( CONST T_TADNODE *nd, CONST char *name )
{
	CONST UB *v = tad_attr(nd, name);

	return (BOOL)( v != NULL && ( str_same(v, "true") || str_same(v, "1") ) );
}

/* "l,t,r,b" */
LOCAL BOOL rect( CONST T_TADNODE *nd, CONST char *name, T_DPRECT *r )
{
	CONST UB *v = tad_attr(nd, name);
	INT	a[4], k;

	if ( v == NULL ) {
		return FALSE;
	}
	for ( k = 0; k < 4; k++ ) {
		BOOL	neg = FALSE;

		a[k] = 0;
		while ( *v == ' ' || *v == ',' ) v++;
		if ( *v == '-' ) {
			neg = TRUE;
			v++;
		}
		if ( *v < '0' || *v > '9' ) {
			return FALSE;
		}
		while ( *v >= '0' && *v <= '9' ) {
			a[k] = a[k] * 10 + ( *v++ - '0' );
		}
		if ( neg ) a[k] = -a[k];
	}
	r->left = a[0];
	r->top = a[1];
	r->right = a[2];
	r->bottom = a[3];
	return TRUE;
}

/* The words directly inside an element */
LOCAL INT words( CONST T_TADNODE *nd, UB *out, INT max )
{
	CONST T_TADNODE	*c;
	INT		n = 0;

	for ( c = nd->first; c != NULL; c = c->next ) {
		CONST UB *t = c->text;

		if ( c->kind != TAD_ND_TEXT || t == NULL ) {
			continue;
		}
		while ( *t != 0 && n < max - 1 ) {
			if ( *t != '\n' && *t != '\r' && *t != '\t' ) {
				out[n++] = *t;
			}
			t++;
		}
	}
	out[n] = 0;
	return n;
}

/* The <name>s of an element into the pool: how many */
LOCAL INT names( T_WMPANEL *def, CONST T_TADNODE *nd, INT *p_at, CONST char *tag )
{
	CONST T_TADNODE	*c;
	UB		w[WM_LABEL_MAX];
	INT		n = 0, at;

	*p_at = def->used;
	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( !is(c, tag) ) {
			continue;
		}
		(void)words(c, w, sizeof(w));
		at = wm_panel_name(def, w);
		if ( at < 0 ) {
			break;
		}
		n++;
	}
	return n;
}

LOCAL void label( T_WMPART *pt, CONST UB *s )
{
	INT	i;

	for ( i = 0; i < WM_LABEL_MAX - 1 && s[i] != 0; i++ ) {
		pt->label[i] = s[i];
	}
	pt->label[i] = 0;
}

/* Fields along a line: the label with a run of hashes for each field */
LOCAL void serial( T_WMPANEL *def, T_WMPART *pt, CONST T_TADNODE *nd )
{
	CONST T_TADNODE	*c;
	INT		n = 0, f = 0, k;

	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_TEXT && c->text != NULL ) {
			CONST UB *t = c->text;

			while ( *t != 0 && n < WM_LABEL_MAX - 1 ) {
				if ( *t != '\n' && *t != '\r' && *t != '\t' && *t != '#' ) {
					pt->label[n++] = *t;
				}
				t++;
			}
			continue;
		}
		if ( ( !is(c, "f") && !is(c, "n") ) || f >= WM_SB_MAX ) {
			continue;
		}
		pt->sbw[f] = num(c, "w", 2);
		for ( k = 0; k < pt->sbw[f] && n < WM_LABEL_MAX - 1; k++ ) {
			pt->label[n++] = '#';
		}
		if ( is(c, "f") ) {
			pt->sblo[f] = num(c, "lo", 0);
			pt->sbhi[f] = num(c, "hi", 0);
			pt->sbv[f] = num(c, "value", pt->sblo[f]);
			if ( yes(c, "zero") ) pt->sbf[f] |= SBF_ZERO;
			if ( yes(c, "left") ) pt->sbf[f] |= SBF_LEFT;
		} else {
			INT	at, cnt = names(def, c, &at, "name");

			pt->sbf[f] = SBF_NAMES;
			pt->sbpool[f] = at;
			pt->sblo[f] = 0;
			pt->sbhi[f] = ( cnt > 0 ) ? cnt - 1 : 0;
			pt->sbv[f] = num(c, "value", 0);
		}
		f++;
		/* two fields side by side are kept apart */
		if ( c->next != NULL && ( is(c->next, "f") || is(c->next, "n") ) && n < WM_LABEL_MAX - 1 ) {
			pt->label[n++] = ' ';
		}
	}
	pt->label[n] = 0;
	pt->sb_n = f;
	pt->sb_at = 0;
}

/* One element of a <panel> as a part, appended; FALSE for what is not one */
LOCAL BOOL part( T_WMPANEL *def, CONST T_TADNODE *nd )
{
	T_WMPART	*pt;
	UB		w[WM_LABEL_MAX];
	INT		at;

	if ( nd->kind != TAD_ND_ELEM || def->npart >= WM_PART_MAX ) {
		return FALSE;
	}
	pt = &def->part[def->npart];
	knl_memset(pt, 0, sizeof(*pt));
	pt->num = num(nd, "num", 0);
	pt->sheet = num(nd, "sheet", 0);
	pt->sb_at = -1;
	(void)rect(nd, "rect", &pt->r);
	if ( yes(nd, "off") ) pt->type |= P_DISABLE;
	if ( yes(nd, "emph") ) pt->type |= P_EMPHAS;
	if ( yes(nd, "across") ) pt->type |= P_HALIGN;

	if ( is(nd, "text") ) {
		INT	x = num(nd, "x", 0), y = num(nd, "y", 0);

		pt->type |= WM_PT_LABEL | P_BASE;
		(void)words(nd, w, sizeof(w));
		label(pt, w);
		pt->r.left = x;
		pt->r.top = y - 16;
		pt->r.right = def->r.right - def->r.left;
		pt->r.bottom = y + 4;
		if ( pt->r.right <= x ) pt->r.right = x + 1;
	} else if ( is(nd, "tags") ) {
		pt->type |= TG_PARTS;
		(void)rect(nd, "inner", &pt->inner);
		pt->count = names(def, nd, &at, "tag");
		pt->pool = at;
		pt->value = num(nd, "value", 1);
	} else if ( is(nd, "box") || is(nd, "secret") ) {
		pt->type |= is(nd, "box") ? TB_PARTS : XB_PARTS;
		pt->max = num(nd, "max", 0);
		if ( yes(nd, "jp") ) pt->type |= P_JP;
		if ( yes(nd, "digits") ) pt->type |= P_DIGITS;
		if ( yes(nd, "ascii") ) pt->type |= P_ASCII;
		(void)words(nd, pt->text, WM_LABEL_MAX);
	} else if ( is(nd, "number") ) {
		pt->type |= NB_PARTS;
		pt->value = num(nd, "value", 0);
	} else if ( is(nd, "serial") ) {
		pt->type |= SB_PARTS;
		if ( yes(nd, "zero") ) pt->type |= P_ZERO;
		if ( yes(nd, "blank") ) pt->type |= P_BLANK;
		serial(def, pt, nd);
	} else if ( is(nd, "switch") ) {
		pt->type |= AS_PARTS;
		pt->value = num(nd, "value", 0);
		(void)words(nd, w, sizeof(w));
		label(pt, w);
	} else if ( is(nd, "button") ) {
		CONST UB *a = tad_attr(nd, "answer");

		pt->type |= MS_PARTS;
		(void)words(nd, w, sizeof(w));
		label(pt, w);
		pt->answer = ( a == NULL ) ? WM_ANS_NONE : str_same(a, "ok") ? WM_ANS_OK
			   : str_same(a, "cancel") ? WM_ANS_CANCEL : WM_ANS_NONE;
	} else if ( is(nd, "select") || is(nd, "list") ) {
		pt->type |= is(nd, "select") ? WS_PARTS : SS_PARTS;
		if ( yes(nd, "nosel") ) pt->type |= P_NOSEL;
		if ( yes(nd, "double") ) pt->type |= P_DOUBLE;
		pt->count = names(def, nd, &at, "name");
		pt->pool = at;
		pt->value = num(nd, "value", 0);
		pt->now = num(nd, "now", 0);
		pt->top = 1;
		if ( is(nd, "select") && pt->count == 0 ) {
			return FALSE;		/* a selector with no names is nothing */
		}
	} else if ( is(nd, "volume") ) {
		pt->type |= VL_PARTS;
		pt->lo = num(nd, "lo", 0);
		pt->hi = num(nd, "hi", 100);
		pt->value = num(nd, "value", pt->lo);
		pt->sub = num(nd, "box", 0);
	} else if ( is(nd, "line") ) {
		pt->type |= WM_PT_LINE;
		if ( pt->r.bottom <= pt->r.top ) pt->r.bottom = pt->r.top + 1;
		if ( pt->r.right <= pt->r.left ) pt->r.right = pt->r.left + 1;
	} else {
		return FALSE;
	}
	def->npart++;
	return TRUE;
}

LOCAL CONST char *pn_kinds[] = { "plain", "warn", "fatal", "bare" };

EXPORT ER knl_pn_load( CONST TS_UUID *box, INT n, T_WMPANEL *def, UB *title, INT tmax )
{
	CONST T_TADNODE	*db = NULL, *c, *pn = NULL;
	T_TAD		*doc = NULL;
	UB		*rec;
	SZ		size = 0;
	ER		er;
	INT		k;

	if ( box == NULL || def == NULL ) {
		return E_PAR;
	}
	rec = om_obj_record(box, 0, &size);
	if ( rec == NULL ) {
		return E_NOEXS;
	}
	er = tad_parse(rec, size, NULL, &doc);
	if ( er >= E_OK ) {
		for ( c = tad_root(doc)->first; c != NULL && db == NULL; c = c->next ) {
			if ( is(c, "databox") ) db = c;
		}
		for ( c = ( db != NULL ) ? db->first : NULL; c != NULL && pn == NULL; c = c->next ) {
			if ( is(c, "panel") && num(c, "num", -1) == n ) pn = c;
		}
		er = ( pn != NULL ) ? E_OK : E_NOEXS;
	}
	if ( er >= E_OK ) {
		CONST UB *kind = tad_attr(pn, "kind");

		knl_memset(def, 0, sizeof(*def));
		def->num = n;
		if ( !rect(pn, "rect", &def->r) || def->r.right <= def->r.left
		  || def->r.bottom <= def->r.top ) {
			er = E_OBJ;
		}
		for ( k = 0; kind != NULL && k < 4; k++ ) {
			if ( str_same(kind, pn_kinds[k]) ) def->kind = (UINT)k;
		}
		if ( title != NULL && tmax > 0 ) {
			CONST UB *t = tad_attr(pn, "title");
			INT	 i = 0;

			while ( t != NULL && t[i] != 0 && i < tmax - 1 ) {
				title[i] = t[i];
				i++;
			}
			title[i] = 0;
		}
		for ( c = pn->first; er >= E_OK && c != NULL; c = c->next ) {
			(void)part(def, c);
		}
	}
	if ( doc != NULL ) tad_free(doc);
	Kfree(rec);
	return er;
}
