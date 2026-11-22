/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rt_local.h
 *	The inside of the engine of 実時間制御TAD (design 15.3)
 *
 *	A program is the elements of a <realtime> read into nodes: each
 *	node is one element the engine acts on, its block (the events of a
 *	group, the branch of an if, the body of a while) the nodes under
 *	it. Running it is a set of threads, each walking the nodes of one
 *	group at a time of its own; a parallel group is a new thread that
 *	goes on beside the one that met it, a serial group one the other
 *	waits for.
 */

#ifndef __RT_LOCAL_H__
#define __RT_LOCAL_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/rtad.h>

/*
 * The tables of a program (objects, media, threads, changes over time,
 * triggers, sounds waiting, ...) grow as it needs them. What stays fixed
 * is the format's: the user variables below.
 */
#define RT_DEPTH0	8		/* blocks one thread is inside: the room it starts with */
#define RT_NVAR		0x4000		/* user variables 0 .. 0x3FFF */
#define RT_SPIN		20000		/* nodes a thread may do without time going on */

/* Node kinds */
#define OP_NOP		0
#define OP_GROUP	1
#define OP_BLOCK	2		/* <eventArray>, <then>, <do> taken as a block */
#define OP_TDELTA	3
#define OP_TABS		4
#define OP_LABEL	5
#define OP_JUMP		6
#define OP_IF		7
#define OP_ELSEIF	8
#define OP_ELSE		9
#define OP_WHILE	10
#define OP_ASSIGN	11
#define OP_RANDOM	12
#define OP_INTERP	13
#define OP_USPEC	14
#define OP_USPECT	15
#define OP_USCALE	16
#define OP_UCOEFF	17
#define OP_NOTE		18
#define OP_MIDI		19
#define OP_PCM		20
#define OP_AUDIO	21
#define OP_VIDEO	22
#define OP_IMAGE	23
#define OP_FIGREF	24
#define OP_CTRL		25
#define OP_WAIT		26
#define OP_KEYFR	27
#define OP_TRIGGER	28
#define OP_ONERROR	29

/* Which control (OP_CTRL i1) */
#define CT_AUDIO	0
#define CT_VIDEO	1
#define CT_IMAGE	2
#define CT_TEXT		3
#define CT_SHAPE	4
#define CT_GROUP	5
#define CT_TRIGGER	6

/* OP_CTRL flags: which box attributes it has */
#define CF_LEFT		0x01
#define CF_TOP		0x02
#define CF_RIGHT	0x04
#define CF_BOTTOM	0x08
#define CF_VALUE	0x10
#define CF_DUR		0x20

typedef struct rtn RTN;

struct rtn {
	UH		op;
	UH		flag;
	RTN		*next;		/* the next in its block */
	RTN		*body;		/* the first of the block it holds */
	RTN		*alt;		/* an if's next elseif or else */
	RTN		*up;		/* the node whose block it is in; NULL the top */
	CONST T_TADNODE	*nd;
	CONST UB	*s1, *s2, *s3, *s4;
	RT_V		v[4];
	INT		i1, i2, i3;
	UH		*code;		/* an expression */
	INT		ncode;
	UH		*code2;		/* a while's endExpr */
	INT		ncode2;
	RT_EASE		ease;
	INT		last;		/* <random exclude="last">: the one chosen last */
	INT		block;		/* the <realData> it is in */
	INT		trig;		/* <trigger>: its entry once met, -1 */
};

/* A unit system */
typedef struct {
	INT	id;
	D	pn, pd;			/* the primary unit: pn/pd seconds */
	INT	naux;
	INT	caux;			/* room in the three below */
	INT	*auxno;
	D	*an, *ad;		/* an/ad of the primary */
} RTUSYS;

/* The units in force for a thread */
typedef struct {
	INT	sys;			/* index into usys, -1: milliseconds */
	INT	aux;			/* the auxiliary unit's number, -1 the primary */
	INT	tsys, taux;		/* <unitSpecTemp>: for the next value only; tsys -2 none */
	D	sn, sd;			/* <unitScale> of time */
} RTUNIT;

/* One block a thread is inside */
#define FR_GROUP	1		/* the block of its own group (the bottom frame) */
#define FR_TOP		2		/* the top of a <realData>, or a trigger's, handler's body */
#define FR_BLOCK	3		/* eventArray, then, else, random's choice */
#define FR_WHILE	4

typedef struct {
	UINT	kind;
	RTN	*node;			/* the node whose block it is */
	RTN	*ret;			/* where to go on after it */
	RTN	*end;			/* a random's choice: the node after it, where the block stops */
	INT	count;			/* a while: times round */
} RTFRAME;

/* Thread states */
#define TS_FREE		0
#define TS_RUN		1		/* ready at t */
#define TS_CHILD	2		/* waiting for a serial group */
#define TS_WAIT		3		/* at a <wait> */
#define TS_LABEL	4		/* at a label a medium is to jump to when it ends */
#define TS_DONE		5

typedef struct {
	UINT		st;
	RTN		*pc;
	RTFRAME		*fr;
	INT		cfr;		/* room in fr */
	INT		nfr;
	RT_TIME		t;		/* when what it does next happens */
	RT_TIME		t0;		/* when its group began */
	INT		parent;		/* the thread that started it, -1 */
	INT		child;		/* TS_CHILD: the serial group it waits for */
	RTN		*group;		/* its group, NULL for a top */
	INT		block;		/* its <realData> */
	INT		stream;
	RTUNIT		u;
	INT		spin;
	RT_TIME		spin_t;
	RTN		*wait;		/* TS_WAIT: the <wait> */
	RT_TIME		until;		/* and when it gives up, RT_NEVER none */
	BOOL		woken;		/* what it waited for came */
	CONST UB	*label;		/* TS_LABEL */
	BOOL		held;		/* groupControl pause */
	RT_TIME		held_at;
	RT_V		speed;		/* groupControl setSpeed, RT_ONE as written */
	BOOL		trig;		/* a trigger's or a handler's: not waited for at the end */
	RT_TIME		ff_until;	/* run forward to a seek: nothing before it sounds */
	INT		gen;
} RTTHR;

/* Properties an animation moves beyond RT_P_*: the box moved whole, by one of its edges */
#define PM_X		16
#define PM_Y		17
#define PM_R		18
#define PM_B		19
#define PM_SCALE	20		/* across and down together */

/* A change over time */
#define AN_OBJ		1		/* a property of a display object */
#define AN_MEDIA	2		/* volume, pan or rate of a medium */
#define AN_BOX		3		/* the whole box of an object (setPosition with a duration) */

#define MP_VOLUME	0
#define MP_PAN		1
#define MP_RATE		2

#define DIR_NORMAL	0
#define DIR_REVERSE	1
#define DIR_ALT		2
#define DIR_ALTREV	3

#define FILL_NONE	0
#define FILL_FWD	1
#define FILL_BACK	2
#define FILL_BOTH	3

typedef struct {
	BOOL		used;
	UINT		kind;
	INT		target;		/* object or medium */
	INT		prop;
	INT		n;		/* keyframes */
	INT		ckey;		/* room in the three below */
	RT_V		*val;
	INT		*off;		/* where each stands, 65536ths of the whole */
	RT_EASE		*ease;		/* into each */
	RT_EASE		all;		/* across the whole (an interpolation's function) */
	BOOL		spline;		/* an interpolation through its points */
	INT		steps;		/* an interpolation's: values in each part, 0 smooth */
	RT_TIME		t0;		/* when it began */
	RT_TIME		dur;		/* one time through */
	INT		iter;		/* times through, 0 for ever */
	UINT		dir;
	UINT		fill;
	RT_V		before;		/* the value before it, for fill="none" */
	RT_V		box[4];		/* AN_BOX: from */
	RT_V		box2[4];	/* and to */
	INT		thr;		/* the thread whose group it belongs to */
	BOOL		rev;		/* turned round by groupControl reverse */
	RT_TIME		held_at;	/* its group is paused: since when, 0 not */
	BOOL		stop_at_end;	/* fadeOut: the medium stops when it ends */
	RT_V		last;		/* the value given last (media) */
} RTANIM;

/* A <trigger> once met */
typedef struct {
	RTN		*node;
	BOOL		enabled;
	BOOL		inside;		/* mouseenter, mouseleave: the pointer is over it */
	INT		fired;
} RTTRIG;

/* A sound waiting for its time */
typedef struct {
	RT_SND		s;
	RT_TIME		t;		/* program time */
	INT		thr;
} RTSNDQ;

/* A <realData> */
typedef struct {
	CONST T_TADNODE	*nd;
	RTN		*body;
	INT		autoplay;	/* -1 inherit, 0 no, 1 yes */
	RT_TIME		delay;		/* startDelay, as its first units say */
	CONST UB	*delay_s;
	BOOL		loop;
	INT		count;		/* loopCount, 0 for ever */
	INT		onerr0;		/* its <onerror> nodes: rt->onerr from here, */
	INT		nonerr;		/* this many */
} RTBLOCK;

/* imageControl replace: a picture an object may be given */
typedef struct {
	CONST UB	*href;
	CONST UB	*ref;		/* the object's id */
} RTREPL;

struct rt {
	RT_HOST		host;
	CONST T_TADNODE	*root;		/* <realtime> */
	BOOL		autoplay;
	BOOL		loop;
	UINT		preload;
	BOOL		sounds;		/* notes, MIDI, PCM or audio in it */
	BOOL		oom;		/* a table could not grow while the program was read */

	/* the program */
	RTN		*pre;		/* the media written outside every <realData> */
	RTBLOCK		*blk;
	INT		nblk, cblk;
	RTN		**onerr;
	INT		nonerr, conerr;
	RTUSYS		*usys;
	INT		nusys, cusys;
	RT_STREAM	*stream;
	INT		nstream, cstream;
	RT_OBJ		*obj;
	INT		nobj, cobj;
	RT_MEDIA	*media;
	RTN		**mnode;	/* the element of each met last: its handlers */
	BOOL		*upaused;	/* paused with the whole program */
	INT		nmedia, cmedia, cmnode, cupaused;
	RTREPL		*repl;
	INT		nrepl, crepl;
	CONST UB	**want;		/* the ids the program moves */
	INT		nwant, cwant;

	/* the run */
	BOOL		running;
	BOOL		paused;
	BOOL		finished;
	RT_TIME		origin;		/* the host's clock at program time 0 */
	RT_TIME		paused_at;	/* the host's clock when paused */
	RT_TIME		now;		/* program time of the last step */
	INT		cur;		/* the <realData> playing */
	INT		loops;		/* times it has gone round */
	INT		all_loops;
	BOOL		waiting_user;	/* a block that does not play by itself waits for a start */
	RTTHR		**thr;		/* each slot a place of its own, which does not move */
	INT		cthr;
	RTANIM		**anim;
	INT		canim;
	RTTRIG		**trig;
	INT		ntrig, ctrig;
	RTSNDQ		*sndq;
	INT		nsndq, csndq;
	H		*var;
	UINT		seed;
	INT		last_key;
	INT		ptr_x, ptr_y;
	INT		slide;		/* RT_SV_SLIDE */
	BOOL		ffwd;		/* running forward to a seek: nothing past sounds */
	RT_TIME		ffwd_to;
	BOOL		said_spin;
	RT_TIME		sound_until;	/* when the last note started ends */
	INT		gen;
	/* media events, kept until the next step */
	struct rt_mev {
		INT	inst, gen;
		UINT	ev;
		RT_V	v;
	}		*mev;
	INT		nmev, cmev;
};

/* rt_num.c */
IMPORT void    rt_zero( void *p, SZ n );
IMPORT INT     rt_slen( CONST UB *s );
IMPORT BOOL    rt_same( CONST UB *a, CONST char *b );
IMPORT BOOL    rt_same_ci( CONST UB *a, CONST char *b );
IMPORT BOOL    rt_eq( CONST UB *a, CONST UB *b );
IMPORT BOOL    rt_is( CONST T_TADNODE *nd, CONST char *name );
IMPORT CONST UB *rt_attr( CONST T_TADNODE *nd, CONST char *name );
IMPORT RT_V    rt_val( CONST UB *s, RT_V dflt );
IMPORT BOOL    rt_bool( CONST UB *s, BOOL dflt );
IMPORT D       rt_muldiv( D a, D n, D d );
IMPORT CONST UB *rt_text( CONST T_TADNODE *nd );
IMPORT CONST T_TADNODE *rt_child( CONST T_TADNODE *nd, CONST char *name );
IMPORT UINT    rt_rand( RT *rt );
IMPORT INT     rt_rand_n( RT *rt, INT n );

/* rt_prog.c */
IMPORT ER   rt_compile( RT *rt );
IMPORT void rt_free_prog( RT *rt );
IMPORT BOOL rt_room( RT *rt, void *pp, INT *cap, INT need, SZ esz );
IMPORT BOOL rt_room_slots( RT *rt, void *pp, INT *cap, INT need, SZ esz );
IMPORT BOOL rt_anim_keys( RT *rt, RTANIM *a, INT n );
IMPORT BOOL rt_frames( RT *rt, RTTHR *th, INT n );
IMPORT INT  rt_obj_add( RT *rt, CONST UB *id, UINT kind, CONST T_TADNODE *nd );
IMPORT INT  rt_media_of( CONST RT *rt, CONST UB *id );
IMPORT RTN *rt_label_find( RT *rt, CONST UB *name );
IMPORT void rt_media_attrs( RT *rt, RT_MEDIA *m, CONST T_TADNODE *nd, CONST RTUNIT *u );

/* rt_run.c */
IMPORT RT_TIME rt_utime( RT *rt, RTUNIT *u, CONST UB *value, CONST UB *unit );

#endif /* __RT_LOCAL_H__ */
