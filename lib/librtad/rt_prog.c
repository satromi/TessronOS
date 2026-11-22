/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rt_prog.c
 *	A <realtime> read into a program (design 15.3)
 *
 *	Every element the engine acts on becomes a node, and the elements
 *	inside one that holds events -- a group, an eventArray, the then of
 *	an if, the do of a while, a random, an interpolation, a trigger, a
 *	handler -- become the block under it. What only describes -- the
 *	streams, the unit systems -- is taken into tables as it is read,
 *	and the media and pictures the program brings in are declared once,
 *	so that whoever shows them can make them ready before the run.
 *
 *	Nothing is copied: names and values point into the parsed record.
 */

#include "rt_local.h"

LOCAL RTN *block( RT *rt, CONST T_TADNODE *first, RTN *up, INT blk );

/* ---------------------------------------------------------------- memory */

LOCAL void *get( RT *rt, SZ size )
{
	return rt->host.alloc(rt->host.arg, size);
}

LOCAL RTN *node( RT *rt, UH op, CONST T_TADNODE *nd, RTN *up, INT blk )
{
	RTN	*n = (RTN *)get(rt, sizeof(RTN));

	if ( n == NULL ) {
		rt->oom = TRUE;
		return NULL;
	}
	n->op = op;
	n->nd = nd;
	n->up = up;
	n->block = blk;
	n->last = -1;
	n->trig = -1;
	return n;
}

LOCAL void free_nodes( RT *rt, RTN *n )
{
	while ( n != NULL ) {
		RTN	*next = n->next;

		free_nodes(rt, n->body);
		free_nodes(rt, n->alt);
		if ( n->code != NULL ) rt->host.free(rt->host.arg, n->code);
		if ( n->code2 != NULL ) rt->host.free(rt->host.arg, n->code2);
		rt->host.free(rt->host.arg, n);
		n = next;
	}
}

/*
 * A table made room in for 'need' entries of esz bytes: it doubles (from
 * 8) and what it held is moved. pp is the address of the table's
 * pointer. FALSE when the memory is not there (the table is as it was).
 */
EXPORT BOOL rt_room( RT *rt, void *pp, INT *cap, INT need, SZ esz )
{
	void	**p = (void **)pp;
	void	*t;
	INT	c = *cap;

	if ( need <= c ) return TRUE;
	if ( c < 8 ) c = 8;
	while ( c < need ) {
		if ( c > 0x3FFFFFFF ) return FALSE;
		c *= 2;
	}
	if ( (SZ)c > (SZ)0x7FFFFFFF / esz ) return FALSE;
	t = get(rt, (SZ)c * esz);
	if ( t == NULL ) return FALSE;
	if ( *p != NULL ) {
		SZ	k, n = (SZ)*cap * esz;

		for ( k = 0; k < n; k++ ) ((UB *)t)[k] = ((UB *)*p)[k];
		rt->host.free(rt->host.arg, *p);
	}
	*p = t;
	*cap = c;
	return TRUE;
}

/*
 * A table of slots, each a place of its own that does not move once
 * made (a thread is pointed to while others are started). Room for need
 * slots; if the memory runs out half way, the slots there are stay and
 * FALSE is answered.
 */
EXPORT BOOL rt_room_slots( RT *rt, void *pp, INT *cap, INT need, SZ esz )
{
	void	***p = (void ***)pp;
	void	**t;
	INT	c = *cap, k;

	if ( need <= c ) return TRUE;
	if ( c < 8 ) c = 8;
	while ( c < need ) {
		if ( c > 0x3FFFFFFF ) return FALSE;
		c *= 2;
	}
	if ( (SZ)c > (SZ)0x7FFFFFFF / (SZ)sizeof(void *) ) return FALSE;
	t = (void **)get(rt, (SZ)c * sizeof(void *));
	if ( t == NULL ) return FALSE;
	for ( k = 0; k < *cap; k++ ) t[k] = (*p)[k];
	for ( k = *cap; k < c; k++ ) {
		t[k] = get(rt, esz);
		if ( t[k] == NULL ) break;
	}
	if ( *p != NULL ) rt->host.free(rt->host.arg, *p);
	*p = t;
	*cap = k;
	return (BOOL)( k >= need );
}

/* Room for n keyframes in an animation */
EXPORT BOOL rt_anim_keys( RT *rt, RTANIM *a, INT n )
{
	INT	c = a->ckey, c2 = c, c3 = c;

	if ( n <= c ) return TRUE;
	if ( !rt_room(rt, &a->val, &c, n, sizeof(RT_V)) ) return FALSE;
	if ( !rt_room(rt, &a->off, &c2, n, sizeof(INT)) ) return FALSE;
	if ( !rt_room(rt, &a->ease, &c3, n, sizeof(RT_EASE)) ) return FALSE;
	a->ckey = c;			/* the three grew by the same steps */
	return TRUE;
}

/* Room for n blocks in a thread */
EXPORT BOOL rt_frames( RT *rt, RTTHR *th, INT n )
{
	INT	c = th->cfr;

	if ( n <= c ) return TRUE;
	if ( c == 0 && n <= RT_DEPTH0 ) n = RT_DEPTH0;
	if ( !rt_room(rt, &th->fr, &c, n, sizeof(RTFRAME)) ) return FALSE;
	th->cfr = c;
	return TRUE;
}

LOCAL void drop( RT *rt, void *p )
{
	if ( p != NULL ) rt->host.free(rt->host.arg, p);
}

/* The tables of the run, given back */
LOCAL void free_slots( RT *rt, void **t, INT c )
{
	INT	i;

	for ( i = 0; t != NULL && i < c; i++ ) drop(rt, t[i]);
	drop(rt, t);
}

EXPORT void rt_free_prog( RT *rt )
{
	INT	i;

	free_nodes(rt, rt->pre);
	rt->pre = NULL;
	for ( i = 0; i < rt->nblk; i++ ) {
		free_nodes(rt, rt->blk[i].body);
		rt->blk[i].body = NULL;
	}
	for ( i = 0; i < rt->nusys; i++ ) {
		drop(rt, rt->usys[i].auxno);
		drop(rt, rt->usys[i].an);
		drop(rt, rt->usys[i].ad);
	}
	for ( i = 0; i < rt->cthr; i++ ) drop(rt, rt->thr[i]->fr);
	for ( i = 0; i < rt->canim; i++ ) {
		drop(rt, rt->anim[i]->val);
		drop(rt, rt->anim[i]->off);
		drop(rt, rt->anim[i]->ease);
	}
	free_slots(rt, (void **)rt->thr, rt->cthr);
	free_slots(rt, (void **)rt->anim, rt->canim);
	free_slots(rt, (void **)rt->trig, rt->ctrig);
	drop(rt, rt->blk);
	drop(rt, rt->onerr);
	drop(rt, rt->usys);
	drop(rt, rt->stream);
	drop(rt, rt->obj);
	drop(rt, rt->media);
	drop(rt, rt->mnode);
	drop(rt, rt->upaused);
	drop(rt, rt->repl);
	drop(rt, rt->want);
	drop(rt, rt->sndq);
	drop(rt, rt->mev);
	rt->thr = NULL;
	rt->anim = NULL;
	rt->trig = NULL;
	rt->blk = NULL;
	rt->onerr = NULL;
	rt->usys = NULL;
	rt->stream = NULL;
	rt->obj = NULL;
	rt->media = NULL;
	rt->mnode = NULL;
	rt->upaused = NULL;
	rt->repl = NULL;
	rt->want = NULL;
	rt->sndq = NULL;
	rt->mev = NULL;
	rt->cthr = rt->canim = rt->ctrig = 0;
	rt->nblk = rt->nusys = rt->nstream = rt->nobj = rt->nmedia = rt->nrepl = rt->nwant = 0;
}

/* ---------------------------------------------------------------- what it moves */

LOCAL void want( RT *rt, CONST UB *id )
{
	INT	i;

	if ( id == NULL || id[0] == 0 ) return;
	for ( i = 0; i < rt->nwant; i++ ) {
		if ( rt_eq(rt->want[i], id) ) return;
	}
	if ( rt_room(rt, &rt->want, &rt->cwant, rt->nwant + 1, sizeof(CONST UB *)) ) rt->want[rt->nwant++] = id;
	else rt->oom = TRUE;
}

EXPORT BOOL rt_wants( CONST RT *rt, CONST UB *id )
{
	INT	i;

	if ( rt == NULL || id == NULL ) return FALSE;
	for ( i = 0; i < rt->nwant; i++ ) {
		if ( rt_eq(rt->want[i], id) ) return TRUE;
	}
	return FALSE;
}

EXPORT INT rt_obj_find( CONST RT *rt, CONST UB *id )
{
	INT	i;

	if ( rt == NULL || id == NULL ) return -1;
	for ( i = 0; i < rt->nobj; i++ ) {
		if ( rt_eq(rt->obj[i].id, id) ) return i;
	}
	return -1;
}

EXPORT INT rt_obj_add( RT *rt, CONST UB *id, UINT kind, CONST T_TADNODE *nd )
{
	RT_OBJ	*o;
	INT	i = rt_obj_find(rt, id);

	if ( i >= 0 ) return i;
	if ( !rt_room(rt, &rt->obj, &rt->cobj, rt->nobj + 1, sizeof(RT_OBJ)) ) {
		rt->oom = TRUE;
		return -1;
	}
	i = rt->nobj++;
	o = &rt->obj[i];
	o->id = id;
	o->kind = kind;
	o->nd = nd;
	o->media = -1;
	o->p[RT_P_ALPHA] = RT_ONE;
	o->p[RT_P_SX] = o->p[RT_P_SY] = RT_ONE;
	return i;
}

/* An object's place and look as an element's attributes say */
LOCAL void obj_from( RT_OBJ *o, CONST T_TADNODE *nd )
{
	o->p[RT_P_LEFT] = rt_val(rt_attr(nd, "left"), o->p[RT_P_LEFT]);
	o->p[RT_P_TOP] = rt_val(rt_attr(nd, "top"), o->p[RT_P_TOP]);
	o->p[RT_P_RIGHT] = rt_val(rt_attr(nd, "right"), o->p[RT_P_RIGHT]);
	o->p[RT_P_BOTTOM] = rt_val(rt_attr(nd, "bottom"), o->p[RT_P_BOTTOM]);
	o->p[RT_P_ALPHA] = rt_val(rt_attr(nd, "alpha"), o->p[RT_P_ALPHA]);
	o->p[RT_P_ROT] = rt_val(rt_attr(nd, "rotation"), o->p[RT_P_ROT]);
	if ( rt_attr(nd, "zIndex") != NULL ) o->z = rt_int(rt_attr(nd, "zIndex"), o->z);
	if ( rt_attr(nd, "flipH") != NULL ) o->flip_h = rt_bool(rt_attr(nd, "flipH"), FALSE);
	if ( rt_attr(nd, "flipV") != NULL ) o->flip_v = rt_bool(rt_attr(nd, "flipV"), FALSE);
}

LOCAL void obj_keep_base( RT_OBJ *o )
{
	INT	k;

	for ( k = 0; k < RT_NP; k++ ) o->base[k] = o->p[k];
	o->base_z = o->z;
	o->base_shown = o->shown;
	o->base_href = o->href;
}

EXPORT INT rt_bind( RT *rt, CONST UB *id, UINT kind, CONST T_DPRECT *box, INT z,
		    CONST T_TADNODE *nd )
{
	INT	i;
	RT_OBJ	*o;

	if ( rt == NULL || id == NULL || box == NULL ) return E_PAR;
	i = rt_obj_add(rt, id, kind, nd);
	if ( i < 0 ) return E_NOMEM;
	o = &rt->obj[i];
	if ( o->bound ) return i;		/* the first element with the id is the one */
	o->kind = kind;
	o->nd = nd;
	o->p[RT_P_LEFT] = (RT_V)box->left * RT_ONE;
	o->p[RT_P_TOP] = (RT_V)box->top * RT_ONE;
	o->p[RT_P_RIGHT] = (RT_V)box->right * RT_ONE;
	o->p[RT_P_BOTTOM] = (RT_V)box->bottom * RT_ONE;
	o->p[RT_P_ALPHA] = RT_ONE;
	o->p[RT_P_ROT] = 0;
	o->p[RT_P_SX] = o->p[RT_P_SY] = RT_ONE;
	o->z = z;
	o->shown = TRUE;
	o->bound = TRUE;
	if ( kind == RT_OB_IMAGE ) {
		o->href = rt_attr(nd, "href");
		o->p[RT_P_ALPHA] = rt_val(rt_attr(nd, "alpha"), RT_ONE);
		o->p[RT_P_ROT] = rt_val(rt_attr(nd, "rotation"), 0);
		o->flip_h = rt_bool(rt_attr(nd, "flipH"), FALSE);
		o->flip_v = rt_bool(rt_attr(nd, "flipV"), FALSE);
	}
	o->dirty = RT_D_BOX | RT_D_LOOK | RT_D_SHOWN | RT_D_Z;
	obj_keep_base(o);
	return i;
}

EXPORT INT rt_nobj( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->nobj : 0;
}

EXPORT RT_OBJ *rt_obj( RT *rt, INT i )
{
	return ( rt != NULL && i >= 0 && i < rt->nobj ) ? &rt->obj[i] : NULL;
}

EXPORT CONST UB *rt_replace_href( CONST RT *rt, INT i, CONST UB **p_ref )
{
	if ( rt == NULL || i < 0 || i >= rt->nrepl ) return NULL;
	if ( p_ref != NULL ) *p_ref = rt->repl[i].ref;
	return rt->repl[i].href;
}

/* ---------------------------------------------------------------- media */

EXPORT INT rt_media_of( CONST RT *rt, CONST UB *id )
{
	INT	i;

	if ( id == NULL ) return -1;
	for ( i = 0; i < rt->nmedia; i++ ) {
		if ( rt_eq(rt->media[i].id, id) ) return i;
	}
	return -1;
}

LOCAL RT_TIME mtime( RT *rt, CONST RTUNIT *u, CONST T_TADNODE *nd, CONST char *name, RT_TIME dflt )
{
	CONST UB	*s = rt_attr(nd, name);
	RTUNIT		uu;

	if ( s == NULL ) return dflt;
	uu = *u;
	return rt_utime(rt, &uu, s, NULL);
}

EXPORT void rt_media_attrs( RT *rt, RT_MEDIA *m, CONST T_TADNODE *nd, CONST RTUNIT *u )
{
	CONST UB	*s;

	m->nd = nd;
	m->href = rt_attr(nd, "href");
	m->format = rt_attr(nd, "format");
	m->autoplay = rt_bool(rt_attr(nd, "autoplay"), TRUE);
	s = rt_attr(nd, "trigger");
	m->trigger = rt_same(s, "manual") ? RT_TR_MANUAL : rt_same(s, "load") ? RT_TR_LOAD
		   : rt_same(s, "visible") ? RT_TR_VISIBLE : RT_TR_TIME;
	s = rt_attr(nd, "preload");
	m->preload = rt_same(s, "none") ? RT_PRE_NONE : rt_same(s, "metadata") ? RT_PRE_META : RT_PRE_AUTO;
	m->volume = rt_val(rt_attr(nd, "volume"), RT_ONE);
	m->pan = rt_val(rt_attr(nd, "pan"), 0);
	m->rate = rt_val(rt_attr(nd, "playbackRate"), RT_ONE);
	m->start = mtime(rt, u, nd, "startTime", 0);
	m->dur = mtime(rt, u, nd, "duration", 0);
	m->loop = rt_bool(rt_attr(nd, "loop"), FALSE);
	m->loop_start = mtime(rt, u, nd, "loopStart", 0);
	m->loop_end = mtime(rt, u, nd, "loopEnd", 0);
	m->fade_in = mtime(rt, u, nd, "fadeIn", 0);
	m->fade_out = mtime(rt, u, nd, "fadeOut", 0);
	m->timeout = mtime(rt, u, nd, "timeout", 0);
	m->total = mtime(rt, u, nd, "totalDuration", 0);
	m->muted = rt_bool(rt_attr(nd, "muted"), FALSE);
	m->track = rt_int(rt_attr(nd, "audioTrack"), 0);
	m->onended = rt_attr(nd, "onended");
	m->title = rt_attr(nd, "title");
	m->artist = rt_attr(nd, "artist");
	m->album = rt_attr(nd, "album");
	m->trackno = rt_int(rt_attr(nd, "trackNumber"), 0);
	m->sample_rate = rt_int(rt_attr(nd, "sampleRate"), 0);
	m->channels = rt_int(rt_attr(nd, "channels"), 0);
	m->bits = rt_int(rt_attr(nd, "bitDepth"), rt_int(rt_attr(nd, "bits"), 0));
}

/* The medium an element declares: one for each id, one for each element without */
LOCAL INT media_decl( RT *rt, CONST T_TADNODE *nd, UINT kind )
{
	CONST UB	*id = rt_attr(nd, "id");
	RT_MEDIA	*m;
	RTUNIT		u;
	INT		i = ( id != NULL ) ? rt_media_of(rt, id) : -1;

	if ( i >= 0 ) return i;
	if ( !rt_room(rt, &rt->media, &rt->cmedia, rt->nmedia + 1, sizeof(RT_MEDIA)) ) {
		rt->oom = TRUE;
		return -1;
	}
	if ( !rt_room(rt, &rt->mnode, &rt->cmnode, rt->nmedia + 1, sizeof(RTN *))
	  || !rt_room(rt, &rt->upaused, &rt->cupaused, rt->nmedia + 1, sizeof(BOOL)) ) {
		rt->oom = TRUE;
		return -1;
	}
	i = rt->nmedia++;
	m = &rt->media[i];
	m->id = id;
	m->kind = kind;
	m->obj = -1;
	u.sys = -1;
	u.aux = -1;
	u.tsys = -2;
	u.taux = -1;
	u.sn = u.sd = 1;
	rt_media_attrs(rt, m, nd, &u);
	if ( kind == RT_MK_VIDEO ) {
		/* and it is shown: a display object of the same id, or of its file */
		INT	k = rt_obj_add(rt, ( id != NULL ) ? id : m->href, RT_OB_VIDEO, nd);

		if ( k >= 0 ) {
			RT_OBJ	*o = &rt->obj[k];

			o->media = i;
			o->kind = RT_OB_VIDEO;
			obj_from(o, nd);
			o->href = rt_attr(nd, "poster");
			o->bound = TRUE;
			o->shown = FALSE;
			obj_keep_base(o);
			m->obj = k;
		}
	}
	return i;
}

/* ---------------------------------------------------------------- expressions */

EXPORT INT rt_expr_code( CONST T_TADNODE *expr, UH *code, INT max )
{
	CONST T_TADNODE	*c;
	INT		n = 0;

	for ( c = ( expr != NULL ) ? expr->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM ) continue;
		if ( rt_is(c, "const") ) {
			if ( n + 2 > max ) return -1;
			code[n++] = RT_EX_CONST;
			code[n++] = (UH)rt_int(rt_attr(c, "value"), 0);
		} else if ( rt_is(c, "var") ) {
			INT	v = rt_int(rt_attr(c, "id"), 0);

			if ( n + 1 > max || v <= 0 || v > 0x7FFF ) return -1;
			code[n++] = (UH)v;
		} else if ( rt_is(c, "op") ) {
			UINT	op = rt_op_code(rt_attr(c, "type"));

			if ( n + 1 > max || op == 0 ) return -1;
			code[n++] = (UH)op;
		}
	}
	return n;
}

/* An <expr> child of an element into a node's code; FALSE when there is none */
LOCAL BOOL expr_of( RT *rt, CONST T_TADNODE *in, CONST char *name, UH **p_code, INT *p_n )
{
	CONST T_TADNODE	*e = rt_child(in, name);
	UH		buf[RT_EX_MAX];
	INT		n;

	*p_code = NULL;
	*p_n = 0;
	if ( e == NULL ) return FALSE;
	n = rt_expr_code(e, buf, RT_EX_MAX);
	if ( n <= 0 ) return FALSE;		/* nothing in it: as if it were not there */
	*p_code = (UH *)get(rt, sizeof(UH) * (SZ)n);
	if ( *p_code == NULL ) return FALSE;
	*p_n = n;
	for ( n = n - 1; n >= 0; n-- ) ( *p_code )[n] = buf[n];
	return TRUE;
}

/* ---------------------------------------------------------------- units */

LOCAL void unit_system( RT *rt, CONST T_TADNODE *nd )
{
	RTUSYS		*u;
	CONST T_TADNODE	*p, *d, *c;
	INT		id = rt_int(rt_attr(nd, "id"), -1), i;

	if ( id < 0 ) return;
	for ( i = 0; i < rt->nusys && rt->usys[i].id != id; i++ ) ;
	if ( i == rt->nusys ) {
		if ( !rt_room(rt, &rt->usys, &rt->cusys, rt->nusys + 1, sizeof(RTUSYS)) ) {
			rt->oom = TRUE;
			return;
		}
		rt->nusys++;
	}
	u = &rt->usys[i];
	u->id = id;
	u->pn = 1;			/* a second, unless it says */
	u->pd = 1;
	u->naux = 0;
	p = rt_child(nd, "primaryUnit");
	d = rt_child(p, "derivation");
	if ( d != NULL ) (void)rt_ratio(rt_attr(d, "ratio"), &u->pn, &u->pd);
	if ( u->pn <= 0 || u->pd <= 0 ) {
		u->pn = 1;
		u->pd = 1;
	}
	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( rt_is(c, "auxiliaryUnit") ) {
			D	n = 1, dd = 1;

			if ( u->naux >= u->caux ) {
				INT	c1 = u->caux, c2 = u->caux, c3 = u->caux;

				if ( !rt_room(rt, &u->auxno, &c1, u->naux + 1, sizeof(INT))
				  || !rt_room(rt, &u->an, &c2, u->naux + 1, sizeof(D))
				  || !rt_room(rt, &u->ad, &c3, u->naux + 1, sizeof(D)) ) {
					rt->oom = TRUE;
					break;
				}
				u->caux = c1;
			}

			if ( !rt_ratio(rt_attr(c, "derivation"), &n, &dd) || n <= 0 || dd <= 0 ) {
				n = 1;
				dd = 1;
			}
			u->auxno[u->naux] = rt_int(rt_attr(c, "number"), u->naux);
			u->an[u->naux] = n;
			u->ad[u->naux] = dd;
			u->naux++;
		}
	}
}

/* ---------------------------------------------------------------- controls */

LOCAL void ctrl( RT *rt, RTN *n, UINT kind )
{
	CONST T_TADNODE	*nd = n->nd;

	n->i1 = (INT)kind;
	n->s1 = rt_attr(nd, "ref");
	if ( n->s1 == NULL ) n->s1 = rt_attr(nd, "target");
	n->s2 = rt_attr(nd, "command");
	if ( n->s2 == NULL ) n->s2 = rt_attr(nd, "action");
	n->s3 = rt_attr(nd, "duration");
	n->s4 = rt_attr(nd, "href");
	if ( rt_num(rt_attr(nd, "value"), &n->v[0]) ) n->flag |= CF_VALUE;
	if ( rt_attr(nd, "left") != NULL ) n->flag |= CF_LEFT;
	if ( rt_attr(nd, "top") != NULL ) n->flag |= CF_TOP;
	if ( rt_attr(nd, "right") != NULL ) n->flag |= CF_RIGHT;
	if ( rt_attr(nd, "bottom") != NULL ) n->flag |= CF_BOTTOM;
	if ( n->s3 != NULL ) n->flag |= CF_DUR;
	if ( kind == CT_IMAGE || kind == CT_TEXT || kind == CT_SHAPE || kind == CT_VIDEO ) {
		want(rt, n->s1);
	}
	if ( kind == CT_IMAGE && rt_same(n->s2, "replace") && n->s4 != NULL
	  && rt_room(rt, &rt->repl, &rt->crepl, rt->nrepl + 1, sizeof(RTREPL)) ) {
		rt->repl[rt->nrepl].href = n->s4;
		rt->repl[rt->nrepl].ref = n->s1;
		rt->nrepl++;
	}
}

/* ---------------------------------------------------------------- elements */

LOCAL RTN *element( RT *rt, CONST T_TADNODE *nd, RTN *up, INT blk )
{
	RTN		*n = NULL;
	CONST T_TADNODE	*c;

	if ( rt_is(nd, "realGroup") ) {
		CONST UB	*t = rt_attr(nd, "type");

		n = node(rt, OP_GROUP, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->i1 = rt_same(t, "serial") ? 1 : 0;
		n->i2 = rt_int(rt_attr(nd, "stream"), -1);
		n->s1 = rt_attr(nd, "id");
		n->body = block(rt, nd->first, n, blk);
	} else if ( rt_is(nd, "eventArray") ) {
		n = node(rt, OP_BLOCK, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "id");
		n->body = block(rt, nd->first, n, blk);
	} else if ( rt_is(nd, "timeDelta") || rt_is(nd, "timeAbsolute") ) {
		n = node(rt, rt_is(nd, "timeDelta") ? OP_TDELTA : OP_TABS, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "value");
		n->s2 = rt_attr(nd, "unit");
	} else if ( rt_is(nd, "label") ) {
		n = node(rt, OP_LABEL, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "id");
		n->s2 = rt_attr(nd, "name");
	} else if ( rt_is(nd, "jump") ) {
		n = node(rt, OP_JUMP, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "target");
	} else if ( rt_is(nd, "if") ) {
		RTN	**tail;

		n = node(rt, OP_IF, nd, up, blk);
		if ( n == NULL ) return NULL;
		(void)expr_of(rt, nd, "expr", &n->code, &n->ncode);
		c = rt_child(nd, "then");
		if ( c != NULL ) {
			n->body = block(rt, c->first, n, blk);
		} else {
			/* the events written straight inside it */
			n->body = block(rt, nd->first, n, blk);
		}
		tail = &n->alt;
		for ( c = nd->first; c != NULL; c = c->next ) {
			RTN		*e;
			CONST T_TADNODE	*t;

			if ( rt_is(c, "elseif") ) {
				e = node(rt, OP_ELSEIF, c, up, blk);
				if ( e == NULL ) break;
				(void)expr_of(rt, c, "expr", &e->code, &e->ncode);
				t = rt_child(c, "then");
				e->body = block(rt, ( t != NULL ) ? t->first : c->first, e, blk);
			} else if ( rt_is(c, "else") ) {
				e = node(rt, OP_ELSE, c, up, blk);
				if ( e == NULL ) break;
				e->body = block(rt, c->first, e, blk);
			} else {
				continue;
			}
			*tail = e;
			tail = &e->alt;
		}
	} else if ( rt_is(nd, "while") ) {
		n = node(rt, OP_WHILE, nd, up, blk);
		if ( n == NULL ) return NULL;
		(void)expr_of(rt, nd, "expr", &n->code, &n->ncode);
		c = rt_child(nd, "endExpr");
		if ( c != NULL ) {
			UH	buf[RT_EX_MAX];
			INT	k, m = rt_expr_code(c, buf, RT_EX_MAX);

			if ( m > 0 ) {
				n->code2 = (UH *)get(rt, sizeof(UH) * (SZ)m);
				if ( n->code2 != NULL ) {
					for ( k = 0; k < m; k++ ) n->code2[k] = buf[k];
					n->ncode2 = m;
				}
			}
		}
		c = rt_child(nd, "do");
		n->body = block(rt, ( c != NULL ) ? c->first : nd->first, n, blk);
	} else if ( rt_is(nd, "assign") ) {
		n = node(rt, OP_ASSIGN, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->i1 = rt_int(rt_attr(nd, "var"), 0);
		(void)expr_of(rt, nd, "expr", &n->code, &n->ncode);
	} else if ( rt_is(nd, "random") ) {
		n = node(rt, OP_RANDOM, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->i1 = rt_same(rt_attr(nd, "exclude"), "last") ? 1 : 0;
		n->body = block(rt, nd->first, n, blk);
	} else if ( rt_is(nd, "interpolate") ) {
		n = node(rt, OP_INTERP, nd, up, blk);
		if ( n == NULL ) return NULL;
		if ( !rt_ease_parse(rt_attr(nd, "function"), &n->ease) ) n->ease.kind = RT_EASE_LINEAR;
		n->i1 = rt_int(rt_attr(nd, "divisions"), 0);
		n->body = block(rt, nd->first, n, blk);
	} else if ( rt_is(nd, "unitSystem") ) {
		unit_system(rt, nd);
		return NULL;
	} else if ( rt_is(nd, "unitSpec") || rt_is(nd, "unitSpecTemp") ) {
		n = node(rt, rt_is(nd, "unitSpec") ? OP_USPEC : OP_USPECT, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->i1 = rt_int(rt_attr(nd, "unitId"), -1);
		n->i2 = rt_int(rt_attr(nd, "unitNumber"), -1);
	} else if ( rt_is(nd, "unitScale") ) {
		D	a = 1, b = 1;

		n = node(rt, OP_USCALE, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->i1 = rt_int(rt_attr(nd, "bunitId"), 2);
		n->i2 = rt_int(rt_attr(nd, "texp"), 0);
		if ( !rt_ratio(rt_attr(nd, "ratio"), &a, &b) || a <= 0 ) {
			a = 1;
			b = 1;
		}
		n->v[0] = a;
		n->v[1] = b;
	} else if ( rt_is(nd, "absoluteUnitCoeff") ) {
		D	a = 1, b = 1;

		n = node(rt, OP_UCOEFF, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->i1 = rt_int(rt_attr(nd, "unitId"), -1);
		if ( !rt_ratio(rt_attr(nd, "ratioSI"), &a, &b) || a <= 0 ) {
			a = 1;
			b = 1;
		}
		n->v[0] = a;
		n->v[1] = b;
	} else if ( rt_is(nd, "note") ) {
		n = node(rt, OP_NOTE, nd, up, blk);
		if ( n == NULL ) return NULL;
		rt->sounds = TRUE;
		n->i1 = rt_pitch(rt_attr(nd, "pitch"));
		n->i2 = rt_int(rt_attr(nd, "velocity"), 100);
		n->i3 = rt_int(rt_attr(nd, "channel"), 0);
		n->s1 = rt_attr(nd, "duration");
	} else if ( rt_is(nd, "midi") ) {
		INT	k = 0;

		n = node(rt, OP_MIDI, nd, up, blk);
		if ( n == NULL ) return NULL;
		rt->sounds = TRUE;
		n->code = (UH *)get(rt, sizeof(UH) * 3 * RT_MIDI_MAX);
		for ( c = nd->first; c != NULL && n->code != NULL && k < RT_MIDI_MAX; c = c->next ) {
			if ( rt_is(c, "message") ) {
				/* written in hexadecimal, as a MIDI message is read */
				CONST char	*f[3] = { "status", "data1", "data2" };
				INT		j;

				for ( j = 0; j < 3; j++ ) {
					CONST UB	*s = rt_attr(c, f[j]);
					INT		v = 0, q;

					for ( q = 0; s != NULL && s[q] != 0; q++ ) {
						UB	ch = s[q];

						if ( ch >= '0' && ch <= '9' ) v = v * 16 + ( ch - '0' );
						else if ( ch >= 'a' && ch <= 'f' ) v = v * 16 + ( ch - 'a' + 10 );
						else if ( ch >= 'A' && ch <= 'F' ) v = v * 16 + ( ch - 'A' + 10 );
						else if ( ch == 'x' || ch == 'X' ) v = 0;
					}
					n->code[k * 3 + j] = (UH)( v & 0xFF );
				}
				k++;
			}
		}
		n->ncode = k;
	} else if ( rt_is(nd, "pcm") ) {
		n = node(rt, OP_PCM, nd, up, blk);
		if ( n == NULL ) return NULL;
		rt->sounds = TRUE;
		n->i1 = ( rt_attr(nd, "href") != NULL ) ? media_decl(rt, nd, RT_MK_PCM) : -1;
		n->body = block(rt, nd->first, n, blk);		/* its <onerror> */
		if ( n->i1 >= 0 && rt->mnode[n->i1] == NULL ) rt->mnode[n->i1] = n;
	} else if ( rt_is(nd, "audio") || rt_is(nd, "video") ) {
		BOOL	v = rt_is(nd, "video");

		n = node(rt, v ? OP_VIDEO : OP_AUDIO, nd, up, blk);
		if ( n == NULL ) return NULL;
		if ( !v ) rt->sounds = TRUE;
		n->i1 = media_decl(rt, nd, v ? RT_MK_VIDEO : RT_MK_AUDIO);
		n->body = block(rt, nd->first, n, blk);		/* its <onerror> */
		if ( n->i1 >= 0 && rt->mnode[n->i1] == NULL ) rt->mnode[n->i1] = n;
	} else if ( rt_is(nd, "image") ) {
		CONST UB	*key = rt_attr(nd, "id");
		INT		k;

		if ( key == NULL ) key = rt_attr(nd, "href");
		n = node(rt, OP_IMAGE, nd, up, blk);
		if ( n == NULL ) return NULL;
		k = rt_obj_add(rt, key, RT_OB_IMAGE, nd);
		if ( k >= 0 && !rt->obj[k].bound ) {
			RT_OBJ	*o = &rt->obj[k];

			obj_from(o, nd);
			o->href = rt_attr(nd, "href");
			o->kind = RT_OB_IMAGE;
			o->bound = TRUE;
			o->shown = FALSE;
			obj_keep_base(o);
		}
		n->i1 = k;
	} else if ( rt_is(nd, "figureRef") ) {
		n = node(rt, OP_FIGREF, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "id");
		if ( n->s1 == NULL ) n->s1 = rt_attr(nd, "ref");
		n->v[0] = rt_val(rt_attr(nd, "x"), 0);
		n->v[1] = rt_val(rt_attr(nd, "y"), 0);
		want(rt, n->s1);
	} else if ( rt_is(nd, "audioControl") ) {
		if ( ( n = node(rt, OP_CTRL, nd, up, blk) ) != NULL ) ctrl(rt, n, CT_AUDIO);
	} else if ( rt_is(nd, "videoControl") ) {
		if ( ( n = node(rt, OP_CTRL, nd, up, blk) ) != NULL ) ctrl(rt, n, CT_VIDEO);
	} else if ( rt_is(nd, "imageControl") ) {
		if ( ( n = node(rt, OP_CTRL, nd, up, blk) ) != NULL ) ctrl(rt, n, CT_IMAGE);
	} else if ( rt_is(nd, "textControl") ) {
		if ( ( n = node(rt, OP_CTRL, nd, up, blk) ) != NULL ) ctrl(rt, n, CT_TEXT);
	} else if ( rt_is(nd, "shapeControl") ) {
		if ( ( n = node(rt, OP_CTRL, nd, up, blk) ) != NULL ) ctrl(rt, n, CT_SHAPE);
	} else if ( rt_is(nd, "groupControl") ) {
		if ( ( n = node(rt, OP_CTRL, nd, up, blk) ) != NULL ) ctrl(rt, n, CT_GROUP);
	} else if ( rt_is(nd, "triggerControl") ) {
		if ( ( n = node(rt, OP_CTRL, nd, up, blk) ) != NULL ) ctrl(rt, n, CT_TRIGGER);
	} else if ( rt_is(nd, "wait") ) {
		n = node(rt, OP_WAIT, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "ref");
		n->s2 = rt_attr(nd, "refs");
		n->s3 = rt_attr(nd, "event");
		n->s4 = rt_attr(nd, "target");
		want(rt, n->s4);
	} else if ( rt_is(nd, "keyframes") ) {
		n = node(rt, OP_KEYFR, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "ref");
		n->s2 = rt_attr(nd, "property");
		if ( !rt_ease_parse(rt_attr(nd, "function"), &n->ease) ) n->ease.kind = RT_EASE_LINEAR;
		if ( rt_media_of(rt, n->s1) < 0 || !rt_same(n->s2, "volume") ) want(rt, n->s1);
	} else if ( rt_is(nd, "trigger") ) {
		n = node(rt, OP_TRIGGER, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->s1 = rt_attr(nd, "event");
		n->s2 = rt_attr(nd, "target");
		n->s3 = rt_attr(nd, "key");
		n->s4 = rt_attr(nd, "id");
		want(rt, n->s2);
		n->body = block(rt, nd->first, n, blk);
	} else if ( rt_is(nd, "onerror") ) {
		n = node(rt, OP_ONERROR, nd, up, blk);
		if ( n == NULL ) return NULL;
		n->body = block(rt, nd->first, n, blk);
	} else if ( rt_is(nd, "stream") ) {
		if ( rt_room(rt, &rt->stream, &rt->cstream, rt->nstream + 1, sizeof(RT_STREAM)) ) {
			RT_STREAM	*s = &rt->stream[rt->nstream++];

			s->number = rt_int(rt_attr(nd, "number"), rt->nstream);
			s->device = rt_text(rt_child(nd, "deviceName"));
			s->params = rt_text(rt_child(nd, "deviceParams"));
			s->data = rt_text(rt_child(nd, "streamData"));
		}
		return NULL;
	}
	return n;
}

LOCAL RTN *block( RT *rt, CONST T_TADNODE *first, RTN *up, INT blk )
{
	RTN		*head = NULL, **tail = &head, *n;
	CONST T_TADNODE	*c;

	for ( c = first; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM ) continue;
		/* what describes the if, the while, the media is read by them;
		   a figure or a text is the scene's, not an event */
		if ( rt_is(c, "expr") || rt_is(c, "endExpr") || rt_is(c, "then") || rt_is(c, "elseif")
		  || rt_is(c, "else") || rt_is(c, "do") || rt_is(c, "fallback") || rt_is(c, "keyframe")
		  || rt_is(c, "message") || rt_is(c, "data") || rt_is(c, "figure")
		  || rt_is(c, "document") ) {
			continue;
		}
		n = element(rt, c, up, blk);
		if ( n == NULL ) continue;
		*tail = n;
		while ( n->next != NULL ) n = n->next;
		tail = &n->next;
	}
	return head;
}

/* ---------------------------------------------------------------- labels */

LOCAL RTN *label_in( RTN *n, CONST UB *name )
{
	RTN	*f;

	for ( ; n != NULL; n = n->next ) {
		if ( n->op == OP_LABEL && ( rt_eq(n->s1, name) || rt_eq(n->s2, name) ) ) return n;
		if ( ( f = label_in(n->body, name) ) != NULL ) return f;
		if ( ( f = label_in(n->alt, name) ) != NULL ) return f;
	}
	return NULL;
}

EXPORT RTN *rt_label_find( RT *rt, CONST UB *name )
{
	RTN	*f;

	if ( name == NULL ) return NULL;
	if ( rt->cur >= 0 && rt->cur < rt->nblk
	  && ( f = label_in(rt->blk[rt->cur].body, name) ) != NULL ) {
		return f;
	}
	return label_in(rt->pre, name);
}

/* ---------------------------------------------------------------- the whole */

EXPORT ER rt_compile( RT *rt )
{
	CONST T_TADNODE	*c;
	RTN		**pre = &rt->pre;
	INT		i;

	for ( c = rt->root->first; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM ) continue;
		if ( rt_is(c, "realData") ) {
			RTBLOCK		*b;
			CONST T_TADNODE	*e;
			CONST UB	*a;
			RTN		*n;

			if ( !rt_room(rt, &rt->blk, &rt->cblk, rt->nblk + 1, sizeof(RTBLOCK)) ) return E_NOMEM;
			b = &rt->blk[rt->nblk];
			b->nd = c;
			a = rt_attr(c, "autoplay");
			b->autoplay = ( a == NULL || rt_same(a, "inherit") ) ? -1 : rt_bool(a, TRUE) ? 1 : 0;
			b->delay_s = rt_attr(c, "startDelay");
			b->loop = rt_bool(rt_attr(c, "loop"), FALSE);
			b->count = rt_int(rt_attr(c, "loopCount"), 0);
			/* the unit systems are read before the events that use them */
			for ( e = c->first; e != NULL; e = e->next ) {
				if ( rt_is(e, "unitSystem") ) unit_system(rt, e);
			}
			b->body = block(rt, c->first, NULL, rt->nblk);
			for ( n = b->body; n != NULL; n = n->next ) {
				if ( n->op != OP_ONERROR ) continue;
				if ( !rt_room(rt, &rt->onerr, &rt->conerr, rt->nonerr + 1, sizeof(RTN *)) ) {
					rt->oom = TRUE;
					break;
				}
				if ( b->nonerr == 0 ) b->onerr0 = rt->nonerr;
				b->nonerr++;
				rt->onerr[rt->nonerr++] = n;
			}
			rt->nblk++;
		} else if ( rt_is(c, "audio") || rt_is(c, "video") || rt_is(c, "image")
			 || rt_is(c, "pcm") || rt_is(c, "trigger") || rt_is(c, "onerror") ) {
			/* written outside every <realData>: met at the start of the first */
			RTN	*n = element(rt, c, NULL, 0);

			if ( n != NULL ) {
				*pre = n;
				pre = &n->next;
			}
		} else if ( rt_is(c, "unitSystem") ) {
			unit_system(rt, c);
		}
	}
	if ( rt->nblk == 0 ) {
		/* only media outside: an empty block to play them in */
		if ( !rt_room(rt, &rt->blk, &rt->cblk, 1, sizeof(RTBLOCK)) ) return E_NOMEM;
		rt->blk[0].nd = NULL;
		rt->blk[0].autoplay = -1;
		rt->nblk = 1;
	}
	for ( i = 0; i < rt->nblk; i++ ) {
		rt->blk[i].delay = 0;
	}
	return rt->oom ? E_NOMEM : E_OK;
}

/* ---------------------------------------------------------------- finding it */

LOCAL CONST T_TADNODE *find_in( CONST T_TADNODE *nd, INT depth )
{
	CONST T_TADNODE	*c, *f;

	if ( nd == NULL || depth > 8 ) return NULL;
	if ( rt_is(nd, "realtime") ) return nd;
	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM ) continue;
		if ( ( f = find_in(c, depth + 1) ) != NULL ) return f;
	}
	return NULL;
}

EXPORT CONST T_TADNODE *rt_find( CONST T_TAD *doc )
{
	return ( doc != NULL ) ? find_in(tad_root(doc), 0) : NULL;
}

EXPORT INT rt_nmedia( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->nmedia : 0;
}

EXPORT RT_MEDIA *rt_media( RT *rt, INT i )
{
	return ( rt != NULL && i >= 0 && i < rt->nmedia ) ? &rt->media[i] : NULL;
}

EXPORT INT rt_nstream( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->nstream : 0;
}

EXPORT CONST RT_STREAM *rt_stream( CONST RT *rt, INT i )
{
	return ( rt != NULL && i >= 0 && i < rt->nstream ) ? &rt->stream[i] : NULL;
}

EXPORT CONST RT_STREAM *rt_stream_of( CONST RT *rt, INT number )
{
	INT	i;

	for ( i = 0; rt != NULL && i < rt->nstream; i++ ) {
		if ( rt->stream[i].number == number ) return &rt->stream[i];
	}
	return NULL;
}

EXPORT BOOL rt_sounds( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->sounds : FALSE;
}

EXPORT BOOL rt_autoplay( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->autoplay : FALSE;
}

EXPORT BOOL rt_loops( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->loop : FALSE;
}

EXPORT UINT rt_preload( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->preload : RT_PRE_META;
}
