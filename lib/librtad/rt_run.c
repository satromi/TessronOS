/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rt_run.c
 *	Running a program of 実時間制御TAD (design 15.4)
 *
 *	Each thread walks the nodes of one group at a time of its own, the
 *	time its next event happens at; a time setting moves that time on,
 *	and nothing else does. A parallel group is a thread that starts at
 *	the time of the one that met it and goes on beside it; a serial
 *	group is one that the thread that met it waits for, taking up its
 *	time where the group's ended. A step does, of everything due by the
 *	time asked, always the earliest first, so that what one thread
 *	writes into a variable is what a later event of another reads.
 *
 *	Events are done at their own time, not at the time the step happens
 *	to come: a sound carries the time it was due, so a step that comes
 *	late does not make the music late.
 *
 *	A label that a medium playing is to jump to when it ends (onended
 *	"jump:L") is a place to wait: the thread that reaches it stands
 *	there until the medium ends, and goes on from it then. That is what
 *	makes a list of media written one after another, each followed by
 *	the label its end jumps to, play one after another.
 */

#include "rt_local.h"

LOCAL void run_thread( RT *rt, INT i, RT_TIME T );
LOCAL void exec( RT *rt, INT i, RTN *n );
LOCAL void do_jump( RT *rt, INT i, CONST UB *target, RT_TIME T );
LOCAL void media_play( RT *rt, INT k, RT_TIME at, INT thr );
LOCAL void media_ended( RT *rt, INT k, RT_TIME T );
LOCAL void labels_check( RT *rt, RT_TIME T );
LOCAL void anim_apply( RT *rt, RTANIM *a, RT_TIME T );

#define Q	65536

/* ---------------------------------------------------------------- telling */

LOCAL void say( RT *rt, CONST char *a, CONST UB *b )
{
	char	*line;
	INT	n = 0, i, la = 0, lb = 0;

	if ( rt->host.log == NULL ) return;
	while ( a != NULL && a[la] != 0 ) la++;
	while ( b != NULL && b[lb] != 0 ) lb++;
	line = (char *)rt->host.alloc(rt->host.arg, (SZ)la + (SZ)lb + 1);
	if ( line == NULL ) {
		/* no room for the whole line: what a says */
		rt->host.log(rt->host.arg, ( a != NULL ) ? a : "realtime");
		return;
	}
	for ( i = 0; i < la; i++ ) line[n++] = a[i];
	for ( i = 0; i < lb; i++ ) line[n++] = (char)b[i];
	line[n] = 0;
	rt->host.log(rt->host.arg, line);
	rt->host.free(rt->host.arg, line);
}

LOCAL void mop( RT *rt, INT k, UINT op, RT_V v )
{
	if ( rt->host.media != NULL ) rt->host.media(rt->host.arg, rt, k, op, v);
}

/* ---------------------------------------------------------------- units */

LOCAL INT usys_index( RT *rt, INT id )
{
	INT	i;

	for ( i = 0; i < rt->nusys; i++ ) {
		if ( rt->usys[i].id == id ) return i;
	}
	return -1;
}

EXPORT RT_TIME rt_utime( RT *rt, RTUNIT *u, CONST UB *value, CONST UB *unit )
{
	RT_V	v;
	RT_TIME	us;
	INT	sys, aux, k;

	if ( !rt_num(value, &v) ) return 0;
	if ( unit != NULL && !rt_same_ci(unit, "tick") ) {
		/* a unit of its own: v is the value in millionths */
		if ( rt_same_ci(unit, "us") || rt_same(unit, "\xCE\xBCs") ) us = v / 1000000;
		else if ( rt_same_ci(unit, "ms") ) us = v / 1000;
		else if ( rt_same_ci(unit, "s") || rt_same_ci(unit, "sec") ) us = v;
		else if ( rt_same_ci(unit, "min") ) us = v * 60;
		else us = v / 1000;
		return us;
	}
	sys = u->sys;
	aux = u->aux;
	if ( u->tsys != -2 ) {
		sys = u->tsys;
		aux = u->taux;
	}
	if ( sys < 0 || sys >= rt->nusys ) {
		/* no unit system said: milliseconds; a "tick" is 1/960 second
		   (480 to a crotchet at 120 a minute) */
		us = ( unit != NULL ) ? rt_muldiv(v, 1000, 960000) : v / 1000;
	} else {
		RTUSYS	*s = &rt->usys[sys];

		/* v millionths of the unit, the unit pn/pd seconds: microseconds */
		us = rt_muldiv(v, s->pn, s->pd);
		for ( k = 0; k < s->naux; k++ ) {
			if ( s->auxno[k] == aux ) {
				us = rt_muldiv(us, s->an[k], s->ad[k]);
				break;
			}
		}
	}
	if ( u->sn > 0 && u->sd > 0 && ( u->sn != 1 || u->sd != 1 ) ) {
		us = rt_muldiv(us, u->sn, u->sd);
	}
	return us;
}

LOCAL void unit_reset( RTUNIT *u )
{
	u->sys = -1;
	u->aux = -1;
	u->tsys = -2;
	u->taux = -1;
	u->sn = 1;
	u->sd = 1;
}

EXPORT RT_TIME rt_time_of( RT *rt, CONST UB *value, CONST UB *unit )
{
	RTUNIT	u;
	INT	i;

	unit_reset(&u);
	for ( i = 0; i < rt->cthr; i++ ) {
		if ( rt->thr[i]->st != TS_FREE ) {
			u = rt->thr[i]->u;
			break;
		}
	}
	return rt_utime(rt, &u, value, unit);
}

/* ---------------------------------------------------------------- expressions */

LOCAL INT sysvar( RT *rt, INT id )
{
	INT	i, n = 0;

	switch ( id ) {
	case RT_SV_TIME:	return (INT)( ( rt->now / 1000 ) & 0x7FFF );
	case RT_SV_RANDOM:	return (INT)( rt_rand(rt) >> 17 );
	case RT_SV_KEY:		return rt->last_key;
	case RT_SV_X:		return rt->ptr_x;
	case RT_SV_Y:		return rt->ptr_y;
	case RT_SV_PLAYING:
		for ( i = 0; i < rt->nmedia; i++ ) {
			if ( rt->media[i].state == RT_MS_PLAYING ) n++;
		}
		return n;
	case RT_SV_SLIDE:	return rt->slide;
	case RT_SV_BLOCK:	return rt->cur + 1;
	default:		return 0;
	}
}

EXPORT INT rt_var( CONST RT *rt, INT id )
{
	if ( rt == NULL ) return 0;
	if ( id > 0 && id < RT_NVAR && rt->var != NULL ) return rt->var[id];
	if ( id >= 0x4000 && id <= 0x7FFF ) return sysvar((RT *)rt, id);
	return 0;
}

EXPORT void rt_set_var( RT *rt, INT id, INT v )
{
	if ( rt != NULL && id > 0 && id < RT_NVAR && rt->var != NULL ) rt->var[id] = (H)v;
	if ( rt != NULL && id == RT_SV_SLIDE ) rt->slide = v;
}

LOCAL INT w16( INT v )
{
	return (INT)(H)(UH)( v & 0xFFFF );
}

EXPORT INT rt_expr_eval( RT *rt, CONST UH *code, INT n )
{
	INT	st[RT_EX_MAX], sp = 0, i, a, b;

	for ( i = 0; i < n; i++ ) {
		UINT	c = code[i];

		if ( c == RT_EX_CONST ) {
			if ( i + 1 >= n ) break;
			if ( sp < RT_EX_MAX ) st[sp++] = w16((INT)code[++i]);
			continue;
		}
		if ( c < 0x8000 ) {
			if ( sp < RT_EX_MAX ) st[sp++] = w16(rt_var(rt, (INT)c));
			continue;
		}
		/* the unary operators take one, the stack ones work on it */
		if ( ( c >= 0x8030 && c <= 0x8045 ) || c == 0x8050 ) {
			if ( sp < 1 ) return 0;
			a = st[sp - 1];
			switch ( c ) {
			case 0x8030: st[sp - 1] = ( a == 0 ) ? 1 : 0; break;
			case 0x8031: st[sp - 1] = w16(-a); break;
			case 0x8040: st[sp - 1] = ( a < 0 ) ? 1 : 0; break;
			case 0x8041: st[sp - 1] = ( a > 0 ) ? 1 : 0; break;
			case 0x8042: st[sp - 1] = ( a <= 0 ) ? 1 : 0; break;
			case 0x8043: st[sp - 1] = ( a >= 0 ) ? 1 : 0; break;
			case 0x8044: st[sp - 1] = ( a == 0 ) ? 1 : 0; break;
			case 0x8045: st[sp - 1] = ( a != 0 ) ? 1 : 0; break;
			case 0x8050: if ( sp < RT_EX_MAX ) st[sp++] = a; break;
			default: break;
			}
			continue;
		}
		if ( c == 0x8052 ) {
			/* pick: the value N below the top, N taken off first */
			if ( sp < 1 ) return 0;
			a = st[--sp];
			if ( a < 0 || a >= sp ) return 0;
			st[sp] = st[sp - 1 - a];
			sp++;
			continue;
		}
		if ( sp < 2 ) return 0;
		b = st[--sp];
		a = st[sp - 1];
		switch ( c ) {
		case 0x8000: a = w16(a + b); break;
		case 0x8001: a = w16(a - b); break;
		case 0x8002: a = w16(a * b); break;
		case 0x8003: a = ( b == 0 ) ? 0 : w16(a / b); break;
		case 0x8004: a = ( b == 0 ) ? 0 : w16(a % b); break;
		case 0x8010: a = ( a < b ); break;
		case 0x8011: a = ( a > b ); break;
		case 0x8012: a = ( a <= b ); break;
		case 0x8013: a = ( a >= b ); break;
		case 0x8014: a = ( a == b ); break;
		case 0x8015: a = ( a != b ); break;
		case 0x8020: a = ( a != 0 && b != 0 ); break;
		case 0x8021: a = ( a != 0 || b != 0 ); break;
		case 0x8051: st[sp - 1] = b; st[sp++] = a; continue;
		default: return 0;
		}
		st[sp - 1] = a;
	}
	return ( sp > 0 ) ? st[sp - 1] : 0;
}

/* An if's or a while's condition: none is always true */
LOCAL BOOL cond( RT *rt, CONST UH *code, INT n )
{
	return (BOOL)( n <= 0 || rt_expr_eval(rt, code, n) != 0 );
}

/* ---------------------------------------------------------------- objects */

LOCAL RT_V obj_get( RT_OBJ *o, INT prop )
{
	switch ( prop ) {
	case PM_X:	return o->p[RT_P_LEFT];
	case PM_Y:	return o->p[RT_P_TOP];
	case PM_R:	return o->p[RT_P_RIGHT];
	case PM_B:	return o->p[RT_P_BOTTOM];
	case PM_SCALE:	return o->p[RT_P_SX];
	default:	return ( prop >= 0 && prop < RT_NP ) ? o->p[prop] : 0;
	}
}

LOCAL void obj_set( RT_OBJ *o, INT prop, RT_V v )
{
	RT_V	w = o->p[RT_P_RIGHT] - o->p[RT_P_LEFT];
	RT_V	h = o->p[RT_P_BOTTOM] - o->p[RT_P_TOP];

	switch ( prop ) {
	case PM_X:
		if ( o->p[RT_P_LEFT] == v ) return;
		o->p[RT_P_LEFT] = v;
		o->p[RT_P_RIGHT] = v + w;
		o->dirty |= RT_D_BOX;
		return;
	case PM_Y:
		if ( o->p[RT_P_TOP] == v ) return;
		o->p[RT_P_TOP] = v;
		o->p[RT_P_BOTTOM] = v + h;
		o->dirty |= RT_D_BOX;
		return;
	case PM_R:
		if ( o->p[RT_P_RIGHT] == v ) return;
		o->p[RT_P_RIGHT] = v;
		o->p[RT_P_LEFT] = v - w;
		o->dirty |= RT_D_BOX;
		return;
	case PM_B:
		if ( o->p[RT_P_BOTTOM] == v ) return;
		o->p[RT_P_BOTTOM] = v;
		o->p[RT_P_TOP] = v - h;
		o->dirty |= RT_D_BOX;
		return;
	case PM_SCALE:
		if ( o->p[RT_P_SX] == v && o->p[RT_P_SY] == v ) return;
		o->p[RT_P_SX] = o->p[RT_P_SY] = v;
		o->dirty |= RT_D_LOOK;
		return;
	default:
		if ( prop < 0 || prop >= RT_NP || o->p[prop] == v ) return;
		o->p[prop] = v;
		o->dirty |= ( prop <= RT_P_BOTTOM ) ? RT_D_BOX : RT_D_LOOK;
		return;
	}
}

LOCAL void obj_show( RT_OBJ *o, BOOL on )
{
	if ( o->shown != on ) {
		o->shown = on;
		o->dirty |= RT_D_SHOWN;
	}
}

/* Is a point of the scene (millionths) on the object? */
LOCAL BOOL obj_hit( CONST RT_OBJ *o, RT_V x, RT_V y )
{
	RT_V	cx = ( o->p[RT_P_LEFT] + o->p[RT_P_RIGHT] ) / 2;
	RT_V	cy = ( o->p[RT_P_TOP] + o->p[RT_P_BOTTOM] ) / 2;
	RT_V	hw = rt_muldiv(( o->p[RT_P_RIGHT] - o->p[RT_P_LEFT] ) / 2, o->p[RT_P_SX], RT_ONE);
	RT_V	hh = rt_muldiv(( o->p[RT_P_BOTTOM] - o->p[RT_P_TOP] ) / 2, o->p[RT_P_SY], RT_ONE);

	if ( hw < 0 ) hw = -hw;
	if ( hh < 0 ) hh = -hh;
	return (BOOL)( o->shown && x >= cx - hw && x < cx + hw && y >= cy - hh && y < cy + hh );
}

/* ---------------------------------------------------------------- threads */

LOCAL INT thr_new( RT *rt, INT parent, RTN *first, RTN *group, UINT kind, RTN *frnode, RT_TIME t )
{
	INT	i;
	RTTHR	*th;

	for ( i = 0; i < rt->cthr; i++ ) {
		if ( rt->thr[i]->st == TS_FREE ) break;
	}
	if ( i == rt->cthr && !rt_room_slots(rt, &rt->thr, &rt->cthr, i + 1, sizeof(RTTHR)) && i >= rt->cthr ) {
		say(rt, "realtime: not enough memory for another group", NULL);
		return -1;
	}
	th = rt->thr[i];
	{
		RTFRAME	*fr = th->fr;
		INT	cfr = th->cfr;

		rt_zero(th, sizeof(*th));
		th->fr = fr;
		th->cfr = cfr;
	}
	if ( !rt_frames(rt, th, 1) ) {
		say(rt, "realtime: not enough memory for another group", NULL);
		return -1;
	}
	th->st = TS_RUN;
	th->pc = first;
	th->nfr = 1;
	th->fr[0].kind = kind;
	th->fr[0].node = frnode;
	th->t = t;
	th->t0 = t;
	th->parent = parent;
	th->child = -1;
	th->group = group;
	th->block = rt->cur;
	th->speed = RT_ONE;
	th->until = RT_NEVER;
	unit_reset(&th->u);
	if ( parent >= 0 ) {
		RTTHR	*p = rt->thr[parent];

		th->stream = p->stream;
		th->u = p->u;
		th->u.tsys = -2;
		th->speed = p->speed;
		th->ff_until = p->ff_until;
		th->trig = p->trig;
		th->block = p->block;
	}
	th->gen = ++rt->gen;
	return i;
}

LOCAL void thr_end( RT *rt, INT i )
{
	RTTHR	*th = rt->thr[i];
	INT	p = th->parent, k;

	if ( th->st == TS_FREE ) return;
	if ( p >= 0 && rt->thr[p]->st == TS_CHILD && rt->thr[p]->child == i ) {
		/* a serial group's time is the time of the one that met it */
		rt->thr[p]->st = TS_RUN;
		rt->thr[p]->child = -1;
		if ( th->t > rt->thr[p]->t ) rt->thr[p]->t = th->t;
	}
	for ( k = 0; k < rt->cthr; k++ ) {
		if ( rt->thr[k]->st != TS_FREE && rt->thr[k]->parent == i ) rt->thr[k]->parent = p;
	}
	th->st = TS_FREE;
	/* a group that ends may have been what a label was waited at for */
	if ( th->group != NULL ) labels_check(rt, rt->now);
}

/* Is thread i the thread a, or started from it? */
LOCAL BOOL descends( RT *rt, INT i, INT a )
{
	INT	n = 0;

	while ( i >= 0 && n++ < rt->cthr ) {
		if ( i == a ) return TRUE;
		i = rt->thr[i]->parent;
	}
	return FALSE;
}

/* A thread, with every thread started from it, ended without going on */
LOCAL void thr_kill( RT *rt, INT a )
{
	INT	k;

	for ( k = 0; k < rt->cthr; k++ ) {
		if ( k != a && rt->thr[k]->st != TS_FREE && descends(rt, k, a) ) {
			rt->thr[k]->parent = -1;
			rt->thr[k]->st = TS_FREE;
		}
	}
	for ( k = 0; k < rt->canim; k++ ) {
		if ( rt->anim[k]->used && rt->anim[k]->thr == a ) rt->anim[k]->thr = -1;
	}
	rt->thr[a]->parent = -1;		/* nobody waits for it */
	rt->thr[a]->st = TS_FREE;
}

LOCAL BOOL push( RT *rt, RTTHR *th, UINT kind, RTN *node, RTN *ret, RTN *first, RTN *end )
{
	RTFRAME	*f;

	if ( !rt_frames(rt, th, th->nfr + 1) ) {
		say(rt, "realtime: not enough memory for a deeper block", NULL);
		return FALSE;
	}
	f = &th->fr[th->nfr++];
	f->kind = kind;
	f->node = node;
	f->ret = ret;
	f->end = end;
	f->count = 0;
	th->pc = first;
	return TRUE;
}

/* The end of the block a thread is in: what follows it */
LOCAL void block_end( RT *rt, INT i )
{
	RTTHR	*th = rt->thr[i];
	RTFRAME	*f = &th->fr[th->nfr - 1];

	switch ( f->kind ) {
	case FR_WHILE: {
		RTN	*n = f->node;
		BOOL	go = cond(rt, n->code2, n->ncode2) && cond(rt, n->code, n->ncode);

		if ( go ) {
			f->count++;
			th->pc = n->body;
			if ( n->body == NULL ) {
				/* nothing inside: once round is all it does at this time */
				th->pc = f->ret;
				th->nfr--;
			}
		} else {
			th->pc = f->ret;
			th->nfr--;
		}
		return;
	}
	case FR_BLOCK:
		th->pc = f->ret;
		th->nfr--;
		return;
	default:
		thr_end(rt, i);
		return;
	}
}

/* ---------------------------------------------------------------- sounds */

LOCAL void sound_out( RT *rt, RT_SND *s, RT_TIME t, INT thr )
{
	RTTHR	*th = ( thr >= 0 ) ? rt->thr[thr] : NULL;

	if ( rt->ffwd && t < rt->ffwd_to ) return;		/* past, in a seek */
	if ( th != NULL && t < th->ff_until ) return;
	s->at = t + rt->origin;
	if ( s->kind == RT_SND_NOTE && t + s->dur > rt->sound_until ) rt->sound_until = t + s->dur;
	if ( rt->host.sound != NULL ) rt->host.sound(rt->host.arg, rt, s);
}

/* A sound for later (an interpolation's notes) */
LOCAL void sound_later( RT *rt, CONST RT_SND *s, RT_TIME t, INT thr )
{
	if ( !rt_room(rt, &rt->sndq, &rt->csndq, rt->nsndq + 1, sizeof(RTSNDQ)) ) {
		say(rt, "realtime: not enough memory for a sound to wait", NULL);
		return;
	}
	rt->sndq[rt->nsndq].s = *s;
	rt->sndq[rt->nsndq].t = t;
	rt->sndq[rt->nsndq].thr = thr;
	rt->nsndq++;
}

LOCAL RT_TIME note_dur( RT *rt, RTTHR *th, RTN *n )
{
	RT_TIME	d = ( n->s1 != NULL ) ? rt_utime(rt, &th->u, n->s1, NULL) : 0;

	return ( d > 0 ) ? d : 500000;
}

LOCAL void note_out( RT *rt, INT i, RTN *n, INT pitch, INT vel, RT_TIME dur, RT_TIME t )
{
	RT_SND	s;

	if ( pitch < 0 ) return;
	rt_zero(&s, sizeof(s));
	s.kind = RT_SND_NOTE;
	s.stream = rt->thr[i]->stream;
	s.pitch = pitch;
	s.velocity = ( vel < 0 ) ? 0 : ( vel > 127 ) ? 127 : vel;
	s.channel = n->i3;
	s.dur = dur;
	s.nd = n->nd;
	if ( t <= rt->now ) sound_out(rt, &s, t, i);
	else sound_later(rt, &s, t, i);
}

/* ---------------------------------------------------------------- animations */

LOCAL RTANIM *anim_new( RT *rt, UINT kind, INT target, INT prop, INT thr, RT_TIME t0 )
{
	INT	i;

	/* one change of a property at a time: a new one takes over from the old */
	for ( i = 0; i < rt->canim; i++ ) {
		RTANIM	*a = rt->anim[i];

		if ( a->used && a->kind == kind && a->target == target
		  && ( a->prop == prop || kind == AN_BOX
		    || ( kind == AN_OBJ && ( ( prop == PM_X && a->prop == PM_R )
					   || ( prop == PM_R && a->prop == PM_X )
					   || ( prop == PM_Y && a->prop == PM_B )
					   || ( prop == PM_B && a->prop == PM_Y ) ) ) ) ) {
			a->used = FALSE;
		}
	}
	for ( i = 0; i < rt->canim; i++ ) {
		if ( !rt->anim[i]->used ) break;
	}
	if ( i == rt->canim && !rt_room_slots(rt, &rt->anim, &rt->canim, i + 1, sizeof(RTANIM)) && i >= rt->canim ) {
		say(rt, "realtime: not enough memory for another change", NULL);
		return NULL;
	}
	{
		RTANIM	*a = rt->anim[i];
		RT_V	*val = a->val;
		INT	*off = a->off;
		RT_EASE	*ease = a->ease;
		INT	ckey = a->ckey;

		rt_zero(a, sizeof(*a));
		a->val = val;
		a->off = off;
		a->ease = ease;
		a->ckey = ckey;
		rt_zero(a->val, sizeof(RT_V) * ckey);
		rt_zero(a->off, sizeof(INT) * ckey);
		rt_zero(a->ease, sizeof(RT_EASE) * ckey);
		if ( !rt_anim_keys(rt, a, 2) ) {
			say(rt, "realtime: not enough memory for another change", NULL);
			return NULL;
		}
		a->used = TRUE;
		a->kind = kind;
		a->target = target;
		a->prop = prop;
		a->thr = thr;
		a->t0 = t0;
		a->iter = 1;
		a->fill = FILL_FWD;
		a->last = -1;
		return a;
	}
}

/* A value along a run of keyframes at p (65536ths of one time through) */
LOCAL RT_V kf_value( CONST RTANIM *a, INT p )
{
	INT	k, q;

	if ( a->n <= 0 ) return 0;
	if ( a->n == 1 || p <= a->off[0] ) return a->val[0];
	if ( p >= a->off[a->n - 1] ) return a->val[a->n - 1];
	for ( k = 0; k + 1 < a->n && p > a->off[k + 1]; k++ ) ;
	if ( a->off[k + 1] <= a->off[k] ) return a->val[k + 1];
	q = (INT)( (D)( p - a->off[k] ) * Q / ( a->off[k + 1] - a->off[k] ) );
	if ( a->steps > 0 ) {
		/* an interpolation makes its values by divisions, each held a step */
		q = (INT)( (D)( q * a->steps / Q ) * Q / a->steps );
	}
	if ( a->spline && a->n >= 3 ) {
		/* Catmull-Rom through the points, the ends taken twice */
		D	p0 = a->val[( k > 0 ) ? k - 1 : k];
		D	p1 = a->val[k], p2 = a->val[k + 1];
		D	p3 = a->val[( k + 2 < a->n ) ? k + 2 : k + 1];
		D	t = q, t2 = t * t / Q, t3 = t2 * t / Q;

		return p1 + ( ( p2 - p0 ) * t / Q
			   + ( 2 * p0 - 5 * p1 + 4 * p2 - p3 ) * t2 / Q
			   + ( 3 * p1 - p0 - 3 * p2 + p3 ) * t3 / Q ) / 2;
	}
	if ( a->ease[k + 1].kind != RT_EASE_LINEAR ) q = rt_ease(&a->ease[k + 1], q);
	else q = rt_ease(&a->all, q);
	return a->val[k] + rt_muldiv(a->val[k + 1] - a->val[k], q, Q);
}

LOCAL void anim_put( RT *rt, RTANIM *a, RT_V v, BOOL last )
{
	if ( a->kind == AN_OBJ ) {
		if ( a->target >= 0 && a->target < rt->nobj ) obj_set(&rt->obj[a->target], a->prop, v);
	} else if ( a->kind == AN_BOX ) {
		RT_OBJ	*o;
		INT	k;

		if ( a->target < 0 || a->target >= rt->nobj ) return;
		o = &rt->obj[a->target];
		/* v is the way along, 0 .. RT_ONE */
		for ( k = 0; k < 4; k++ ) {
			RT_V	b = a->box[k] + rt_muldiv(a->box2[k] - a->box[k], v, RT_ONE);

			if ( o->p[k] != b ) {
				o->p[k] = b;
				o->dirty |= RT_D_BOX;
			}
		}
	} else if ( a->kind == AN_MEDIA ) {
		RT_MEDIA	*m;
		RT_V		d = v - a->last;

		if ( a->target < 0 || a->target >= rt->nmedia ) return;
		m = &rt->media[a->target];
		if ( a->prop == MP_VOLUME ) m->volume = v;
		else if ( a->prop == MP_PAN ) m->pan = v;
		else m->rate = v;
		if ( d < 0 ) d = -d;
		if ( last || a->last < 0 || d >= 5000 ) {
			a->last = v;
			mop(rt, a->target, ( a->prop == MP_VOLUME ) ? RT_MO_VOLUME
				     : ( a->prop == MP_PAN ) ? RT_MO_PAN : RT_MO_RATE, v);
		}
	}
}

LOCAL void anim_apply( RT *rt, RTANIM *a, RT_TIME T )
{
	RT_TIME	local, total;
	INT	cyc, p;
	BOOL	back;

	local = ( a->held_at != 0 ) ? a->held_at - a->t0 : T - a->t0;
	if ( local < 0 ) {
		if ( a->fill == FILL_BACK || a->fill == FILL_BOTH ) anim_put(rt, a, a->val[0], FALSE);
		return;
	}
	if ( a->dur <= 0 ) {
		anim_put(rt, a, a->val[a->n - 1], TRUE);
		a->used = FALSE;
		return;
	}
	total = ( a->iter > 0 ) ? a->dur * a->iter : RT_NEVER;
	if ( local >= total ) {
		/* the end: held there, or as it was before */
		cyc = a->iter - 1;
		back = (BOOL)( a->dir == DIR_REVERSE || ( a->dir == DIR_ALT && ( cyc & 1 ) )
			    || ( a->dir == DIR_ALTREV && !( cyc & 1 ) ) );
		if ( a->rev ) back = !back;
		if ( a->fill == FILL_FWD || a->fill == FILL_BOTH ) {
			anim_put(rt, a, back ? a->val[0] : a->val[a->n - 1], TRUE);
		} else {
			anim_put(rt, a, a->before, TRUE);
		}
		if ( a->stop_at_end && a->kind == AN_MEDIA && a->target >= 0 ) {
			RT_MEDIA	*m = &rt->media[a->target];

			if ( m->state == RT_MS_PLAYING || m->state == RT_MS_PAUSED ) {
				mop(rt, a->target, RT_MO_STOP, 0);
				m->state = RT_MS_STOPPED;
				labels_check(rt, T);
			}
		}
		a->used = FALSE;
		return;
	}
	cyc = (INT)( local / a->dur );
	p = (INT)( ( local % a->dur ) * Q / a->dur );
	back = (BOOL)( a->dir == DIR_REVERSE || ( a->dir == DIR_ALT && ( cyc & 1 ) )
		    || ( a->dir == DIR_ALTREV && !( cyc & 1 ) ) );
	if ( a->rev ) back = !back;
	if ( back ) p = Q - p;
	anim_put(rt, a, kf_value(a, p), FALSE);
}

/* ---------------------------------------------------------------- media */

LOCAL BOOL media_live( CONST RT_MEDIA *m )
{
	return (BOOL)( m->state == RT_MS_LOADING || m->state == RT_MS_READY
		    || m->state == RT_MS_PLAYING || m->state == RT_MS_PAUSED );
}

/* An element of audio or video met: made ready, and played when its trigger says */
LOCAL void media_meet( RT *rt, INT i, RTN *n, RT_TIME T )
{
	RTTHR		*th = rt->thr[i];
	RT_MEDIA	*m;
	BOOL		now;

	if ( n->i1 < 0 || n->i1 >= rt->nmedia ) return;
	m = &rt->media[n->i1];
	if ( media_live(m) ) {
		mop(rt, n->i1, RT_MO_STOP, 0);
	}
	rt_media_attrs(rt, m, n->nd, &th->u);
	rt->mnode[n->i1] = n;
	m->gen++;
	m->nfall = 0;
	m->tries = 0;
	m->err = 0;
	m->loaded = FALSE;
	m->retry_at = 0;
	m->pos = 0;
	m->thr = i;
	m->skip = ( th->ff_until > th->t ) ? th->ff_until - th->t
		: ( rt->ffwd && rt->ffwd_to > th->t ) ? rt->ffwd_to - th->t : 0;
	if ( m->obj >= 0 ) {
		RT_OBJ		*o = &rt->obj[m->obj];
		CONST T_TADNODE	*nd = n->nd;

		o->p[RT_P_LEFT] = rt_val(rt_attr(nd, "left"), o->p[RT_P_LEFT]);
		o->p[RT_P_TOP] = rt_val(rt_attr(nd, "top"), o->p[RT_P_TOP]);
		o->p[RT_P_RIGHT] = rt_val(rt_attr(nd, "right"), o->p[RT_P_RIGHT]);
		o->p[RT_P_BOTTOM] = rt_val(rt_attr(nd, "bottom"), o->p[RT_P_BOTTOM]);
		o->p[RT_P_ALPHA] = rt_val(rt_attr(nd, "alpha"), o->p[RT_P_ALPHA]);
		o->p[RT_P_ROT] = rt_val(rt_attr(nd, "rotation"), o->p[RT_P_ROT]);
		if ( rt_attr(nd, "zIndex") != NULL ) o->z = rt_int(rt_attr(nd, "zIndex"), o->z);
		o->dirty |= RT_D_BOX | RT_D_LOOK | RT_D_Z;
		obj_show(o, TRUE);
	}
	now = (BOOL)( ( m->trigger == RT_TR_TIME && m->autoplay ) || m->trigger == RT_TR_LOAD
		   || ( m->trigger == RT_TR_VISIBLE && m->autoplay ) );
	m->play_on_load = now;
	m->play_at = th->t;
	if ( !now && m->preload == RT_PRE_NONE ) {
		/* not wanted yet: it is loaded when it is played */
		m->state = RT_MS_READY;
		return;
	}
	m->state = RT_MS_LOADING;
	m->asked = T;
	mop(rt, n->i1, RT_MO_LOAD, 0);
}

LOCAL void media_go( RT *rt, INT k )
{
	RT_MEDIA	*m = &rt->media[k];

	m->play_on_load = FALSE;
	m->state = RT_MS_PLAYING;
	if ( m->obj >= 0 ) obj_show(&rt->obj[m->obj], TRUE);
	mop(rt, k, RT_MO_PLAY, 0);
	m->skip = 0;
}

/* A medium played: by a control, an onended, a trigger */
LOCAL void media_play( RT *rt, INT k, RT_TIME at, INT thr )
{
	RT_MEDIA	*m;
	RTUNIT		u;

	if ( k < 0 || k >= rt->nmedia ) return;
	m = &rt->media[k];
	if ( m->state == RT_MS_PLAYING ) return;
	if ( m->state == RT_MS_PAUSED ) {
		m->state = RT_MS_PLAYING;
		mop(rt, k, RT_MO_RESUME, 0);
		return;
	}
	if ( m->state == RT_MS_IDLE || m->state == RT_MS_ENDED || m->state == RT_MS_STOPPED
	  || m->state == RT_MS_ERROR ) {
		/* never met, or met before and done: as its element says */
		unit_reset(&u);
		if ( thr >= 0 && rt->thr[thr]->st != TS_FREE ) u = rt->thr[thr]->u;
		rt_media_attrs(rt, m, m->nd, &u);
		m->gen++;
		m->nfall = 0;
		m->tries = 0;
		m->err = 0;
		m->loaded = FALSE;
		m->pos = 0;
		m->skip = 0;
	}
	m->play_at = at;
	if ( m->loaded ) {
		media_go(rt, k);
		return;
	}
	m->play_on_load = TRUE;
	if ( m->state != RT_MS_LOADING ) {
		m->state = RT_MS_LOADING;
		m->asked = rt->now;
		mop(rt, k, RT_MO_LOAD, 0);
	}
}

LOCAL void media_stop( RT *rt, INT k, RT_TIME T )
{
	RT_MEDIA	*m = &rt->media[k];

	if ( !media_live(m) ) return;
	mop(rt, k, RT_MO_STOP, 0);
	m->state = RT_MS_STOPPED;
	m->play_on_load = FALSE;
	labels_check(rt, T);
}

/* A handler's events run now, as a thread of their own */
LOCAL void handler_run( RT *rt, RTN *body, RTN *node, RT_TIME T )
{
	INT	c;

	if ( body == NULL ) return;
	c = thr_new(rt, -1, body, NULL, FR_TOP, node, T);
	if ( c < 0 ) return;
	rt->thr[c]->trig = TRUE;
	run_thread(rt, c, T);
}

/* Does an <onerror type> take this error? */
LOCAL BOOL err_takes( CONST UB *types, UINT err )
{
	INT	i = 0;

	if ( types == NULL ) return TRUE;
	while ( types[i] != 0 ) {
		UB	w[16];
		INT	n = 0;
		UINT	bit = 0;

		while ( types[i] == ' ' || types[i] == ',' ) i++;
		while ( types[i] != 0 && types[i] != ',' && types[i] != ' ' && n < 15 ) w[n++] = types[i++];
		w[n] = 0;
		if ( n == 0 ) break;
		if ( rt_same(w, "all") ) return TRUE;
		if ( rt_same(w, "network") ) bit = RT_ERR_NETWORK;
		else if ( rt_same(w, "decode") ) bit = RT_ERR_DECODE | RT_ERR_UNSUPP;
		else if ( rt_same(w, "notfound") ) bit = RT_ERR_NOTFOUND;
		else if ( rt_same(w, "timeout") ) bit = RT_ERR_TIMEOUT;
		else if ( rt_same(w, "aborted") ) bit = RT_ERR_ABORTED;
		if ( ( bit & err ) != 0 ) return TRUE;
	}
	return FALSE;
}

/* Does a <fallback condition> stand for this error? */
LOCAL BOOL fall_takes( CONST UB *c, UINT err )
{
	if ( c == NULL || rt_same(c, "error") ) return TRUE;
	if ( rt_same(c, "unsupported") ) return (BOOL)( ( err & ( RT_ERR_UNSUPP | RT_ERR_DECODE ) ) != 0 );
	if ( rt_same(c, "timeout") ) return (BOOL)( ( err & RT_ERR_TIMEOUT ) != 0 );
	return FALSE;			/* slow: nothing here is fetched over a slow line */
}

LOCAL void media_error( RT *rt, INT k, UINT err, RT_TIME T )
{
	RT_MEDIA	*m = &rt->media[k];
	CONST T_TADNODE	*c;
	INT		i, nf = 0;

	m->err = err;
	m->loaded = FALSE;
	/* the next fallback that stands for it */
	for ( c = ( m->nd != NULL ) ? m->nd->first : NULL; c != NULL; c = c->next ) {
		if ( !rt_is(c, "fallback") ) continue;
		if ( nf++ < m->nfall ) continue;
		m->nfall = nf;
		if ( fall_takes(rt_attr(c, "condition"), err) && rt_attr(c, "href") != NULL ) {
			m->href = rt_attr(c, "href");
			m->format = rt_attr(c, "format");
			m->state = RT_MS_LOADING;
			m->asked = T;
			say(rt, "realtime: trying ", m->href);
			mop(rt, k, RT_MO_LOAD, 0);
			return;
		}
	}
	m->state = RT_MS_ERROR;
	m->play_on_load = FALSE;
	say(rt, "realtime: medium failed: ", m->href);
	/* its own handler, tried again first if it says so */
	{
		RTN	*e = ( rt->mnode[k] != NULL ) ? rt->mnode[k]->body : NULL;

		for ( ; e != NULL; e = e->next ) {
			if ( e->op == OP_ONERROR && err_takes(rt_attr(e->nd, "type"), err) ) break;
		}
		if ( e != NULL ) {
			INT	retry = rt_int(rt_attr(e->nd, "retry"), 0);

			if ( m->tries < retry ) {
				RTUNIT	u;
				RT_TIME	d;

				unit_reset(&u);
				d = ( rt_attr(e->nd, "retryDelay") != NULL )
				  ? rt_utime(rt, &u, rt_attr(e->nd, "retryDelay"), NULL) : 1000000;
				m->tries++;
				m->retry_at = T + ( ( d > 0 ) ? d : 1 );
				m->state = RT_MS_LOADING;
				m->play_on_load = TRUE;
				return;
			}
			handler_run(rt, e->body, e, T);
			labels_check(rt, T);
			return;
		}
	}
	/* else the handlers of the whole <realData> */
	if ( rt->cur >= 0 && rt->cur < rt->nblk ) {
		RTBLOCK	*b = &rt->blk[rt->cur];

		for ( i = 0; i < b->nonerr; i++ ) {
			RTN	*e = rt->onerr[b->onerr0 + i];

			if ( err_takes(rt_attr(e->nd, "type"), err) ) {
				handler_run(rt, e->body, e, T);
				break;
			}
		}
	}
	labels_check(rt, T);
}

LOCAL void media_ev( RT *rt, INT k, UINT ev, RT_V v, RT_TIME T );

/* ---------------------------------------------------------------- waits */

/* The next id of a comma separated list: where it starts and how long it is (spaces round it left out) */
LOCAL CONST UB *next_id( CONST UB *s, CONST UB **p_id, INT *p_len )
{
	INT	n = 0;

	if ( s == NULL ) return NULL;
	while ( *s == ' ' || *s == ',' ) s++;
	if ( *s == 0 ) return NULL;
	*p_id = s;
	while ( s[n] != 0 && s[n] != ',' ) n++;
	s += n;
	while ( n > 0 && (*p_id)[n - 1] == ' ' ) n--;
	*p_len = n;
	return s;
}

/* The medium whose id is the n bytes at s, -1 none */
LOCAL INT media_of_span( CONST RT *rt, CONST UB *s, INT n )
{
	INT	i, k;

	for ( i = 0; i < rt->nmedia; i++ ) {
		CONST UB	*id = rt->media[i].id;

		if ( id == NULL ) continue;
		for ( k = 0; k < n && id[k] == s[k]; k++ ) ;
		if ( k == n && id[k] == 0 ) return i;
	}
	return -1;
}

LOCAL BOOL ended( CONST RT_MEDIA *m )
{
	return (BOOL)( m->state == RT_MS_ENDED || m->state == RT_MS_STOPPED || m->state == RT_MS_ERROR );
}

/* Has what a <wait> waits for come, of the media? */
LOCAL BOOL wait_met( RT *rt, RTTHR *th, RT_TIME T )
{
	RTN		*n = th->wait;
	CONST UB	*ev = n->s3;
	INT		k;

	if ( th->woken ) return TRUE;
	if ( rt_same(ev, "any") || rt_same(ev, "all") ) {
		BOOL		any = FALSE, all = TRUE, one = FALSE;
		CONST UB	*w, *s = ( n->s2 != NULL ) ? n->s2 : n->s1;
		INT		wl;

		while ( ( s = next_id(s, &w, &wl) ) != NULL ) {
			k = media_of_span(rt, w, wl);
			if ( k < 0 ) continue;
			one = TRUE;
			if ( ended(&rt->media[k]) && rt->media[k].gen > 0 ) any = TRUE;
			else all = FALSE;
		}
		return (BOOL)( one && ( rt_same(ev, "any") ? any : all ) );
	}
	k = rt_media_of(rt, n->s1);
	if ( k < 0 ) {
		/* a video's display object, a picture: nothing to wait for */
		return (BOOL)( n->s1 != NULL && !rt_same(ev, "click") && !rt_same(ev, "dblclick")
			    && !rt_same(ev, "keydown") && !rt_same(ev, "keyup") );
	}
	if ( rt_same(ev, "ended") ) return (BOOL)( ended(&rt->media[k]) && rt->media[k].gen > 0 );
	if ( rt_same(ev, "playing") ) return (BOOL)( rt->media[k].state == RT_MS_PLAYING );
	if ( rt_same(ev, "pause") ) return (BOOL)( rt->media[k].state == RT_MS_PAUSED );
	if ( rt_same(ev, "loaded") || rt_same(ev, "canplay") ) return rt->media[k].loaded;
	if ( rt_same(ev, "timeupdate") ) {
		RTUNIT	u;
		RT_TIME	at;

		unit_reset(&u);
		at = rt_utime(rt, &u, rt_attr(n->nd, "time"), NULL);
		return (BOOL)( rt->media[k].state == RT_MS_PLAYING && rt->media[k].pos >= at );
	}
	return FALSE;
}

LOCAL void waits_check( RT *rt, RT_TIME T )
{
	INT	i;

	for ( i = 0; i < rt->cthr; i++ ) {
		RTTHR	*th = rt->thr[i];

		if ( th->st != TS_WAIT || th->held ) continue;
		if ( wait_met(rt, th, T) || T >= th->until ) {
			th->st = TS_RUN;
			th->woken = FALSE;
			th->wait = NULL;
			if ( th->t < T ) th->t = T;
		}
	}
}

/* ---------------------------------------------------------------- labels */

LOCAL BOOL ends_at( CONST UB *onended, CONST RTN *label )
{
	CONST UB	*s = onended;
	INT		i;

	if ( s == NULL || s[0] != 'j' || s[1] != 'u' || s[2] != 'm' || s[3] != 'p' || s[4] != ':' ) {
		return FALSE;
	}
	s += 5;
	for ( i = 0; s[i] != 0 && s[i] != ' '; i++ ) ;
	if ( label->s1 != NULL ) {
		INT	k;

		for ( k = 0; k < i && label->s1[k] == s[k]; k++ ) ;
		if ( k == i && label->s1[k] == 0 ) return TRUE;
	}
	if ( label->s2 != NULL ) {
		INT	k;

		for ( k = 0; k < i && label->s2[k] == s[k]; k++ ) ;
		if ( k == i && label->s2[k] == 0 ) return TRUE;
	}
	return FALSE;
}

/* An element of audio or video from n on, or inside what follows, that ends by jumping to the label */
LOCAL BOOL ahead( CONST RTN *n, CONST RTN *label, INT depth )
{
	for ( ; n != NULL; n = n->next ) {
		if ( ( n->op == OP_AUDIO || n->op == OP_VIDEO || n->op == OP_PCM )
		  && ends_at(rt_attr(n->nd, "onended"), label) ) {
			return TRUE;
		}
		if ( n->op != OP_TRIGGER && n->op != OP_ONERROR && n->op != OP_AUDIO && n->op != OP_VIDEO
		  && ( ahead(n->body, label, depth + 1) || ahead(n->alt, label, depth + 1) ) ) {
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * A medium is to jump to this label when it ends: one playing or ready,
 * or one a group running now has still to start.
 */
LOCAL BOOL awaited( RT *rt, CONST RTN *label )
{
	INT	k, f;

	for ( k = 0; k < rt->nmedia; k++ ) {
		if ( media_live(&rt->media[k]) && ends_at(rt->media[k].onended, label) ) return TRUE;
	}
	for ( k = 0; k < rt->cthr; k++ ) {
		RTTHR	*th = rt->thr[k];

		if ( th->st == TS_FREE || th->st == TS_LABEL || th->trig || th->group == NULL ) continue;
		if ( ahead(th->pc, label, 0) ) return TRUE;
		for ( f = th->nfr - 1; f > 0; f-- ) {
			if ( th->fr[f].kind != FR_WHILE && ahead(th->fr[f].ret, label, 0) ) return TRUE;
		}
	}
	return FALSE;
}

/* The threads standing at a label nobody is to jump to any more go on */
LOCAL void labels_check( RT *rt, RT_TIME T )
{
	INT	i;

	for ( i = 0; i < rt->cthr; i++ ) {
		RTTHR	*th = rt->thr[i];
		RTN	*l;

		if ( th->st != TS_LABEL ) continue;
		/* the label is found again by its name; the thread goes on after it */
		l = rt_label_find(rt, th->label);
		if ( l == NULL || !awaited(rt, l) ) {
			th->st = TS_RUN;
			if ( th->t < T ) th->t = T;
		}
	}
}

/* ---------------------------------------------------------------- jumping */

/* Is node n inside the block of node b (NULL: the top of a <realData>)? */
LOCAL BOOL in_block( CONST RTN *n, CONST RTN *b )
{
	return (BOOL)( n->up == b );
}

/* Thread i's frame whose block holds label l; -1 none */
LOCAL INT frame_of( RTTHR *th, CONST RTN *l )
{
	INT	f;

	for ( f = th->nfr - 1; f >= 0; f-- ) {
		CONST RTN	*b = th->fr[f].node;

		/* the bottom of a top thread is the top of its block */
		if ( th->fr[f].kind == FR_TOP && b == NULL ) {
			if ( l->up == NULL ) return f;
			continue;
		}
		if ( th->fr[f].kind == FR_GROUP || th->fr[f].kind == FR_BLOCK || th->fr[f].kind == FR_WHILE
		  || th->fr[f].kind == FR_TOP ) {
			if ( in_block(l, b) ) return f;
			/* an if's branches hang from the if, an elseif's from it */
			if ( b != NULL && ( b->op == OP_ELSEIF || b->op == OP_ELSE ) && l->up == b ) return f;
		}
	}
	return -1;
}

/* Thread j goes on from just after label l, at time t */
LOCAL void go_from( RT *rt, INT j, INT f, RTN *l, RT_TIME t )
{
	RTTHR	*th = rt->thr[j];

	th->nfr = f + 1;
	th->pc = l->next;
	if ( th->fr[f].kind == FR_BLOCK && th->fr[f].end != NULL ) th->fr[f].end = NULL;
	th->st = TS_RUN;
	th->child = -1;
	th->wait = NULL;
	if ( th->t < t ) th->t = t;
}

LOCAL void jump_label( RT *rt, INT i, RTN *l, RT_TIME t )
{
	INT	j, f, k;

	/* the thread itself, or one it was started from */
	for ( j = i; j >= 0; j = rt->thr[j]->parent ) {
		f = frame_of(rt->thr[j], l);
		if ( f >= 0 ) {
			/* those between end: their groups are left */
			for ( k = i; k >= 0 && k != j; ) {
				INT	p = rt->thr[k]->parent;

				thr_kill(rt, k);
				k = p;
			}
			go_from(rt, j, f, l, t);
			return;
		}
	}
	/* elsewhere: the thread whose block it is, or the top made to go on there */
	for ( j = 0; j < rt->cthr; j++ ) {
		if ( rt->thr[j]->st == TS_FREE || rt->thr[j]->trig ) continue;
		f = frame_of(rt->thr[j], l);
		if ( f >= 0 ) {
			if ( i >= 0 && i != j && rt->thr[i]->trig ) thr_end(rt, i);
			go_from(rt, j, f, l, t);
			return;
		}
	}
	if ( l->up == NULL ) {
		j = thr_new(rt, -1, l->next, NULL, FR_TOP, NULL, t);
		if ( j >= 0 ) {
			rt->thr[j]->block = l->block;
			if ( i >= 0 && rt->thr[i]->trig && rt->thr[i]->st != TS_FREE ) thr_end(rt, i);
		}
		return;
	}
	say(rt, "realtime: no way to the label ", l->s1);
}

LOCAL void do_jump( RT *rt, INT i, CONST UB *target, RT_TIME T )
{
	RTTHR	*th = ( i >= 0 ) ? rt->thr[i] : NULL;
	RTN	*l;
	RT_TIME	t = ( th != NULL ) ? th->t : T;

	if ( target == NULL ) return;
	if ( th != NULL && ( rt_same(target, "start") || rt_same(target, "0xFFFE") ) ) {
		/* the group again from its start */
		th->nfr = 1;
		th->pc = ( th->fr[0].node != NULL ) ? th->fr[0].node->body
		       : ( th->block >= 0 && th->block < rt->nblk ) ? rt->blk[th->block].body : NULL;
		th->t0 = th->t;
		return;
	}
	if ( th != NULL && ( rt_same(target, "end") || rt_same(target, "0xFFFF") ) ) {
		thr_end(rt, i);
		return;
	}
	l = rt_label_find(rt, target);
	if ( l == NULL ) {
		/* in a slide show, the slides' own moves */
		if ( rt_same(target, "next_slide") ) {
			if ( rt->host.action != NULL ) rt->host.action(rt->host.arg, rt, RT_ACT_NEXT, 0);
		} else if ( rt_same(target, "prev_slide") ) {
			if ( rt->host.action != NULL ) rt->host.action(rt->host.arg, rt, RT_ACT_PREV, 0);
		} else {
			say(rt, "realtime: no label ", target);
		}
		return;
	}
	jump_label(rt, i, l, t);
}

/* ---------------------------------------------------------------- onended */

LOCAL INT media_pick( RT *rt, INT from, CONST UB *what )
{
	INT	k, n;

	if ( rt_same(what, "next") ) return ( from + 1 < rt->nmedia ) ? from + 1 : -1;
	if ( rt_same(what, "first") ) return ( rt->nmedia > 0 ) ? 0 : -1;
	if ( rt_same(what, "last") ) return rt->nmedia - 1;
	if ( rt_same(what, "random") ) {
		n = rt->nmedia;
		if ( n <= 1 ) return 0;
		k = rt_rand_n(rt, n - 1);
		return ( k >= from ) ? k + 1 : k;
	}
	return rt_media_of(rt, what);
}

LOCAL void onended( RT *rt, INT k, RT_TIME T )
{
	CONST UB	*a = rt->media[k].onended;
	UB		act[16];
	CONST UB	*arg;
	INT		n = 0;

	if ( a == NULL ) return;
	while ( a[n] != 0 && a[n] != ':' && n < 15 ) {
		act[n] = a[n];
		n++;
	}
	act[n] = 0;
	arg = ( a[n] == ':' ) ? a + n + 1 : NULL;
	if ( rt_same(act, "play") || rt_same(act, "next") ) {
		media_play(rt, media_pick(rt, k, ( arg != NULL ) ? arg : (CONST UB *)"next"), T, -1);
	} else if ( rt_same(act, "pause") ) {
		INT	j = media_pick(rt, k, arg);

		if ( j >= 0 && rt->media[j].state == RT_MS_PLAYING ) {
			rt->media[j].state = RT_MS_PAUSED;
			mop(rt, j, RT_MO_PAUSE, 0);
		}
	} else if ( rt_same(act, "stop") ) {
		if ( arg == NULL ) {
			for ( n = 0; n < rt->nmedia; n++ ) media_stop(rt, n, T);
		} else {
			INT	j = media_pick(rt, k, arg);

			if ( j >= 0 ) media_stop(rt, j, T);
		}
	} else if ( rt_same(act, "jump") && arg != NULL ) {
		RTN	*l = rt_label_find(rt, arg);
		INT	i;

		/* a thread standing at it goes on; else the program goes there */
		for ( i = 0; l != NULL && i < rt->cthr; i++ ) {
			if ( rt->thr[i]->st == TS_LABEL && rt_eq(rt->thr[i]->label, arg) ) {
				rt->thr[i]->st = TS_RUN;
				if ( rt->thr[i]->t < T ) rt->thr[i]->t = T;
				return;
			}
		}
		if ( l != NULL ) jump_label(rt, -1, l, T);
		else say(rt, "realtime: no label ", arg);
	} else if ( rt_same(act, "slide") && arg != NULL && rt->host.action != NULL ) {
		if ( rt_same(arg, "next") ) rt->host.action(rt->host.arg, rt, RT_ACT_NEXT, 0);
		else if ( rt_same(arg, "prev") ) rt->host.action(rt->host.arg, rt, RT_ACT_PREV, 0);
		else if ( arg[0] == 'i' && arg[1] == 'n' && arg[2] == 'd' && arg[3] == 'e' && arg[4] == 'x'
		       && arg[5] == ':' ) {
			rt->host.action(rt->host.arg, rt, RT_ACT_INDEX, rt_int(arg + 6, 0));
		}
	}
}

LOCAL void media_ended( RT *rt, INT k, RT_TIME T )
{
	RT_MEDIA	*m = &rt->media[k];

	m->state = RT_MS_ENDED;
	m->play_on_load = FALSE;
	onended(rt, k, T);
	labels_check(rt, T);
}

LOCAL void media_ev( RT *rt, INT k, UINT ev, RT_V v, RT_TIME T )
{
	RT_MEDIA	*m = &rt->media[k];

	switch ( ev ) {
	case RT_ME_LOADED:
		m->loaded = TRUE;
		if ( v > 0 ) m->total = v;
		if ( m->state == RT_MS_LOADING ) {
			m->state = RT_MS_READY;
			if ( m->play_on_load ) media_go(rt, k);
		}
		break;
	case RT_ME_PLAYING:
		if ( m->state == RT_MS_READY || m->state == RT_MS_PAUSED || m->state == RT_MS_LOADING ) {
			m->state = RT_MS_PLAYING;
		}
		break;
	case RT_ME_PAUSED:
		if ( m->state == RT_MS_PLAYING ) m->state = RT_MS_PAUSED;
		break;
	case RT_ME_ENDED:
		if ( m->state == RT_MS_PLAYING || m->state == RT_MS_PAUSED ) media_ended(rt, k, T);
		break;
	case RT_ME_ERROR:
		if ( media_live(m) ) media_error(rt, k, ( v != 0 ) ? (UINT)v : RT_ERR_DECODE, T);
		break;
	case RT_ME_TIME:
		m->pos = v;
		break;
	default:
		break;
	}
}

EXPORT void rt_play_only( RT *rt, INT inst, RT_TIME now )
{
	INT	k;

	if ( rt == NULL || inst < 0 || inst >= rt->nmedia || !rt->running ) return;
	for ( k = 0; k < rt->nmedia; k++ ) {
		if ( k != inst && rt->media[k].kind == rt->media[inst].kind && rt->media[k].state == RT_MS_PLAYING ) {
			media_stop(rt, k, rt->now);
		}
	}
	(void)now;
	media_play(rt, inst, rt->now, -1);
}

EXPORT void rt_media_event( RT *rt, INT inst, INT gen, UINT ev, RT_V v, RT_TIME now )
{
	(void)now;
	if ( rt == NULL || inst < 0 || inst >= rt->nmedia ) return;
	if ( !rt_room(rt, &rt->mev, &rt->cmev, rt->nmev + 1, sizeof(*rt->mev)) ) {
		say(rt, "realtime: not enough memory for an event of a medium", NULL);
		return;
	}
	rt->mev[rt->nmev].inst = inst;
	rt->mev[rt->nmev].gen = gen;
	rt->mev[rt->nmev].ev = ev;
	rt->mev[rt->nmev].v = v;
	rt->nmev++;
}

/* ---------------------------------------------------------------- controls */

/* A time a node says (its duration), as the thread's units and speed make it */
LOCAL RT_TIME dur_of( RT *rt, RTTHR *th, CONST UB *s )
{
	RT_TIME	d;

	if ( s == NULL ) return 0;
	d = rt_utime(rt, &th->u, s, NULL);
	if ( th->speed > 0 && th->speed != RT_ONE ) d = rt_muldiv(d, RT_ONE, th->speed);
	return ( d > 0 ) ? d : 0;
}

/* A property of an object set now, or moved there over a time */
LOCAL void obj_to( RT *rt, INT i, INT k, INT prop, RT_V v, RT_TIME dur )
{
	RT_OBJ	*o = &rt->obj[k];

	if ( dur > 0 ) {
		RTANIM	*a = anim_new(rt, AN_OBJ, k, prop, i, rt->thr[i]->t);

		if ( a != NULL ) {
			a->n = 2;
			a->val[0] = obj_get(o, prop);
			a->val[1] = v;
			a->off[0] = 0;
			a->off[1] = Q;
			a->dur = dur;
			a->before = a->val[0];
		}
		return;
	}
	obj_set(o, prop, v);
}

LOCAL void ctrl_obj( RT *rt, INT i, RTN *n, INT k )
{
	RTTHR		*th = rt->thr[i];
	RT_OBJ		*o = &rt->obj[k];
	CONST UB	*c = n->s2;
	RT_TIME		dur = dur_of(rt, th, n->s3);
	RT_V		v = n->v[0];

	if ( rt_same(c, "hide") ) {
		obj_show(o, FALSE);
	} else if ( rt_same(c, "show") ) {
		obj_show(o, TRUE);
	} else if ( rt_same(c, "setVisibility") ) {
		obj_show(o, (BOOL)( v > RT_ONE / 2 ));
	} else if ( rt_same(c, "setZIndex") ) {
		o->z = (INT)( v / RT_ONE );
		o->dirty |= RT_D_Z;
	} else if ( rt_same(c, "replace") ) {
		if ( n->s4 != NULL ) {
			o->href = n->s4;
			o->dirty |= RT_D_PICT;
		}
	} else if ( rt_same(c, "setAlpha") ) {
		obj_to(rt, i, k, RT_P_ALPHA, v, dur);
	} else if ( rt_same(c, "setRotation") ) {
		obj_to(rt, i, k, RT_P_ROT, v, dur);
	} else if ( rt_same(c, "setScale") ) {
		obj_to(rt, i, k, PM_SCALE, v, dur);
	} else if ( rt_same(c, "setPositionX") ) {
		obj_to(rt, i, k, PM_X, v, dur);
	} else if ( rt_same(c, "setPositionY") ) {
		obj_to(rt, i, k, PM_Y, v, dur);
	} else if ( rt_same(c, "setPosition") ) {
		RT_V	b[4], w = o->p[RT_P_RIGHT] - o->p[RT_P_LEFT], h = o->p[RT_P_BOTTOM] - o->p[RT_P_TOP];
		INT	e;

		b[0] = rt_val(rt_attr(n->nd, "left"), o->p[RT_P_LEFT]);
		b[1] = rt_val(rt_attr(n->nd, "top"), o->p[RT_P_TOP]);
		/* an edge not said keeps the size: the box is moved */
		b[2] = ( n->flag & CF_RIGHT ) ? rt_val(rt_attr(n->nd, "right"), 0) : b[0] + w;
		b[3] = ( n->flag & CF_BOTTOM ) ? rt_val(rt_attr(n->nd, "bottom"), 0) : b[1] + h;
		if ( dur > 0 ) {
			RTANIM	*a = anim_new(rt, AN_BOX, k, -1, i, th->t);

			if ( a != NULL ) {
				for ( e = 0; e < 4; e++ ) {
					a->box[e] = o->p[e];
					a->box2[e] = b[e];
				}
				a->n = 2;
				a->val[0] = 0;
				a->val[1] = RT_ONE;
				a->off[1] = Q;
				a->dur = dur;
				a->before = 0;
			}
		} else {
			for ( e = 0; e < 4; e++ ) {
				if ( o->p[e] != b[e] ) {
					o->p[e] = b[e];
					o->dirty |= RT_D_BOX;
				}
			}
		}
	}
}

LOCAL void media_to( RT *rt, INT i, INT k, INT prop, RT_V from, RT_V v, RT_TIME dur, BOOL stop )
{
	RTANIM	*a;

	if ( dur <= 0 ) {
		RT_MEDIA	*m = &rt->media[k];

		if ( prop == MP_VOLUME ) m->volume = v;
		else if ( prop == MP_PAN ) m->pan = v;
		else m->rate = v;
		mop(rt, k, ( prop == MP_VOLUME ) ? RT_MO_VOLUME : ( prop == MP_PAN ) ? RT_MO_PAN : RT_MO_RATE, v);
		return;
	}
	a = anim_new(rt, AN_MEDIA, k, prop, i, ( i >= 0 ) ? rt->thr[i]->t : rt->now);
	if ( a == NULL ) return;
	a->n = 2;
	a->val[0] = from;
	a->val[1] = v;
	a->off[1] = Q;
	a->dur = dur;
	a->before = from;
	a->stop_at_end = stop;
}

LOCAL void ctrl_media1( RT *rt, INT i, RTN *n, INT k )
{
	RTTHR		*th = rt->thr[i];
	RT_MEDIA	*m = &rt->media[k];
	CONST UB	*c = n->s2;
	RT_TIME		dur = dur_of(rt, th, n->s3);

	if ( rt_same(c, "play") ) {
		media_play(rt, k, th->t, i);
	} else if ( rt_same(c, "pause") ) {
		if ( m->state == RT_MS_PLAYING ) {
			m->state = RT_MS_PAUSED;
			mop(rt, k, RT_MO_PAUSE, 0);
		}
	} else if ( rt_same(c, "resume") ) {
		if ( m->state == RT_MS_PAUSED ) {
			m->state = RT_MS_PLAYING;
			mop(rt, k, RT_MO_RESUME, 0);
		} else if ( m->state != RT_MS_PLAYING ) {
			media_play(rt, k, th->t, i);
		}
	} else if ( rt_same(c, "stop") ) {
		media_stop(rt, k, rt->now);
	} else if ( rt_same(c, "seek") ) {
		RT_TIME	to = ( n->flag & CF_VALUE ) ? rt_utime(rt, &th->u, rt_attr(n->nd, "value"), NULL) : 0;

		m->pos = to;
		mop(rt, k, RT_MO_SEEK, to);
	} else if ( rt_same(c, "setVolume") ) {
		media_to(rt, i, k, MP_VOLUME, m->volume, n->v[0], dur, FALSE);
	} else if ( rt_same(c, "setPan") ) {
		media_to(rt, i, k, MP_PAN, m->pan, n->v[0], dur, FALSE);
	} else if ( rt_same(c, "setPlaybackRate") ) {
		RT_V	r = n->v[0];

		if ( r < RT_ONE / 10 ) r = RT_ONE / 10;
		if ( r > 4 * RT_ONE ) r = 4 * RT_ONE;
		media_to(rt, i, k, MP_RATE, m->rate, r, dur, FALSE);
	} else if ( rt_same_ci(c, "fadeIn") ) {
		/* the value is how long it takes; up to the volume the element gave */
		RT_TIME	t = ( n->flag & CF_VALUE ) ? rt_utime(rt, &th->u, rt_attr(n->nd, "value"), NULL) : dur;
		RT_V	to = rt_val(rt_attr(m->nd, "volume"), RT_ONE);

		if ( t <= 0 ) t = 1000000;
		media_to(rt, i, k, MP_VOLUME, 0, to, t, FALSE);
		if ( m->state != RT_MS_PLAYING ) media_play(rt, k, th->t, i);
	} else if ( rt_same_ci(c, "fadeOut") ) {
		RT_TIME	t = ( n->flag & CF_VALUE ) ? rt_utime(rt, &th->u, rt_attr(n->nd, "value"), NULL) : dur;

		if ( t <= 0 ) t = 1000000;
		media_to(rt, i, k, MP_VOLUME, m->volume, 0, t, TRUE);
	} else if ( rt_same(c, "mute") ) {
		m->muted = TRUE;
		mop(rt, k, RT_MO_MUTE, 1);
	} else if ( rt_same(c, "unmute") ) {
		m->muted = FALSE;
		mop(rt, k, RT_MO_MUTE, 0);
	} else if ( m->obj >= 0 ) {
		/* the rest move what is shown */
		ctrl_obj(rt, i, n, m->obj);
	}
}

LOCAL void ctrl_media( RT *rt, INT i, RTN *n )
{
	INT	k;

	if ( rt_same(n->s1, "all") ) {
		for ( k = 0; k < rt->nmedia; k++ ) {
			if ( ( n->i1 == CT_AUDIO ) == ( rt->media[k].kind != RT_MK_VIDEO ) ) ctrl_media1(rt, i, n, k);
		}
		return;
	}
	k = rt_media_of(rt, n->s1);
	if ( k >= 0 ) {
		ctrl_media1(rt, i, n, k);
		return;
	}
	k = rt_obj_find(rt, n->s1);
	if ( k >= 0 ) ctrl_obj(rt, i, n, k);
	else say(rt, "realtime: nothing called ", n->s1);
}

/* The group of an id, anywhere in the program */
LOCAL RTN *group_in( RTN *n, CONST UB *id )
{
	RTN	*f;

	for ( ; n != NULL; n = n->next ) {
		if ( n->op == OP_GROUP && rt_eq(n->s1, id) ) return n;
		if ( ( f = group_in(n->body, id) ) != NULL ) return f;
		if ( ( f = group_in(n->alt, id) ) != NULL ) return f;
	}
	return NULL;
}

/* The top thread of the <realData> playing: where a group started from outside belongs */
LOCAL INT top_thread( RT *rt )
{
	INT	i;

	for ( i = 0; i < rt->cthr; i++ ) {
		if ( rt->thr[i]->st != TS_FREE && !rt->thr[i]->trig && rt->thr[i]->fr[0].kind == FR_TOP ) return i;
	}
	return -1;
}

LOCAL void group_hold( RT *rt, INT g, BOOL on, RT_TIME T )
{
	INT	k;

	for ( k = 0; k < rt->cthr; k++ ) {
		RTTHR	*th = rt->thr[k];

		if ( th->st == TS_FREE || !descends(rt, k, g) ) continue;
		if ( on && !th->held ) {
			th->held = TRUE;
			th->held_at = T;
		} else if ( !on && th->held ) {
			th->held = FALSE;
			if ( th->t > th->held_at ) th->t += T - th->held_at;
			else th->t = T;
			if ( th->until != RT_NEVER ) th->until += T - th->held_at;
		}
	}
	for ( k = 0; k < rt->canim; k++ ) {
		RTANIM	*a = rt->anim[k];

		if ( !a->used || a->thr < 0 || !descends(rt, a->thr, g) ) continue;
		if ( on && a->held_at == 0 ) {
			a->held_at = ( T > 0 ) ? T : 1;
		} else if ( !on && a->held_at != 0 ) {
			a->t0 += T - a->held_at;
			a->held_at = 0;
		}
	}
}

LOCAL void group_start( RT *rt, RTN *g, INT parent, RT_TIME t, RT_TIME ff )
{
	INT	c = thr_new(rt, parent, g->body, g, FR_GROUP, g, t);

	if ( c < 0 ) return;
	if ( g->i2 >= 0 ) rt->thr[c]->stream = g->i2;
	if ( ff > 0 ) rt->thr[c]->ff_until = ff;
	run_thread(rt, c, t);
}

LOCAL void ctrl_group( RT *rt, INT i, RTN *n )
{
	RTTHR		*th = rt->thr[i];
	CONST UB	*c = n->s2;
	RT_TIME		T = th->t;
	INT		k, found = 0;
	RTN		*g = NULL;

	if ( rt->cur >= 0 && rt->cur < rt->nblk ) g = group_in(rt->blk[rt->cur].body, n->s1);
	if ( g == NULL ) g = group_in(rt->pre, n->s1);
	for ( k = 0; k < rt->cthr; k++ ) {
		RTTHR	*gt = rt->thr[k];

		if ( gt->st == TS_FREE || gt->group == NULL || !rt_eq(gt->group->s1, n->s1) ) continue;
		found++;
		if ( rt_same(c, "pause") ) {
			group_hold(rt, k, TRUE, T);
		} else if ( rt_same(c, "resume") || rt_same(c, "play") ) {
			group_hold(rt, k, FALSE, T);
		} else if ( rt_same(c, "stop") || rt_same(c, "restart") || rt_same(c, "seek") ) {
			if ( k != i && !descends(rt, i, k) ) thr_kill(rt, k);
		} else if ( rt_same(c, "setSpeed") ) {
			RT_V	s = ( n->v[0] > 0 ) ? n->v[0] : RT_ONE;
			INT	q;

			for ( q = 0; q < rt->cthr; q++ ) {
				RTTHR	*d = rt->thr[q];

				if ( d->st == TS_FREE || !descends(rt, q, k) ) continue;
				if ( d->t > T ) d->t = T + rt_muldiv(d->t - T, d->speed, s);
				d->speed = s;
			}
			for ( q = 0; q < rt->canim; q++ ) {
				RTANIM	*a = rt->anim[q];

				if ( a->used && a->thr >= 0 && descends(rt, a->thr, k) && a->dur > 0 ) {
					RT_TIME	e = T - a->t0;
					RT_V	old = ( rt->thr[a->thr]->speed > 0 ) ? rt->thr[a->thr]->speed : RT_ONE;

					a->dur = rt_muldiv(a->dur, old, s);
					a->t0 = T - rt_muldiv(e, old, s);
				}
			}
		} else if ( rt_same(c, "reverse") ) {
			INT	q;

			for ( q = 0; q < rt->canim; q++ ) {
				RTANIM	*a = rt->anim[q];

				if ( a->used && a->thr >= 0 && descends(rt, a->thr, k) && a->dur > 0 ) {
					RT_TIME	e = ( T - a->t0 ) % a->dur;

					a->t0 = T - ( a->dur - e );
					a->rev = !a->rev;
				}
			}
		}
	}
	if ( g == NULL ) {
		if ( found == 0 ) say(rt, "realtime: no group ", n->s1);
		return;
	}
	if ( rt_same(c, "restart") || ( rt_same(c, "play") && found == 0 ) ) {
		group_start(rt, g, top_thread(rt), T, 0);
	} else if ( rt_same(c, "seek") ) {
		/* begun as long ago as the seek says, and run forward without sounding the past */
		RT_TIME	to = ( n->flag & CF_VALUE ) ? rt_utime(rt, &th->u, rt_attr(n->nd, "value"), NULL) : 0;

		group_start(rt, g, top_thread(rt), T - to, T);
	}
}

LOCAL void ctrl_trigger( RT *rt, RTN *n )
{
	INT	k;

	for ( k = 0; k < rt->ntrig; k++ ) {
		if ( rt_eq(rt->trig[k]->node->s4, n->s1) ) {
			if ( rt_same(n->s2, "enable") ) rt->trig[k]->enabled = TRUE;
			else if ( rt_same(n->s2, "disable") ) rt->trig[k]->enabled = FALSE;
		}
	}
}

LOCAL void ctrl( RT *rt, INT i, RTN *n )
{
	INT	k;

	switch ( n->i1 ) {
	case CT_AUDIO:
	case CT_VIDEO:
		ctrl_media(rt, i, n);
		return;
	case CT_GROUP:
		ctrl_group(rt, i, n);
		return;
	case CT_TRIGGER:
		ctrl_trigger(rt, n);
		return;
	default:
		k = rt_obj_find(rt, n->s1);
		if ( k < 0 ) {
			say(rt, "realtime: nothing called ", n->s1);
			return;
		}
		ctrl_obj(rt, i, n, k);
		return;
	}
}

/* ---------------------------------------------------------------- interpolation */

#define RT_INTERP_US	1000000		/* a part of an interpolation given no time */

/* The property a control moves, PM_* or RT_P_*; -1 for one that does not move */
LOCAL INT ctrl_prop( CONST UB *c )
{
	if ( rt_same(c, "setAlpha") ) return RT_P_ALPHA;
	if ( rt_same(c, "setRotation") ) return RT_P_ROT;
	if ( rt_same(c, "setScale") ) return PM_SCALE;
	if ( rt_same(c, "setPositionX") ) return PM_X;
	if ( rt_same(c, "setPositionY") ) return PM_Y;
	return -1;
}

LOCAL INT media_prop( CONST UB *c )
{
	if ( rt_same(c, "setVolume") ) return MP_VOLUME;
	if ( rt_same(c, "setPan") ) return MP_PAN;
	if ( rt_same(c, "setPlaybackRate") ) return MP_RATE;
	return -1;
}

/* An animation through the values given, one part of 'steps' divisions after another */
LOCAL void interp_anim( RT *rt, INT i, UINT kind, INT target, INT prop, CONST RT_V *val, INT n,
			CONST RTN *ip, RT_TIME seg )
{
	RTANIM	*a;
	INT	k;

	if ( n < 2 ) return;
	a = anim_new(rt, kind, target, prop, i, rt->thr[i]->t);
	if ( a == NULL ) return;
	if ( !rt_anim_keys(rt, a, n) ) {
		a->used = FALSE;
		say(rt, "realtime: not enough memory for the values of a change", NULL);
		return;
	}
	a->n = n;
	for ( k = 0; k < n; k++ ) {
		a->val[k] = val[k];
		a->off[k] = (INT)( (D)k * Q / ( n - 1 ) );
	}
	a->all = ip->ease;
	a->spline = (BOOL)( ip->ease.kind == RT_EASE_SPLINE );
	if ( a->spline && n == 2 ) a->spline = FALSE;
	a->steps = ip->i1 + 1;
	a->dur = seg * ( n - 1 );
	a->before = val[0];
}

LOCAL void interp_run( RT *rt, INT i, RTN *ip, RT_TIME T, RT_V *val )
{
	RTTHR	*th = rt->thr[i];
	RTN	*p, *first = NULL;
	RT_TIME	step = 0, seg;
	INT	np = 0, d = ip->i1;

	for ( p = ip->body; p != NULL; p = p->next ) {
		if ( p->op == OP_TDELTA ) {
			if ( step == 0 ) step = dur_of(rt, th, p->s1);
			continue;
		}
		if ( first == NULL ) first = p;
		np++;
	}
	if ( first == NULL ) return;
	if ( d <= 0 || np < 2 ) {
		/* nothing between: the events as they are, at once */
		for ( p = ip->body; p != NULL; p = p->next ) {
			if ( p->op != OP_TDELTA && p->op != OP_GROUP && p->op != OP_BLOCK && p->op != OP_IF
			  && p->op != OP_WHILE && p->op != OP_RANDOM && p->op != OP_WAIT ) {
				exec(rt, i, p);
			}
		}
		return;
	}
	/*
	 * With no time between its values the divisions would all fall in one
	 * frame and the change be a jump; each part from one value to the
	 * next is given a second then, about the length of a note it is made
	 * to go with.
	 */
	seg = ( step > 0 ) ? step * ( d + 1 ) : RT_INTERP_US;

	if ( first->op == OP_CTRL ) {
		/* each control of the same object and command is a run of values */
		RTN	*q;

		for ( p = ip->body; p != NULL; p = p->next ) {
			INT	k, prop, n = 0, obj = -1, med = -1;
			BOOL	seen = FALSE;

			if ( p->op != OP_CTRL ) continue;
			for ( q = ip->body; q != p; q = q->next ) {
				if ( q->op == OP_CTRL && rt_eq(q->s1, p->s1) && rt_eq(q->s2, p->s2) ) seen = TRUE;
			}
			if ( seen ) continue;
			prop = ctrl_prop(p->s2);
			if ( p->i1 == CT_AUDIO || p->i1 == CT_VIDEO ) med = rt_media_of(rt, p->s1);
			if ( med >= 0 && media_prop(p->s2) >= 0 ) {
				prop = media_prop(p->s2);
			} else {
				med = -1;
				obj = rt_obj_find(rt, p->s1);
			}
			if ( rt_same(p->s2, "setPosition") && obj >= 0 ) {
				/* the box: each edge a run of its own */
				static CONST char *CONST edge[4] = { "left", "top", "right", "bottom" };
				INT	e;

				for ( e = 0; e < 4; e++ ) {
					n = 0;
					for ( q = p; q != NULL; q = q->next ) {
						if ( q->op == OP_CTRL && rt_eq(q->s1, p->s1) && rt_eq(q->s2, p->s2) ) {
							val[n++] = rt_val(rt_attr(q->nd, edge[e]), rt->obj[obj].p[e]);
						}
					}
					obj_set(&rt->obj[obj], e, val[0]);
					interp_anim(rt, i, AN_OBJ, obj, e, val, n, ip, seg);
				}
				continue;
			}
			for ( q = p; q != NULL; q = q->next ) {
				if ( q->op == OP_CTRL && rt_eq(q->s1, p->s1) && rt_eq(q->s2, p->s2) ) {
					val[n++] = q->v[0];
				}
			}
			if ( med >= 0 ) {
				RTANIM	*a;

				media_to(rt, i, med, prop, val[0], val[0], 0, FALSE);
				interp_anim(rt, i, AN_MEDIA, med, prop, val, n, ip, seg);
				for ( k = 0; k < rt->canim; k++ ) {
					a = rt->anim[k];
					if ( a->used && a->kind == AN_MEDIA && a->target == med ) a->last = -1;
				}
			} else if ( obj >= 0 && prop >= 0 ) {
				obj_set(&rt->obj[obj], prop, val[0]);
				interp_anim(rt, i, AN_OBJ, obj, prop, val, n, ip, seg);
			} else if ( obj >= 0 ) {
				/* hide, show, setZIndex: the first as it is */
				ctrl_obj(rt, i, p, obj);
			} else {
				say(rt, "realtime: nothing called ", p->s1);
			}
		}
		return;
	}

	if ( first->op == OP_IMAGE || first->op == OP_FIGREF ) {
		INT	obj = ( first->op == OP_IMAGE ) ? first->i1 : rt_obj_find(rt, first->s1);
		INT	e, n;
		RT_OBJ	*o;

		if ( obj < 0 ) {
			say(rt, "realtime: nothing called ", first->s1);
			return;
		}
		o = &rt->obj[obj];
		if ( first->op == OP_IMAGE ) {
			static CONST char *CONST nm[6] = { "left", "top", "right", "bottom", "alpha", "rotation" };
			static CONST INT	pp[6] = { RT_P_LEFT, RT_P_TOP, RT_P_RIGHT, RT_P_BOTTOM, RT_P_ALPHA, RT_P_ROT };

			obj_show(o, TRUE);
			for ( e = 0; e < 6; e++ ) {
				n = 0;
				for ( p = ip->body; p != NULL; p = p->next ) {
					if ( p->op == OP_IMAGE && p->i1 == obj ) {
						val[n++] = rt_val(rt_attr(p->nd, nm[e]), o->base[pp[e]]);
					}
				}
				if ( n == 0 ) continue;
				obj_set(o, pp[e], val[0]);
				if ( n >= 2 && val[n - 1] != val[0] ) interp_anim(rt, i, AN_OBJ, obj, pp[e], val, n, ip, seg);
			}
			if ( rt_attr(first->nd, "zIndex") != NULL ) {
				o->z = rt_int(rt_attr(first->nd, "zIndex"), o->z);
				o->dirty |= RT_D_Z;
			}
		} else {
			for ( e = 0; e < 2; e++ ) {
				n = 0;
				for ( p = ip->body; p != NULL; p = p->next ) {
					if ( p->op == OP_FIGREF && rt_eq(p->s1, first->s1) ) {
						val[n++] = p->v[e];
					}
				}
				obj_set(o, e == 0 ? PM_X : PM_Y, val[0]);
				interp_anim(rt, i, AN_OBJ, obj, e == 0 ? PM_X : PM_Y, val, n, ip, seg);
			}
		}
		return;
	}

	if ( first->op == OP_NOTE ) {
		/* the notes between, each after the last: by the step given, or as long as it sounds */
		RTN	*a = NULL, *b;
		RT_TIME	t = th->t;

		for ( p = ip->body; p != NULL; p = p->next ) {
			if ( p->op != OP_NOTE ) continue;
			b = p;
			if ( a != NULL ) {
				RT_TIME	da = note_dur(rt, th, a), db = note_dur(rt, th, b);
				INT	j;

				for ( j = 0; j <= d; j++ ) {
					INT	q = rt_ease(&ip->ease, (INT)( (D)j * Q / ( d + 1 ) ));
					INT	pitch = a->i1 + (INT)( (D)( b->i1 - a->i1 ) * q / Q );
					INT	vel = a->i2 + (INT)( (D)( b->i2 - a->i2 ) * q / Q );
					RT_TIME	du = da + rt_muldiv(db - da, q, Q);

					note_out(rt, i, a, pitch, vel, du, t);
					t += ( step > 0 ) ? step : du;
				}
			}
			a = b;
		}
		if ( a != NULL ) note_out(rt, i, a, a->i1, a->i2, note_dur(rt, th, a), t);
		return;
	}
	/* what cannot be interpolated is done as it is */
	for ( p = ip->body; p != NULL; p = p->next ) {
		if ( p->op == OP_CTRL ) ctrl(rt, i, p);
	}
}

/* An <interpolation>: the values it gathers need room for as many as it has events */
LOCAL void interp( RT *rt, INT i, RTN *ip, RT_TIME T )
{
	RTN	*p;
	INT	n = 1;
	RT_V	*val;

	for ( p = ip->body; p != NULL; p = p->next ) n++;
	val = (RT_V *)rt->host.alloc(rt->host.arg, sizeof(RT_V) * (SZ)n);
	if ( val == NULL ) {
		say(rt, "realtime: not enough memory for an interpolation", NULL);
		return;
	}
	interp_run(rt, i, ip, T, val);
	rt->host.free(rt->host.arg, val);
}

/* ---------------------------------------------------------------- keyframes */

LOCAL INT kf_prop( CONST UB *s )
{
	if ( rt_same(s, "left") ) return PM_X;
	if ( rt_same(s, "top") ) return PM_Y;
	if ( rt_same(s, "right") ) return PM_R;
	if ( rt_same(s, "bottom") ) return PM_B;
	if ( rt_same(s, "alpha") || rt_same(s, "opacity") ) return RT_P_ALPHA;
	if ( rt_same(s, "scale") ) return PM_SCALE;
	if ( rt_same(s, "scaleX") ) return RT_P_SX;
	if ( rt_same(s, "scaleY") ) return RT_P_SY;
	if ( rt_same(s, "rotation") ) return RT_P_ROT;
	return -1;
}

/* A keyframe's offset: "25%" or 0.25, in 65536ths */
LOCAL INT kf_off( CONST UB *s )
{
	RT_V	v;
	INT	i;

	if ( !rt_num(s, &v) ) return 0;
	for ( i = 0; s[i] != 0 && s[i] != '%'; i++ ) ;
	if ( s[i] == '%' ) v /= 100;
	if ( v < 0 ) v = 0;
	if ( v > RT_ONE ) v = RT_ONE;
	return (INT)( v * Q / RT_ONE );
}

/* Read a <keyframes> into an animation; FALSE when it says nothing that moves */
LOCAL BOOL kf_read( RT *rt, RTN *n, RTANIM *a )
{
	CONST T_TADNODE	*c;
	CONST UB	*s;
	INT		nk;

	a->n = 0;
	for ( c = n->nd->first, nk = 0; c != NULL; c = c->next ) {
		if ( rt_is(c, "keyframe") ) nk++;
	}
	if ( !rt_anim_keys(rt, a, nk) ) {
		say(rt, "realtime: not enough memory for the keyframes", NULL);
		return FALSE;
	}
	for ( c = n->nd->first; c != NULL; c = c->next ) {
		if ( !rt_is(c, "keyframe") ) continue;
		a->off[a->n] = kf_off(rt_attr(c, "offset"));
		a->val[a->n] = rt_val(rt_attr(c, "value"), 0);
		if ( !rt_ease_parse(rt_attr(c, "function"), &a->ease[a->n]) ) a->ease[a->n].kind = RT_EASE_LINEAR;
		a->n++;
	}
	if ( a->n == 0 ) return FALSE;
	a->all = n->ease;
	a->iter = rt_int(rt_attr(n->nd, "iterations"), 1);
	s = rt_attr(n->nd, "direction");
	a->dir = rt_same(s, "reverse") ? DIR_REVERSE : rt_same(s, "alternate") ? DIR_ALT
	       : rt_same(s, "alternate-reverse") ? DIR_ALTREV : DIR_NORMAL;
	s = rt_attr(n->nd, "fill");
	a->fill = rt_same(s, "none") ? FILL_NONE : rt_same(s, "backwards") ? FILL_BACK
		: rt_same(s, "both") ? FILL_BOTH : FILL_FWD;
	return TRUE;
}

LOCAL void keyframes( RT *rt, INT i, RTN *n )
{
	RTTHR	*th = rt->thr[i];
	RTANIM	*a;
	INT	k, med = -1, prop;

	if ( rt_same(n->s2, "volume") ) med = rt_media_of(rt, n->s1);
	if ( med >= 0 ) {
		a = anim_new(rt, AN_MEDIA, med, MP_VOLUME, i, th->t);
		if ( a == NULL ) return;
		if ( !kf_read(rt, n, a) ) {
			a->used = FALSE;
			return;
		}
		a->before = rt->media[med].volume;
	} else {
		k = rt_obj_find(rt, n->s1);
		prop = kf_prop(n->s2);
		if ( k < 0 || prop < 0 ) {
			say(rt, "realtime: nothing to move: ", n->s1);
			return;
		}
		a = anim_new(rt, AN_OBJ, k, prop, i, th->t);
		if ( a == NULL ) return;
		if ( !kf_read(rt, n, a) ) {
			a->used = FALSE;
			return;
		}
		a->before = obj_get(&rt->obj[k], prop);
	}
	a->dur = dur_of(rt, th, rt_attr(n->nd, "duration"));
	anim_apply(rt, a, rt->now);
}

/* ---------------------------------------------------------------- triggers and waits */

LOCAL void trig_add( RT *rt, RTN *n )
{
	RTTRIG	*tr;

	if ( n->trig >= 0 ) return;
	if ( rt->ntrig >= rt->ctrig && !rt_room_slots(rt, &rt->trig, &rt->ctrig, rt->ntrig + 1, sizeof(RTTRIG)) && rt->ntrig >= rt->ctrig ) {
		say(rt, "realtime: not enough memory for another trigger", NULL);
		return;
	}
	tr = rt->trig[rt->ntrig];
	tr->node = n;
	tr->enabled = rt_bool(rt_attr(n->nd, "enabled"), TRUE);
	tr->inside = FALSE;
	tr->fired = 0;
	n->trig = rt->ntrig++;
}

LOCAL BOOL is_user_event( CONST UB *ev )
{
	return (BOOL)( rt_same(ev, "click") || rt_same(ev, "dblclick") || rt_same(ev, "keydown")
		    || rt_same(ev, "keyup") );
}

LOCAL void wait_meet( RT *rt, INT i, RTN *n )
{
	RTTHR		*th = rt->thr[i];
	CONST UB	*to = rt_attr(n->nd, "timeout");
	RT_TIME		d = ( to != NULL ) ? dur_of(rt, th, to) : 0;

	th->wait = n;
	th->woken = FALSE;
	th->until = ( d > 0 ) ? th->t + d : RT_NEVER;
	if ( !is_user_event(n->s3) && wait_met(rt, th, rt->now) ) {
		th->wait = NULL;
		return;
	}
	th->st = TS_WAIT;
}

/* ---------------------------------------------------------------- doing a node */

LOCAL void image_meet( RT *rt, RTN *n )
{
	RT_OBJ		*o;
	CONST T_TADNODE	*nd = n->nd;

	if ( n->i1 < 0 ) return;
	o = &rt->obj[n->i1];
	o->p[RT_P_LEFT] = rt_val(rt_attr(nd, "left"), o->p[RT_P_LEFT]);
	o->p[RT_P_TOP] = rt_val(rt_attr(nd, "top"), o->p[RT_P_TOP]);
	o->p[RT_P_RIGHT] = rt_val(rt_attr(nd, "right"), o->p[RT_P_RIGHT]);
	o->p[RT_P_BOTTOM] = rt_val(rt_attr(nd, "bottom"), o->p[RT_P_BOTTOM]);
	o->p[RT_P_ALPHA] = rt_val(rt_attr(nd, "alpha"), RT_ONE);
	o->p[RT_P_ROT] = rt_val(rt_attr(nd, "rotation"), 0);
	if ( rt_attr(nd, "zIndex") != NULL ) o->z = rt_int(rt_attr(nd, "zIndex"), o->z);
	if ( rt_attr(nd, "href") != NULL && !rt_eq(o->href, rt_attr(nd, "href")) ) {
		o->href = rt_attr(nd, "href");
		o->dirty |= RT_D_PICT;
	}
	o->flip_h = rt_bool(rt_attr(nd, "flipH"), FALSE);
	o->flip_v = rt_bool(rt_attr(nd, "flipV"), FALSE);
	o->dirty |= RT_D_BOX | RT_D_LOOK | RT_D_Z;
	obj_show(o, TRUE);
}

LOCAL void exec( RT *rt, INT i, RTN *n )
{
	RTTHR	*th = rt->thr[i];
	RT_TIME	d;
	INT	k, c;

	switch ( n->op ) {
	case OP_GROUP:
		c = thr_new(rt, i, n->body, n, FR_GROUP, n, th->t);
		if ( c < 0 ) return;
		th = rt->thr[i];
		if ( n->i2 >= 0 ) rt->thr[c]->stream = n->i2;
		if ( n->i1 ) {
			th->st = TS_CHILD;
			th->child = c;
		}
		/* what the group does at once happens before the next of the one that met it */
		run_thread(rt, c, th->t);
		return;
	case OP_BLOCK:
		(void)push(rt, th, FR_BLOCK, n, th->pc, n->body, NULL);
		return;
	case OP_TDELTA:
		d = dur_of(rt, th, n->s1);
		if ( n->s2 != NULL ) {
			d = rt_utime(rt, &th->u, n->s1, n->s2);
			if ( th->speed > 0 && th->speed != RT_ONE ) d = rt_muldiv(d, RT_ONE, th->speed);
		}
		if ( d > 0 ) th->t += d;
		return;
	case OP_TABS:
		d = ( n->s2 != NULL ) ? rt_utime(rt, &th->u, n->s1, n->s2) : rt_utime(rt, &th->u, n->s1, NULL);
		if ( th->speed > 0 && th->speed != RT_ONE ) d = rt_muldiv(d, RT_ONE, th->speed);
		if ( th->t0 + d > th->t ) th->t = th->t0 + d;	/* never back */
		return;
	case OP_LABEL:
		if ( awaited(rt, n) ) {
			th->st = TS_LABEL;
			th->label = ( n->s1 != NULL ) ? n->s1 : n->s2;
		}
		return;
	case OP_JUMP:
		do_jump(rt, i, n->s1, rt->now);
		return;
	case OP_IF: {
		RTN	*b = n;

		while ( b != NULL ) {
			if ( b->op == OP_ELSE || cond(rt, b->code, b->ncode) ) {
				(void)push(rt, th, FR_BLOCK, b, th->pc, b->body, NULL);
				return;
			}
			b = b->alt;
		}
		return;
	}
	case OP_WHILE:
		if ( cond(rt, n->code, n->ncode) && n->body != NULL ) {
			(void)push(rt, th, FR_WHILE, n, th->pc, n->body, NULL);
		}
		return;
	case OP_ASSIGN:
		if ( n->i1 > 0 && n->i1 < RT_NVAR ) {
			rt->var[n->i1] = (H)rt_expr_eval(rt, n->code, n->ncode);
		}
		return;
	case OP_RANDOM: {
		RTN	*a;
		INT	cnt = 0, pick;

		for ( a = n->body; a != NULL; a = a->next ) cnt++;
		if ( cnt == 0 ) return;
		if ( n->i1 && cnt > 1 && n->last >= 0 && n->last < cnt ) {
			pick = rt_rand_n(rt, cnt - 1);
			if ( pick >= n->last ) pick++;
		} else {
			pick = rt_rand_n(rt, cnt);
		}
		n->last = pick;
		for ( a = n->body; pick > 0; a = a->next ) pick--;
		(void)push(rt, th, FR_BLOCK, n, th->pc, a, a->next);
		return;
	}
	case OP_INTERP:
		interp(rt, i, n, rt->now);
		return;
	case OP_USPEC:
		k = usys_index(rt, n->i1);
		th->u.sys = k;
		th->u.aux = n->i2;
		return;
	case OP_USPECT:
		th->u.tsys = usys_index(rt, n->i1);
		if ( th->u.tsys < 0 ) th->u.tsys = -1;
		th->u.taux = n->i2;
		return;
	case OP_USCALE:
		if ( n->i1 == 2 ) {
			D	sn = n->v[0], sd = n->v[1];
			INT	e;

			for ( e = n->i2; e > 0; e-- ) sn *= 10;
			for ( e = n->i2; e < 0; e++ ) sd *= 10;
			th->u.sn = sn;
			th->u.sd = sd;
		}
		return;
	case OP_UCOEFF:
		k = usys_index(rt, n->i1);
		if ( k >= 0 ) {
			rt->usys[k].pn = n->v[0];
			rt->usys[k].pd = n->v[1];
		}
		return;
	case OP_NOTE:
		note_out(rt, i, n, n->i1, n->i2, note_dur(rt, th, n), th->t);
		return;
	case OP_MIDI: {
		RT_SND	s;

		rt_zero(&s, sizeof(s));
		s.kind = RT_SND_MIDI;
		s.stream = th->stream;
		s.nd = n->nd;
		for ( k = 0; k < n->ncode && k < RT_MIDI_MAX; k++ ) {
			s.msg[k][0] = (UB)n->code[k * 3];
			s.msg[k][1] = (UB)n->code[k * 3 + 1];
			s.msg[k][2] = (UB)n->code[k * 3 + 2];
		}
		s.nmsg = k;
		sound_out(rt, &s, th->t, i);
		return;
	}
	case OP_PCM:
		if ( n->i1 >= 0 ) {
			media_meet(rt, i, n, rt->now);
		} else {
			RT_SND	s;

			rt_zero(&s, sizeof(s));
			s.kind = RT_SND_PCM;
			s.stream = th->stream;
			s.nd = n->nd;
			sound_out(rt, &s, th->t, i);
		}
		return;
	case OP_AUDIO:
	case OP_VIDEO:
		media_meet(rt, i, n, rt->now);
		return;
	case OP_IMAGE:
		image_meet(rt, n);
		return;
	case OP_FIGREF:
		k = rt_obj_find(rt, n->s1);
		if ( k >= 0 ) {
			obj_set(&rt->obj[k], PM_X, n->v[0]);
			obj_set(&rt->obj[k], PM_Y, n->v[1]);
		}
		return;
	case OP_CTRL:
		ctrl(rt, i, n);
		return;
	case OP_WAIT:
		wait_meet(rt, i, n);
		return;
	case OP_KEYFR:
		keyframes(rt, i, n);
		return;
	case OP_TRIGGER:
		trig_add(rt, n);
		return;
	default:
		return;
	}
}

/* One node of a thread, or the end of the block it is in */
LOCAL void one( RT *rt, INT i )
{
	RTTHR	*th = rt->thr[i];
	RTFRAME	*f = &th->fr[th->nfr - 1];
	RTN	*n = th->pc;

	if ( th->t != th->spin_t ) {
		th->spin_t = th->t;
		th->spin = 0;
	}
	if ( ++th->spin > RT_SPIN ) {
		/* round and round with no time passing: let time pass */
		if ( !rt->said_spin ) {
			say(rt, "realtime: a loop that takes no time; a millisecond is put in each round", NULL);
			rt->said_spin = TRUE;
		}
		th->t += 1000;
		return;
	}
	if ( n == NULL || ( f->end != NULL && n == f->end ) ) {
		block_end(rt, i);
		return;
	}
	th->pc = n->next;
	exec(rt, i, n);
	th = rt->thr[i];
	if ( n->op != OP_USPECT && th->st != TS_FREE ) th->u.tsys = -2;
}

LOCAL void run_thread( RT *rt, INT i, RT_TIME limit )
{
	INT	guard = 0;

	while ( rt->thr[i]->st == TS_RUN && !rt->thr[i]->held && rt->thr[i]->t <= limit && guard++ < 100000 ) {
		one(rt, i);
	}
}

/* The earliest thread due by T, -1 none */
LOCAL INT earliest( RT *rt, RT_TIME T )
{
	INT	i, best = -1;

	for ( i = 0; i < rt->cthr; i++ ) {
		RTTHR	*th = rt->thr[i];

		if ( th->st != TS_RUN || th->held || th->t > T ) continue;
		if ( best < 0 || th->t < rt->thr[best]->t ) best = i;
	}
	return best;
}

/* ---------------------------------------------------------------- blocks */

LOCAL void reset_trig( RT *rt )
{
	INT	k;

	for ( k = 0; k < rt->ntrig; k++ ) rt->trig[k]->node->trig = -1;
	rt->ntrig = 0;
}

LOCAL void start_block( RT *rt, INT b, RT_TIME t, BOOL user )
{
	RTBLOCK	*blk = &rt->blk[b];
	INT	i, play;
	RTUNIT	u;

	rt->cur = b;
	for ( i = 0; i < rt->cthr; i++ ) rt->thr[i]->st = TS_FREE;
	reset_trig(rt);
	if ( rt->var != NULL ) rt_zero(rt->var, sizeof(H) * RT_NVAR);	/* variables last one <realData> */
	play = ( blk->autoplay < 0 ) ? (INT)rt->autoplay : blk->autoplay;
	if ( !play && !user ) {
		rt->waiting_user = TRUE;
		return;
	}
	rt->waiting_user = FALSE;
	unit_reset(&u);
	blk->delay = ( blk->delay_s != NULL ) ? rt_utime(rt, &u, blk->delay_s, NULL) : 0;
	if ( b == 0 && rt->pre != NULL && rt->loops == 0 && rt->all_loops == 0 ) {
		i = thr_new(rt, -1, rt->pre, NULL, FR_TOP, NULL, t);
		if ( i >= 0 ) rt->thr[i]->trig = FALSE;
	}
	i = thr_new(rt, -1, blk->body, NULL, FR_TOP, NULL, t + blk->delay);
	if ( i >= 0 ) rt->thr[i]->block = b;
}

/* Is anything of the <realData> still going? */
LOCAL BOOL block_busy( RT *rt )
{
	INT	i;

	for ( i = 0; i < rt->cthr; i++ ) {
		if ( rt->thr[i]->st != TS_FREE && !rt->thr[i]->trig ) return TRUE;
	}
	for ( i = 0; i < rt->nmedia; i++ ) {
		RT_MEDIA	*m = &rt->media[i];

		if ( m->state == RT_MS_PLAYING || ( m->state == RT_MS_LOADING && m->play_on_load ) ) return TRUE;
	}
	for ( i = 0; i < rt->canim; i++ ) {
		if ( rt->anim[i]->used && rt->anim[i]->iter > 0 ) return TRUE;
	}
	/* and the last note still sounding */
	return (BOOL)( rt->nsndq > 0 || rt->sound_until > rt->now );
}

LOCAL void block_next( RT *rt, RT_TIME T )
{
	RTBLOCK	*b = &rt->blk[rt->cur];

	if ( b->loop && ( b->count == 0 || rt->loops + 1 < b->count ) ) {
		rt->loops++;
		start_block(rt, rt->cur, T, TRUE);
		return;
	}
	rt->loops = 0;
	if ( rt->cur + 1 < rt->nblk ) {
		start_block(rt, rt->cur + 1, T, FALSE);
		return;
	}
	if ( rt->loop ) {
		rt->all_loops++;
		start_block(rt, 0, T, TRUE);
		return;
	}
	rt->finished = TRUE;
	if ( rt->host.action != NULL ) rt->host.action(rt->host.arg, rt, RT_ACT_DONE, 0);
}

/* ---------------------------------------------------------------- before the start */

/*
 * What an interpolation or a keyframe run will move is put where it
 * starts before the program does, so that what fades in is not seen
 * before its fade, nor what slides in before it slides.
 */
/* A bit for each property, to put each where it starts once */
LOCAL UW pbit( INT prop )
{
	return ( prop >= 0 && prop < 24 ) ? ( 1U << prop ) : 0;
}

LOCAL void preroll_in( RT *rt, RTN *n, UW *done )
{
	for ( ; n != NULL; n = n->next ) {
		if ( n->op == OP_INTERP ) {
			RTN	*p;

			for ( p = n->body; p != NULL; p = p->next ) {
				INT	k, prop;

				if ( p->op != OP_CTRL || ( p->i1 != CT_IMAGE && p->i1 != CT_TEXT && p->i1 != CT_SHAPE
				  && p->i1 != CT_VIDEO ) ) {
					continue;
				}
				k = rt_obj_find(rt, p->s1);
				prop = ctrl_prop(p->s2);
				if ( k < 0 || prop < 0 || ( p->flag & CF_VALUE ) == 0 ) continue;
				if ( done[k] & pbit(prop) ) continue;
				done[k] |= pbit(prop);
				obj_set(&rt->obj[k], prop, p->v[0]);
			}
		} else if ( n->op == OP_KEYFR ) {
			CONST UB	*f = rt_attr(n->nd, "fill");
			INT		k = rt_obj_find(rt, n->s1), prop = kf_prop(n->s2);
			CONST T_TADNODE	*c = rt_child(n->nd, "keyframe");

			if ( ( rt_same(f, "backwards") || rt_same(f, "both") ) && k >= 0 && prop >= 0 && c != NULL
			  && !( done[k] & pbit(prop) ) ) {
				done[k] |= pbit(prop);
				obj_set(&rt->obj[k], prop, rt_val(rt_attr(c, "value"), 0));
			}
		}
		preroll_in(rt, n->body, done);
		preroll_in(rt, n->alt, done);
	}
}

LOCAL void preroll( RT *rt )
{
	UW	*done;
	INT	b;

	/* a word of property bits for each object: which of its properties have been put where they start */
	done = (UW *)rt->host.alloc(rt->host.arg, sizeof(UW) * (SZ)( rt->nobj + 1 ));
	if ( done == NULL ) {
		say(rt, "realtime: not enough memory to make the pictures ready", NULL);
		return;
	}
	rt_zero(done, sizeof(UW) * (SZ)( rt->nobj + 1 ));
	preroll_in(rt, rt->pre, done);
	for ( b = 0; b < rt->nblk; b++ ) preroll_in(rt, rt->blk[b].body, done);
	rt->host.free(rt->host.arg, done);
}

/* ---------------------------------------------------------------- the calls */

EXPORT ER rt_new( CONST T_TADNODE *realtime, CONST RT_HOST *host, RT **p_rt )
{
	RT		*rt;
	CONST UB	*s;
	ER		er;

	if ( realtime == NULL || host == NULL || host->alloc == NULL || host->free == NULL || p_rt == NULL ) {
		return E_PAR;
	}
	if ( !rt_is(realtime, "realtime") ) return E_NOEXS;
	rt = (RT *)host->alloc(host->arg, sizeof(RT));
	if ( rt == NULL ) return E_NOMEM;
	rt->host = *host;
	rt->root = realtime;
	rt->autoplay = rt_bool(rt_attr(realtime, "autoplay"), FALSE);
	rt->loop = rt_bool(rt_attr(realtime, "loop"), FALSE);
	s = rt_attr(realtime, "preload");
	rt->preload = rt_same(s, "none") ? RT_PRE_NONE : rt_same(s, "auto") ? RT_PRE_AUTO : RT_PRE_META;
	rt->var = (H *)host->alloc(host->arg, sizeof(H) * RT_NVAR);
	if ( rt->var == NULL ) {
		host->free(host->arg, rt);
		return E_NOMEM;
	}
	rt_seed(rt, 0x13579BDFU);
	rt->cur = 0;
	er = rt_compile(rt);
	if ( er < E_OK ) {
		rt_delete(rt);
		return er;
	}
	*p_rt = rt;
	return E_OK;
}

EXPORT void rt_delete( RT *rt )
{
	INT	k;

	if ( rt == NULL ) return;
	for ( k = 0; k < rt->nmedia; k++ ) {
		if ( rt->media[k].state != RT_MS_IDLE ) mop(rt, k, RT_MO_CLOSE, 0);
	}
	rt_free_prog(rt);
	if ( rt->var != NULL ) rt->host.free(rt->host.arg, rt->var);
	rt->host.free(rt->host.arg, rt);
}

LOCAL void reset_run( RT *rt )
{
	INT	k, j;

	for ( k = 0; k < rt->cthr; k++ ) rt->thr[k]->st = TS_FREE;
	for ( k = 0; k < rt->canim; k++ ) rt->anim[k]->used = FALSE;
	reset_trig(rt);
	rt->nsndq = 0;
	rt->nmev = 0;
	rt->sound_until = 0;
	for ( k = 0; k < rt->nmedia; k++ ) {
		RT_MEDIA	*m = &rt->media[k];

		if ( m->state != RT_MS_IDLE ) mop(rt, k, RT_MO_CLOSE, 0);
		m->state = RT_MS_IDLE;
		m->loaded = FALSE;
		m->play_on_load = FALSE;
		m->retry_at = 0;
		m->gen++;
		rt->upaused[k] = FALSE;
	}
	for ( k = 0; k < rt->nobj; k++ ) {
		RT_OBJ	*o = &rt->obj[k];

		for ( j = 0; j < RT_NP; j++ ) o->p[j] = o->base[j];
		o->z = o->base_z;
		o->shown = o->base_shown;
		o->href = o->base_href;
		o->dirty = RT_D_BOX | RT_D_LOOK | RT_D_SHOWN | RT_D_Z | RT_D_PICT;
	}
	rt->loops = 0;
	rt->all_loops = 0;
	rt->finished = FALSE;
	rt->waiting_user = FALSE;
	rt->said_spin = FALSE;
}

EXPORT void rt_start( RT *rt, RT_TIME now )
{
	if ( rt == NULL ) return;
	reset_run(rt);
	preroll(rt);
	rt->origin = now;
	rt->now = 0;
	rt->running = TRUE;
	rt->paused = FALSE;
	start_block(rt, 0, 0, FALSE);
}

EXPORT BOOL rt_waiting( CONST RT *rt )
{
	return (BOOL)( rt != NULL && rt->running && rt->waiting_user );
}

EXPORT void rt_begin( RT *rt, RT_TIME now )
{
	RT_TIME	T;

	if ( rt == NULL || !rt->running || !rt->waiting_user ) return;
	T = now - rt->origin;
	if ( T < rt->now ) T = rt->now;
	rt->now = T;
	start_block(rt, rt->cur, T, TRUE);
}

EXPORT RT_TIME rt_step( RT *rt, RT_TIME now )
{
	RT_TIME	T, next = RT_NEVER;
	INT	i, k, ops = 0;

	if ( rt == NULL || !rt->running || rt->paused ) return RT_NEVER;
	T = now - rt->origin;
	if ( T < rt->now ) T = rt->now;
	rt->now = T;

	/* what the media said */
	for ( k = 0; k < rt->nmev; k++ ) {
		INT	m = rt->mev[k].inst;

		if ( rt->mev[k].gen == rt->media[m].gen ) media_ev(rt, m, rt->mev[k].ev, rt->mev[k].v, T);
	}
	rt->nmev = 0;
	for ( k = 0; k < rt->nmedia; k++ ) {
		RT_MEDIA	*m = &rt->media[k];

		if ( m->retry_at != 0 && T >= m->retry_at ) {
			m->retry_at = 0;
			m->href = rt_attr(m->nd, "href");
			m->nfall = 0;
			m->asked = T;
			say(rt, "realtime: trying again ", m->href);
			mop(rt, k, RT_MO_LOAD, 0);
		} else if ( m->state == RT_MS_LOADING && m->retry_at == 0 && m->timeout > 0
			 && T - m->asked >= m->timeout ) {
			mop(rt, k, RT_MO_CLOSE, 0);
			m->gen++;
			media_error(rt, k, RT_ERR_TIMEOUT, T);
		}
	}
	waits_check(rt, T);

	/* everything due, the earliest first */
	while ( !rt->waiting_user && ( i = earliest(rt, T) ) >= 0 && ops++ < 200000 ) {
		one(rt, i);
	}

	/* the sounds put off */
	for ( k = 0; k < rt->nsndq; ) {
		if ( rt->sndq[k].t <= T ) {
			sound_out(rt, &rt->sndq[k].s, rt->sndq[k].t, -1);
			rt->sndq[k] = rt->sndq[--rt->nsndq];
		} else {
			if ( rt->sndq[k].t < next ) next = rt->sndq[k].t;
			k++;
		}
	}
	/* and what moves */
	for ( k = 0; k < rt->canim; k++ ) {
		if ( rt->anim[k]->used ) {
			anim_apply(rt, rt->anim[k], T);
			if ( rt->anim[k]->used && rt->anim[k]->held_at == 0 ) next = T + RT_FRAME_US;
		}
	}
	if ( rt->ffwd && T >= rt->ffwd_to ) rt->ffwd = FALSE;

	if ( !rt->waiting_user && !rt->finished && !block_busy(rt) ) {
		block_next(rt, T);
		if ( !rt->finished && !rt->waiting_user ) next = T;
	}

	for ( i = 0; i < rt->cthr; i++ ) {
		RTTHR	*th = rt->thr[i];

		if ( th->held ) continue;
		if ( th->st == TS_RUN && th->t < next ) next = th->t;
		if ( th->st == TS_WAIT && th->until < next ) next = th->until;
	}
	for ( k = 0; k < rt->nmedia; k++ ) {
		RT_MEDIA	*m = &rt->media[k];

		if ( m->retry_at != 0 && m->retry_at < next ) next = m->retry_at;
		if ( m->state == RT_MS_LOADING && m->timeout > 0 && m->asked + m->timeout < next ) {
			next = m->asked + m->timeout;
		}
	}
	if ( rt->sound_until > T && rt->sound_until < next ) next = rt->sound_until;
	if ( next == RT_NEVER ) return RT_NEVER;
	if ( next < T ) next = T;
	return next + rt->origin;
}

EXPORT void rt_pause( RT *rt, RT_TIME now )
{
	INT	k;

	if ( rt == NULL || !rt->running || rt->paused ) return;
	rt->paused = TRUE;
	rt->paused_at = now;
	for ( k = 0; k < rt->nmedia; k++ ) {
		rt->upaused[k] = FALSE;
		if ( rt->media[k].state == RT_MS_PLAYING ) {
			rt->upaused[k] = TRUE;
			rt->media[k].state = RT_MS_PAUSED;
			mop(rt, k, RT_MO_PAUSE, 0);
		}
	}
}

EXPORT void rt_resume( RT *rt, RT_TIME now )
{
	INT	k;

	if ( rt == NULL || !rt->running || !rt->paused ) return;
	rt->origin += now - rt->paused_at;
	rt->paused = FALSE;
	for ( k = 0; k < rt->nmedia; k++ ) {
		if ( rt->upaused[k] && rt->media[k].state == RT_MS_PAUSED ) {
			rt->media[k].state = RT_MS_PLAYING;
			mop(rt, k, RT_MO_RESUME, 0);
		}
		rt->upaused[k] = FALSE;
	}
}

EXPORT void rt_stop( RT *rt )
{
	if ( rt == NULL ) return;
	reset_run(rt);
	rt->running = FALSE;
	rt->paused = FALSE;
}

EXPORT void rt_seek( RT *rt, RT_TIME to, RT_TIME now )
{
	if ( rt == NULL ) return;
	if ( to < 0 ) to = 0;
	rt_start(rt, now - to);
	if ( rt->waiting_user ) rt_begin(rt, now - to);
	rt->ffwd = TRUE;
	rt->ffwd_to = to;
	(void)rt_step(rt, now);
	rt->ffwd = FALSE;
}

EXPORT BOOL rt_running( CONST RT *rt )
{
	return (BOOL)( rt != NULL && rt->running );
}

EXPORT BOOL rt_paused( CONST RT *rt )
{
	return (BOOL)( rt != NULL && rt->paused );
}

EXPORT BOOL rt_done( CONST RT *rt )
{
	return (BOOL)( rt != NULL && rt->finished );
}

EXPORT RT_TIME rt_now( CONST RT *rt )
{
	return ( rt != NULL ) ? rt->now : 0;
}

/* ---------------------------------------------------------------- the person */

LOCAL BOOL key_is( CONST UB *want, CONST char *got )
{
	if ( want == NULL ) return TRUE;
	if ( got == NULL ) return FALSE;
	if ( rt_same_ci(want, got) ) return TRUE;
	/* the names keys go by elsewhere */
	if ( rt_same_ci(want, "Spacebar") || rt_same(want, " ") ) return (BOOL)rt_same_ci((CONST UB *)got, "Space");
	if ( rt_same_ci(want, "Esc") ) return (BOOL)rt_same_ci((CONST UB *)got, "Escape");
	if ( rt_same_ci(want, "Left") ) return (BOOL)rt_same_ci((CONST UB *)got, "ArrowLeft");
	if ( rt_same_ci(want, "Right") ) return (BOOL)rt_same_ci((CONST UB *)got, "ArrowRight");
	if ( rt_same_ci(want, "Up") ) return (BOOL)rt_same_ci((CONST UB *)got, "ArrowUp");
	if ( rt_same_ci(want, "Down") ) return (BOOL)rt_same_ci((CONST UB *)got, "ArrowDown");
	if ( rt_same_ci(want, "Return") ) return (BOOL)rt_same_ci((CONST UB *)got, "Enter");
	return FALSE;
}

LOCAL BOOL on_target( RT *rt, CONST UB *target, CONST RT_IN *in )
{
	INT	k;

	if ( target == NULL ) return TRUE;
	k = rt_obj_find(rt, target);
	if ( k < 0 ) return FALSE;
	return obj_hit(&rt->obj[k], (RT_V)in->x * RT_ONE, (RT_V)in->y * RT_ONE);
}

/* Does a trigger fire on this? 'look' only asks, changing nothing */
LOCAL BOOL trig_match( RT *rt, RTTRIG *tr, CONST RT_IN *in, BOOL look )
{
	RTN		*n = tr->node;
	CONST UB	*ev = n->s1;

	if ( !tr->enabled ) return FALSE;
	switch ( in->kind ) {
	case RT_IN_CLICK:	return (BOOL)( rt_same(ev, "click") && on_target(rt, n->s2, in) );
	case RT_IN_DBLCLICK:	return (BOOL)( rt_same(ev, "dblclick") && on_target(rt, n->s2, in) );
	case RT_IN_KEYDOWN:	return (BOOL)( rt_same(ev, "keydown") && key_is(n->s3, in->key) );
	case RT_IN_KEYUP:	return (BOOL)( rt_same(ev, "keyup") && key_is(n->s3, in->key) );
	case RT_IN_SWIPE_L:	return rt_same(ev, "swipeleft");
	case RT_IN_SWIPE_R:	return rt_same(ev, "swiperight");
	case RT_IN_SWIPE_U:	return rt_same(ev, "swipeup");
	case RT_IN_SWIPE_D:	return rt_same(ev, "swipedown");
	case RT_IN_RESIZE:	return rt_same(ev, "resize");
	case RT_IN_VISIBLE:	return rt_same(ev, "visibility");
	case RT_IN_MOVE: {
		BOOL	in_now, fire;

		if ( !rt_same(ev, "mouseenter") && !rt_same(ev, "mouseleave") ) return FALSE;
		in_now = ( n->s2 != NULL ) ? on_target(rt, n->s2, in ) : TRUE;
		fire = (BOOL)( in_now != tr->inside && ( rt_same(ev, "mouseenter") ? in_now : !in_now ) );
		if ( !look ) tr->inside = in_now;
		return fire;
	}
	default:
		return FALSE;
	}
}

LOCAL BOOL wait_match( RT *rt, RTTHR *th, CONST RT_IN *in )
{
	CONST UB	*ev;

	if ( th->st != TS_WAIT || th->wait == NULL || th->held ) return FALSE;
	ev = th->wait->s3;
	switch ( in->kind ) {
	case RT_IN_CLICK:	return (BOOL)( rt_same(ev, "click") && on_target(rt, th->wait->s4, in) );
	case RT_IN_DBLCLICK:	return (BOOL)( rt_same(ev, "dblclick") && on_target(rt, th->wait->s4, in) );
	case RT_IN_KEYDOWN:	return rt_same(ev, "keydown");
	case RT_IN_KEYUP:	return rt_same(ev, "keyup");
	default:		return FALSE;
	}
}

EXPORT BOOL rt_input( RT *rt, CONST RT_IN *in, RT_TIME now )
{
	RT_TIME	T;
	BOOL	took = FALSE;
	INT	k, n;

	if ( rt == NULL || in == NULL || !rt->running ) return FALSE;
	T = now - rt->origin;
	if ( rt->paused ) T = rt->paused_at - rt->origin;
	if ( T < rt->now ) T = rt->now;
	if ( in->kind == RT_IN_KEYDOWN ) rt->last_key = (INT)in->code;
	if ( in->kind == RT_IN_CLICK || in->kind == RT_IN_DBLCLICK || in->kind == RT_IN_MOVE ) {
		rt->ptr_x = in->x;
		rt->ptr_y = in->y;
	}
	for ( k = 0; k < rt->cthr; k++ ) {
		if ( wait_match(rt, rt->thr[k], in) ) {
			rt->thr[k]->woken = TRUE;
			rt->thr[k]->st = TS_RUN;
			rt->thr[k]->wait = NULL;
			if ( rt->thr[k]->t < T ) rt->thr[k]->t = T;
			took = TRUE;
		}
	}
	n = rt->ntrig;
	for ( k = 0; k < n; k++ ) {
		RTTRIG	*tr = rt->trig[k];

		if ( trig_match(rt, tr, in, FALSE) ) {
			tr->fired++;
			if ( rt_bool(rt_attr(tr->node->nd, "once"), FALSE) ) tr->enabled = FALSE;
			rt->now = T;
			handler_run(rt, tr->node->body, tr->node, T);
			if ( in->kind != RT_IN_MOVE ) took = TRUE;
		}
	}
	return took;
}

EXPORT BOOL rt_would_take( CONST RT *rt, CONST RT_IN *in )
{
	INT	k;

	if ( rt == NULL || in == NULL || !rt->running ) return FALSE;
	for ( k = 0; k < rt->cthr; k++ ) {
		if ( wait_match((RT *)rt, (RTTHR *)rt->thr[k], in) ) return TRUE;
	}
	for ( k = 0; k < rt->ntrig; k++ ) {
		if ( trig_match((RT *)rt, (RTTRIG *)rt->trig[k], in, TRUE) ) return TRUE;
	}
	return FALSE;
}
