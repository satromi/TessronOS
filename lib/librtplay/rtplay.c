/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtplay.c
 *	The player of 実時間制御TAD: a record's <realtime> shown and played
 *	in a part of the window of the program that links it (design 15.5,
 *	include/ts/rtplay.h)
 *
 *	The scene is laid out once, by the same code that draws documents
 *	and figures (include/ts/tvport.h): everything the program never
 *	moves is drawn into layers that stay as they are, and everything it
 *	moves -- a shape, a text box, a picture, a video, each by its id --
 *	into a picture of its own, a sprite. Each is drawn in an environment
 *	of the process's memory (dp_open_mem). A sprite is drawn twice, on
 *	black and on white, and how much of each pixel shows is what changed
 *	between the two, so the smoothed edges of letters keep their shades
 *	over whatever is under them. Each frame lays the layers and the
 *	sprites over one another in their order, each sprite where the
 *	program has moved it, turned, scaled, and as clear as it says, and
 *	only where something changed, and puts what changed on the window
 *	(dp_put_argb, wm_obj_flush).
 *
 *	The program runs in a task of the player's own (lib/librtad), woken
 *	at the time of its next event and at each frame while anything
 *	moves; the program's tasks only build the scene and pass on what the
 *	person does, holding the player's lock. Sound is made by the process
 *	実時間メディア and video by MPEG Player started for it (--embed), as
 *	children of this program; the player talks to them over channels
 *	(ts/rtmedia.h) and lays each video's frames where its element says.
 *	Each is told which process it plays for, and ends when that process
 *	ends without a word to it.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ts/tvport.h>
#include <ts/uapp.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/tvsrc.h>
#include <ts/ui.h>
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/time.h>
#include <ts/rtad.h>
#include <ts/rtmedia.h>
#include <ts/rtplay.h>
#include <ts/slide.h>
#include <ts/sysdef.h>

#define RT_TASK_STK	( 64 * 1024 )
#define MARGIN		6		/* round a sprite, at life size: strokes reach past the box */
#define MAX_ITEM	512
#define MAX_FIG		16
#define MAX_DOC		32
#define MAX_ALT		8		/* pictures a sprite may be changed to */
#define MAX_DIRTY	8
#define OUTQ		256		/* commands not yet taken by a helper */
#define VOUTQ		32
#define SWIPE		60		/* scene units a press must travel to be a swipe */
#define MIN_AREA	16		/* the least of the area a scene is laid out in */

/* What one thing of the scene is */
#define IT_FIGSH	1		/* a shape of a figure */
#define IT_DOC		2		/* a text box: a <document> standing on its own */
#define IT_PAGE		3		/* the whole document the <realtime> is in */
#define IT_OBJ		4		/* a picture or a video the program brings in */

typedef struct {
	UINT		kind;
	INT		z, ord;
	T_TVFIG		*fig;
	INT		sh;
	T_TVDOC		*doc;
	T_DPRECT	box;		/* scene units */
	INT		obj;		/* the program's object it is, -1 none */
} ITEM;

typedef struct {
	CONST UB	*href;
	UW		*px;
	INT		w, h;
} ALT;

typedef struct {
	INT		obj;		/* a sprite's object; -1 a layer that stays */
	INT		first, n;	/* its items */
	UW		*px;		/* coverage in the top byte, 255 solid */
	INT		w, h;
	T_DPRECT	at;		/* where px stands, zoomed scene pixels */
	T_DPRECT	base;		/* a sprite: its object's box when it was drawn, zoomed */
	T_DPRECT	was;		/* where it was put last; empty none */
	ALT		alt[MAX_ALT];
	INT		nalt;
	INT		cur;		/* the alternative shown, -1 its own */
	BOOL		video;
	UW		*frame;		/* a video's frame, fw by fh */
	INT		fw, fh;
	BOOL		has_frame;
	INT		lw;		/* a video's border */
	UW		lcol;
} LAYER;

/* Commands for a helper not yet on its channel */
typedef struct {
	RTM_CMD		*q;
	INT		size, head, tail;
} OQ;

/* A helper process: 実時間メディア, or MPEG Player for one video */
typedef struct {
	BOOL		used;
	BOOL		up;		/* it said it is ready */
	BOOL		start;		/* to be started by the task */
	BOOL		failed;
	TS_UUID		prog;
	TS_UUID		proc;
	TS_UUID		cmd_u;
	ID		cmd;
	TS_UUID		mem_u;		/* a video: its frames */
	ID		mem;
	OQ		oq;
	INT		w, h;		/* a video: what its frames are made for */
	RT_TIME		asked;		/* when it was started (host µs) */
} HELPER;

typedef struct rtplay DTRT;

struct rtplay {
	INT		gid;
	ID		kw;
	ID		port;
	T_DPRECT	area;		/* of the window's work area, where the scene may go */
	T_DPRECT	fit;		/* where it goes */
	INT		z, zd;		/* magnified z / zd */
	INT		tw, th;		/* the scene, its units */
	UW		paper;
	TS_UUID		obj;
	INT		recno;
	T_TAD		*rec;		/* the player's own: what the program's nodes point into */
	UB		*xml;		/* and the bytes it was read from */
	RT		*rt;
	ITEM		*item;
	INT		nitem;
	T_TVFIG		*fig[MAX_FIG];
	INT		nfig;
	T_TVDOC		*doc[MAX_DOC];
	INT		ndoc;
	LAYER		*layer;
	INT		nlayer;
	UW		*base;		/* the ground and what stays under the first sprite */
	UW		*fb;		/* what is shown */
	INT		fw, fh;
	T_DPRECT	dirty[MAX_DIRTY];
	INT		ndirty;
	ID		mtx;
	ID		tid;
	volatile BOOL	quit;
	volatile BOOL	gone;
	RT_TIME		step_now;	/* the host's clock at the step being made */
	RT_TIME		last_frame;
	/* the helpers */
	TS_UUID		rep_u;
	ID		rep;
	HELPER		snd;
	HELPER		vid[RT_MEDIA_MAX];
	BOOL		hold_start;	/* the program waits for 実時間メディア */
	RT_TIME		hold_until;
	/* what the program asked the desktop to do, done in the desktop's task */
	volatile UINT	act;
	volatile INT	act_n;
	INT		slide_no;
	/* the person */
	BOOL		down;
	INT		down_x, down_y;
	UD		last_click;
	INT		last_cx, last_cy;
	BOOL		said_novideo;
	/* what the program that plays it keeps of it (rtp_do) */
	UINT		no;		/* its number on the console */
	UW		ground;		/* round the scene in the area */
	INT		aw, ah;		/* the area the ground is drawn in */
	INT		track;		/* the one of the playlist chosen last, -1 none */
	BOOL		moved;		/* a move was told on the console (only the first is) */
	BOOL		slide;		/* played as a slide: what the record asks of the slides is kept */
	UINT		go;		/* and what it asked last, not yet handed on (RT_ACT_*, 0 none) */
	INT		go_n;
};

LOCAL UINT	rtp_last = 0;

/* ---------------------------------------------------------------- small things */

LOCAL void tell( CONST char *fmt, ... )
{
	char	line[256];
	va_list	ap;

	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	line[sizeof(line) - 2] = 0;
	strcat(line, "\n");
	(void)tm_putstring((CONST UB *)line);
}

LOCAL RT_TIME now_us( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (RT_TIME)( ns / 1000 );
}

/* The player's lock: a semaphore of one, taken by one task at a time and never twice by it */
LOCAL void lock( DTRT *p )
{
	if ( p->mtx > 0 ) (void)tk_wai_sem(p->mtx, 1, TMO_FEVR);
}

LOCAL void unlock( DTRT *p )
{
	if ( p->mtx > 0 ) (void)tk_sig_sem(p->mtx, 1);
}

LOCAL void wake( DTRT *p )
{
	if ( p->tid > 0 ) (void)tk_wup_tsk(p->tid);
}

LOCAL INT zs( DTRT *p, INT v )
{
	return (INT)( (D)v * p->z / p->zd );
}

LOCAL INT zv( DTRT *p, RT_V v )
{
	/* millionths of a unit into zoomed pixels, rounded down past zero */
	D	x = v * p->z / p->zd;

	return (INT)( ( x >= 0 ) ? x / RT_ONE : -( ( -x + RT_ONE - 1 ) / RT_ONE ) );
}

LOCAL BOOL same_str( CONST UB *a, CONST UB *b )
{
	INT	i;

	if ( a == NULL || b == NULL ) return FALSE;
	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (BOOL)( a[i] == b[i] );
}

LOCAL BOOL r_empty( CONST T_DPRECT *r )
{
	return (BOOL)( r->right <= r->left || r->bottom <= r->top );
}

LOCAL void r_join( T_DPRECT *a, CONST T_DPRECT *b )
{
	if ( r_empty(b) ) return;
	if ( r_empty(a) ) {
		*a = *b;
		return;
	}
	if ( b->left < a->left ) a->left = b->left;
	if ( b->top < a->top ) a->top = b->top;
	if ( b->right > a->right ) a->right = b->right;
	if ( b->bottom > a->bottom ) a->bottom = b->bottom;
}

LOCAL BOOL r_cut( T_DPRECT *a, CONST T_DPRECT *b )
{
	if ( a->left < b->left ) a->left = b->left;
	if ( a->top < b->top ) a->top = b->top;
	if ( a->right > b->right ) a->right = b->right;
	if ( a->bottom > b->bottom ) a->bottom = b->bottom;
	return (BOOL)!r_empty(a);
}

LOCAL void say( DTRT *p, CONST char *s )
{
	(void)p;
	tell("%s", s);
}

/* ---------------------------------------------------------------- the engine's host */

LOCAL void *h_alloc( void *arg, SZ size )
{
	void	*m = Kmalloc(size);

	(void)arg;
	if ( m != NULL ) knl_memset(m, 0, (INT)size);
	return m;
}

LOCAL void h_free( void *arg, void *m )
{
	(void)arg;
	Kfree(m);
}

LOCAL void h_log( void *arg, CONST char *s )
{
	say((DTRT *)arg, s);
}

LOCAL void h_action( void *arg, RT *rt, UINT act, INT n )
{
	DTRT	*p = (DTRT *)arg;

	(void)rt;
	if ( act == RT_ACT_DONE ) {
		tell("realtime: played to the end");
		return;
	}
	p->act_n = n;
	p->act = act;
}

/* ---------------------------------------------------------------- helpers: the channels */

LOCAL BOOL oq_init( OQ *q, INT size )
{
	q->q = (RTM_CMD *)Kmalloc(sizeof(RTM_CMD) * (SZ)size);
	q->size = size;
	q->head = q->tail = 0;
	return (BOOL)( q->q != NULL );
}

LOCAL void oq_put( OQ *q, CONST RTM_CMD *c )
{
	if ( q->q == NULL ) return;
	if ( ( q->tail + 1 ) % q->size == q->head ) {
		tell("realtime: a helper is not taking its commands; one dropped");
		q->head = ( q->head + 1 ) % q->size;
	}
	q->q[q->tail] = *c;
	q->tail = ( q->tail + 1 ) % q->size;
}

/* As many as the channel takes now */
LOCAL void oq_flush( HELPER *h )
{
	while ( h->cmd > 0 && h->up && h->oq.q != NULL && h->oq.head != h->oq.tail ) {
		SZ	asz = 0;

		if ( ob_wri_rec(h->cmd, 0, 0, &h->oq.q[h->oq.head], sizeof(RTM_CMD), &asz) < E_OK ) break;
		h->oq.head = ( h->oq.head + 1 ) % h->oq.size;
	}
}

/*
 * The process object of this program, which each helper watches (its
 * OB_E_EXIT) to end with it: found once among the processes by the
 * number its metadata says
 */
LOCAL BOOL	self_known = FALSE;
LOCAL TS_UUID	self_u;

LOCAL ID pid_of( CONST TS_UUID *pu )
{
	UB	b[16];
	SZ	asz = 0;
	ID	key, n = 0;
	INT	i;

	key = ob_opn_obj(pu, OB_OP_ATRRD);
	if ( key <= 0 ) return 0;
	if ( ob_get_atv(key, (CONST UB *)"tessronos.process.pid", b, sizeof(b) - 1, &asz) < E_OK ) asz = 0;
	ob_cls_obj(key);
	for ( i = 0; i < (INT)asz && b[i] >= '0' && b[i] <= '9'; i++ ) n = n * 10 + ( b[i] - '0' );
	return n;
}

LOCAL void self_find( void )
{
	TS_UUID	ids[16], last;
	INT	cnt, i, all = 0;
	ID	me = ts_get_pid();

	if ( self_known ) return;
	do {
		cnt = 0;
		if ( ob_lst_obj(OB_T_PROCESS, 0, ( all > 0 ) ? &last : NULL, ids, 16, &cnt) < E_OK ) break;
		for ( i = 0; i < cnt && i < 16; i++ ) {
			if ( pid_of(&ids[i]) == me ) {
				self_u = ids[i];
				self_known = TRUE;
				return;
			}
		}
		if ( cnt > 0 ) last = ids[( cnt < 16 ) ? cnt - 1 : 15];
		all += cnt;
	} while ( cnt >= 16 );
}

/* What the helpers use -- the channels and the frames' memory -- is this program's, and its children's */
LOCAL ER chan_make( TS_UUID *u, ID *p_key, UINT ops )
{
	T_OBCRE	c;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	*p_key = ob_opn_obj(u, ops | OB_O_NOWAIT);
	if ( *p_key < E_OK ) {
		er = (ER)*p_key;
		*p_key = 0;
		(void)ob_del_obj(u);
		return er;
	}
	return E_OK;
}

/* A helper made ready to start: its channel; for a video, the memory its frames go to */
LOCAL BOOL helper_make( DTRT *p, HELPER *h, CONST TS_UUID *prog, BOOL video, INT qsize )
{
	knl_memset(h, 0, sizeof(*h));
	h->used = TRUE;
	h->prog = *prog;
	if ( !oq_init(&h->oq, qsize) ) return FALSE;
	if ( chan_make(&h->cmd_u, &h->cmd, OB_OP_WRITE) < E_OK ) return FALSE;
	if ( video ) {
		T_OBCRE	c;
		INT	rec = 0;
		SZ	asz = 0;
		RTM_FRAMES	head;

		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_MEMORY;
		if ( ob_cre_obj(&c, &h->mem_u) < E_OK ) return FALSE;
		h->mem = ob_opn_obj(&h->mem_u, OB_OP_READ | OB_OP_WRITE | OB_OP_RECORD);
		if ( h->mem < E_OK || ob_apd_rec(h->mem, OB_RT_SYSDATA, 0, &rec) < E_OK ) return FALSE;
		knl_memset(&head, 0, sizeof(head));
		(void)ob_wri_rec(h->mem, 0, 0, &head, sizeof(head), &asz);
	}
	h->start = TRUE;
	(void)p;
	return TRUE;
}

/* The helper's process started (in the player's task: reading a program takes a while) */
LOCAL void helper_start( DTRT *p, HELPER *h )
{
	T_OBCRE	c;
	RTM_ARG	a;
	ER	er;

	h->start = FALSE;
	knl_memset(&a, 0, sizeof(a));
	a.magic = RTM_MAGIC;
	a.cmd = h->cmd_u;
	a.reply = p->rep_u;
	a.frames = h->mem_u;
	/* a video's helper says which it is when it is ready: its number, plus one */
	a.flags = ( h != &p->snd ) ? (UINT)( h - p->vid ) + 1 : 0;
	knl_memcpy(a.embed, RTM_EMBED, 8);
	/* the helper ends with this program, however it ends */
	if ( self_known ) a.host = self_u;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = h->prog;
	c.arg = &a;
	c.argsz = sizeof(a);
	unlock(p);
	er = ob_cre_obj(&c, &h->proc);
	lock(p);
	h->asked = now_us();
	if ( er < E_OK ) {
		tell("realtime: a helper could not be started (%d)", (INT)er);
		h->failed = TRUE;
	}
}

LOCAL void helper_end( HELPER *h )
{
	RTM_CMD	c;
	SZ	asz = 0;

	if ( !h->used ) return;
	if ( h->cmd > 0 ) {
		knl_memset(&c, 0, sizeof(c));
		c.op = RTM_QUIT;
		(void)ob_wri_rec(h->cmd, 0, 0, &c, sizeof(c), &asz);
		(void)ob_cls_obj(h->cmd);
		(void)ob_del_obj(&h->cmd_u);	/* its reader wakes and ends */
	}
	if ( h->mem > 0 ) {
		(void)ob_cls_obj(h->mem);
		(void)ob_del_obj(&h->mem_u);
	}
	if ( h->oq.q != NULL ) Kfree(h->oq.q);
	knl_memset(h, 0, sizeof(*h));
}

/* ---------------------------------------------------------------- where a medium is */

/*
 * What an href names: "{realId}_0_0.mp3" or "<uuid>_0_0.mp3", a
 * resource of an object; "<uuid>" alone, an object a file was taken in
 * as; anything else, a resource of the object being played.
 */
LOCAL BOOL obj_href( CONST UB *href, TS_UUID *id, CONST UB **p_name )
{
	char	txt[TS_UUID_STRLEN + 1];
	INT	k;

	for ( k = 0; k < TS_UUID_STRLEN; k++ ) {
		if ( href[k] == 0 ) return FALSE;
		txt[k] = (char)href[k];
	}
	txt[k] = 0;
	if ( ts_str_to_uuid(txt, id) < E_OK || ( href[k] != '_' && href[k] != '.' ) ) return FALSE;
	*p_name = href + k;
	return TRUE;
}

LOCAL void src_of( DTRT *p, CONST UB *href, CONST UB *format, RTM_SRC *s )
{
	TS_UUID		id;
	CONST UB	*name = NULL;
	INT		i;

	knl_memset(s, 0, sizeof(*s));
	s->obj = p->obj;
	s->recno = -1;
	if ( href == NULL ) return;
	if ( href[0] == '{' && href[1] == 'r' && href[7] == '}' ) {
		name = href + 8;
	} else if ( obj_href(href, &id, &name) ) {
		s->obj = id;
	} else {
		char	t[TS_UUID_STRLEN + 1];

		for ( i = 0; i < TS_UUID_STRLEN && href[i] != 0; i++ ) t[i] = (char)href[i];
		t[i] = 0;
		if ( i == TS_UUID_STRLEN && href[i] == 0 && ts_str_to_uuid(t, &id) >= E_OK ) {
			s->obj = id;
			name = (CONST UB *)"";
		} else {
			name = href;
		}
	}
	for ( i = 0; name[i] != 0 && i < RTM_NAME - 1; i++ ) s->name[i] = name[i];
	s->name[i] = 0;
	for ( i = 0; format != NULL && format[i] != 0 && i < 15; i++ ) s->format[i] = format[i];
	s->format[i] = 0;
}

/*
 * Whether a medium is played by MPEG Player: a video, and a sound in a
 * format 実時間メディア does not decode (it decodes WAVE and MP3), which
 * FFmpeg does
 */
LOCAL BOOL lc_is( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		UB	c = s[i];

		if ( c >= 'A' && c <= 'Z' ) c = (UB)( c - 'A' + 'a' );
		if ( w[i] == 0 || c != (UB)w[i] ) return FALSE;
	}
	return (BOOL)( w[n] == 0 );
}

LOCAL BOOL via_video( CONST RT_MEDIA *m )
{
	CONST UB	*e = NULL, *f = m->format;
	INT		i, n;

	if ( m->kind == RT_MK_VIDEO ) return TRUE;
	if ( m->kind != RT_MK_AUDIO ) return FALSE;
	if ( f == NULL || f[0] == 0 ) {
		for ( i = 0; m->href != NULL && m->href[i] != 0; i++ ) {
			if ( m->href[i] == '.' ) e = m->href + i + 1;
			else if ( m->href[i] == '/' ) e = NULL;
		}
		f = e;
	}
	if ( f == NULL ) return FALSE;
	for ( n = 0; f[n] != 0 && f[n] != '?' && f[n] != '#' && f[n] != ';'; n++ ) ;
	if ( n == 0 ) return FALSE;
	return (BOOL)!( lc_is(f, n, "mp3") || lc_is(f, n, "wav") || lc_is(f, n, "wave") || lc_is(f, n, "mpeg")
			|| lc_is(f, n, "audio/mpeg") || lc_is(f, n, "audio/wav") || lc_is(f, n, "audio/x-wav") );
}

LOCAL HELPER *helper_of( DTRT *p, RT_MEDIA *m, INT k )
{
	if ( via_video(m) ) return ( k < RT_MEDIA_MAX ) ? &p->vid[k] : NULL;
	return &p->snd;
}

/* 実時間メディア, made ready when anything is to sound */
LOCAL BOOL snd_helper( DTRT *p )
{
	TS_UUID	u;

	if ( p->snd.used ) return !p->snd.failed;
	if ( ts_str_to_uuid(SYSDEF_PROG_RTMEDIA, &u) < E_OK || !helper_make(p, &p->snd, &u, FALSE, OUTQ) ) {
		p->snd.failed = TRUE;
		return FALSE;
	}
	return TRUE;
}

LOCAL void send( DTRT *p, HELPER *h, RTM_CMD *c )
{
	if ( h == NULL || !h->used || h->failed ) return;
	oq_put(&h->oq, c);
	oq_flush(h);
}

/* The host's clock, in ns, at a time of the program */
LOCAL D at_ns( DTRT *p, RT_TIME prog_t )
{
	RT_TIME	host = p->step_now - ( rt_now(p->rt) - prog_t );

	return host * 1000;
}

LOCAL void video_box( DTRT *p, RT_MEDIA *m, INT *p_w, INT *p_h )
{
	RT_OBJ	*o = rt_obj(p->rt, m->obj);
	INT	w = 320, h = 240;

	if ( o != NULL ) {
		w = zv(p, o->p[RT_P_RIGHT]) - zv(p, o->p[RT_P_LEFT]);
		h = zv(p, o->p[RT_P_BOTTOM]) - zv(p, o->p[RT_P_TOP]);
	}
	if ( w < 16 ) w = 16;
	if ( h < 16 ) h = 16;
	while ( (D)w * h > RTM_FRAMES_MAX ) {
		w = w * 3 / 4;
		h = h * 3 / 4;
	}
	*p_w = w;
	*p_h = h;
}

LOCAL void h_media( void *arg, RT *rt, INT k, UINT op, RT_V v )
{
	DTRT		*p = (DTRT *)arg;
	RT_MEDIA	*m = rt_media(rt, k);
	HELPER		*h;
	RTM_CMD		c;

	if ( m == NULL ) return;
	if ( via_video(m) && op == RT_MO_LOAD && ( k >= RT_MEDIA_MAX || !p->vid[k].used ) ) {
		TS_UUID	pu;
		T_OBREF	r;
		BOOL	there = (BOOL)( ts_str_to_uuid(RTM_VIDEO_UUID, &pu) >= E_OK && ob_ref_obj(&pu, &r) >= E_OK );

		if ( !there || k >= RT_MEDIA_MAX || !helper_make(p, &p->vid[k], &pu, TRUE, VOUTQ) ) {
			/* nothing here decodes it */
			if ( !p->said_novideo ) {
				tell("realtime: no MPEG Player to play a video");
				p->said_novideo = TRUE;
			}
			rt_media_event(rt, k, m->gen, RT_ME_ERROR, RT_ERR_UNSUPP, p->step_now);
			return;
		}
	}
	if ( !via_video(m) && !snd_helper(p) ) {
		if ( op == RT_MO_LOAD ) rt_media_event(rt, k, m->gen, RT_ME_ERROR, RT_ERR_UNSUPP, p->step_now);
		return;
	}
	h = helper_of(p, m, k);
	knl_memset(&c, 0, sizeof(c));
	c.inst = k;
	c.gen = m->gen;
	c.v = v;
	switch ( op ) {
	case RT_MO_LOAD:
		c.op = RTM_OPEN;
		c.u.open.kind = (INT)m->kind;
		c.u.open.preload = (INT)m->preload;
		src_of(p, m->href, m->format, &c.u.open.src);
		if ( m->kind == RT_MK_VIDEO ) {
			RTM_CMD	s;

			knl_memset(&s, 0, sizeof(s));
			s.op = RTM_SIZE;
			s.inst = k;
			s.gen = m->gen;
			video_box(p, m, &s.u.size.w, &s.u.size.h);
			h->w = s.u.size.w;
			h->h = s.u.size.h;
			send(p, h, &s);
		}
		break;
	case RT_MO_PLAY:
		c.op = RTM_PLAY;
		c.at = at_ns(p, m->play_at);
		c.u.play.start = m->start;
		c.u.play.dur = m->dur;
		c.u.play.skip = m->skip;
		c.u.play.volume = m->volume;
		c.u.play.pan = m->pan;
		c.u.play.rate = m->rate;
		c.u.play.loop_start = m->loop_start;
		c.u.play.loop_end = m->loop_end;
		c.u.play.fade_in = m->fade_in;
		c.u.play.fade_out = m->fade_out;
		c.u.play.loop = m->loop;
		c.u.play.muted = m->muted;
		c.u.play.track = m->track;
		break;
	case RT_MO_PAUSE:	c.op = RTM_PAUSE; break;
	case RT_MO_RESUME:	c.op = RTM_RESUME; break;
	case RT_MO_STOP:	c.op = RTM_STOP; break;
	case RT_MO_SEEK:	c.op = RTM_SEEK; break;
	case RT_MO_VOLUME:	c.op = RTM_VOLUME; break;
	case RT_MO_PAN:		c.op = RTM_PAN; break;
	case RT_MO_RATE:	c.op = RTM_RATE; break;
	case RT_MO_MUTE:	c.op = RTM_MUTE; break;
	case RT_MO_CLOSE:	c.op = RTM_CLOSE; break;
	default:		return;
	}
	send(p, h, &c);
}

/* A <pcm> that carries its samples: decoded from Base64 into a memory object 実時間メディア reads */
LOCAL void pcm_send( DTRT *p, CONST RT_SND *s )
{
	CONST T_TADNODE	*d = NULL, *c;
	CONST UB	*b64 = NULL;
	UB		*bytes;
	INT		n;
	T_OBCRE		cr;
	TS_UUID		mu;
	ID		key;
	INT		rec = 0;
	SZ		asz = 0;
	RTM_CMD		cmd;
	CONST UB	*fmt = tad_attr(s->nd, "format");

	for ( c = s->nd->first; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_ELEM && c->name != NULL && c->name[0] == 'd' && c->name[1] == 'a'
		  && c->name[2] == 't' && c->name[3] == 'a' && c->name[4] == 0 ) {
			d = c;
		}
	}
	for ( c = ( d != NULL ) ? d->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_TEXT && c->text != NULL ) {
			b64 = c->text;
			break;
		}
	}
	if ( b64 == NULL ) return;
	n = rt_base64(b64, NULL, 0);
	if ( n <= 0 ) return;
	bytes = (UB *)Kmalloc((SZ)n);
	if ( bytes == NULL ) return;
	(void)rt_base64(b64, bytes, n);
	knl_memset(&cr, 0, sizeof(cr));
	cr.type = OB_T_STORAGE;
	cr.sub = OB_S_MEMORY;
	if ( ob_cre_obj(&cr, &mu) >= E_OK ) {
		key = ob_opn_obj(&mu, OB_OP_READ | OB_OP_WRITE | OB_OP_RECORD);
		if ( key > 0 && ob_apd_rec(key, OB_RT_SYSDATA, 0, &rec) >= E_OK
		  && ob_wri_rec(key, 0, 0, bytes, (SZ)n, &asz) >= E_OK ) {
			knl_memset(&cmd, 0, sizeof(cmd));
			cmd.op = RTM_PCM;
			cmd.stream = s->stream;
			cmd.at = s->at * 1000;
			cmd.v = RT_ONE;
			cmd.u.pcm.mem = mu;
			cmd.u.pcm.rate = rt_int(tad_attr(s->nd, "sampleRate"), 44100);
			cmd.u.pcm.channels = rt_int(tad_attr(s->nd, "channels"), 2);
			cmd.u.pcm.bits = rt_int(tad_attr(s->nd, "bits"), 16);
			cmd.u.pcm.wav = (BOOL)( fmt == NULL || ( fmt[0] == 'w' && fmt[1] == 'a' && fmt[2] == 'v' )
					    || ( n > 4 && bytes[0] == 'R' && bytes[1] == 'I' ) );
			send(p, &p->snd, &cmd);
		}
		/* the object lives as long as someone holds it: the helper opens it before this key goes */
		if ( key > 0 ) (void)ob_cls_obj(key);
	}
	Kfree(bytes);
}

LOCAL void h_sound( void *arg, RT *rt, CONST RT_SND *s )
{
	DTRT	*p = (DTRT *)arg;
	RTM_CMD	c;
	INT	k;

	(void)rt;
	if ( !snd_helper(p) ) return;
	if ( s->kind == RT_SND_PCM ) {
		pcm_send(p, s);
		return;
	}
	knl_memset(&c, 0, sizeof(c));
	c.stream = s->stream;
	c.at = s->at * 1000;
	if ( s->kind == RT_SND_NOTE ) {
		c.op = RTM_NOTE;
		c.u.note.pitch = s->pitch;
		c.u.note.velocity = s->velocity;
		c.u.note.channel = s->channel;
		c.u.note.dur = s->dur;
	} else {
		c.op = RTM_MIDI;
		c.u.midi.n = s->nmsg;
		for ( k = 0; k < s->nmsg && k < 16; k++ ) knl_memcpy(c.u.midi.msg[k], s->msg[k], 3);
	}
	send(p, &p->snd, &c);
}

/* The streams the program describes, told to 実時間メディア once it is up */
LOCAL void streams_send( DTRT *p )
{
	INT		i, k;
	RTM_CMD		c;

	for ( i = 0; i < rt_nstream(p->rt); i++ ) {
		CONST RT_STREAM	*s = rt_stream(p->rt, i);

		knl_memset(&c, 0, sizeof(c));
		c.op = RTM_STREAM;
		c.u.stream.number = s->number;
		for ( k = 0; s->device != NULL && s->device[k] != 0 && s->device[k] != '<' && k < RTM_NAME - 1; k++ ) {
			c.u.stream.device[k] = s->device[k];
		}
		while ( k > 0 && ( c.u.stream.device[k - 1] == ' ' || c.u.stream.device[k - 1] == '\n' ) ) k--;
		c.u.stream.device[k] = 0;
		for ( k = 0; s->params != NULL && s->params[k] != 0 && s->params[k] != '<' && k < RTM_PARAMS - 1; k++ ) {
			c.u.stream.params[k] = s->params[k];
		}
		c.u.stream.params[k] = 0;
		send(p, &p->snd, &c);
	}
}

/* ---------------------------------------------------------------- the answers */

LOCAL void frame_take( DTRT *p, INT k, CONST RTM_ANS *a )
{
	HELPER		*h = &p->vid[k];
	RTM_FRAMES	head;
	SZ		asz = 0;
	INT		li;
	RT_MEDIA	*m = rt_media(p->rt, k);

	if ( m == NULL || m->obj < 0 || h->mem <= 0 ) return;
	if ( ob_rea_rec(h->mem, 0, 0, &head, sizeof(head), &asz) < E_OK || head.magic != RTM_MAGIC ) return;
	if ( head.w <= 0 || head.h <= 0 || (D)head.w * head.h > RTM_FRAMES_MAX ) return;
	for ( li = 0; li < p->nlayer; li++ ) {
		LAYER	*L = &p->layer[li];

		if ( L->obj != m->obj ) continue;
		if ( L->frame == NULL || L->fw != head.w || L->fh != head.h ) {
			if ( L->frame != NULL ) Kfree(L->frame);
			L->frame = (UW *)Kmalloc(sizeof(UW) * (SZ)head.w * head.h);
			L->fw = head.w;
			L->fh = head.h;
			if ( L->frame == NULL ) return;
		}
		(void)ob_rea_rec(h->mem, 0, (D)sizeof(head) + (D)head.shown * head.w * head.h * 4, L->frame,
				 (SZ)head.w * head.h * 4, &asz);
		L->has_frame = TRUE;
		rt_obj(p->rt, m->obj)->dirty |= RT_D_PICT;
		break;
	}
	(void)a;
}

LOCAL void answers( DTRT *p )
{
	RTM_ANS	a;
	SZ	asz = 0;
	INT	n = 0;

	while ( p->rep > 0 && n++ < 256 && ob_rea_rec(p->rep, 0, 0, &a, sizeof(a), &asz) >= E_OK ) {
		HELPER	*h = NULL;
		INT	k;

		if ( asz < (SZ)sizeof(a) ) continue;
		if ( a.op == RTM_A_READY ) {
			/* which of them it is: the one started and not yet up */
			if ( p->snd.used && !p->snd.up && a.inst < 0 && a.gen == 0 && a.w == 0 ) {
				p->snd.up = TRUE;
				streams_send(p);
				oq_flush(&p->snd);
				h = &p->snd;
			}
			for ( k = 0; h == NULL && k < RT_MEDIA_MAX; k++ ) {
				if ( p->vid[k].used && !p->vid[k].up && a.inst == k ) {
					p->vid[k].up = TRUE;
					oq_flush(&p->vid[k]);
					h = &p->vid[k];
				}
			}
			continue;
		}
		if ( a.inst < 0 || a.inst >= rt_nmedia(p->rt) ) continue;
		switch ( a.op ) {
		case RTM_A_LOADED:	rt_media_event(p->rt, a.inst, a.gen, RT_ME_LOADED, a.v, p->step_now); break;
		case RTM_A_PLAYING:	rt_media_event(p->rt, a.inst, a.gen, RT_ME_PLAYING, 0, p->step_now); break;
		case RTM_A_PAUSED:	rt_media_event(p->rt, a.inst, a.gen, RT_ME_PAUSED, 0, p->step_now); break;
		case RTM_A_ENDED:	rt_media_event(p->rt, a.inst, a.gen, RT_ME_ENDED, 0, p->step_now); break;
		case RTM_A_ERROR:	rt_media_event(p->rt, a.inst, a.gen, RT_ME_ERROR, a.v, p->step_now); break;
		case RTM_A_TIME:	rt_media_event(p->rt, a.inst, a.gen, RT_ME_TIME, a.v, p->step_now); break;
		case RTM_A_FRAME:
			if ( a.inst < RT_MEDIA_MAX && rt_media(p->rt, a.inst)->gen == a.gen ) frame_take(p, a.inst, &a);
			break;
		default:
			break;
		}
	}
	oq_flush(&p->snd);
	for ( n = 0; n < RT_MEDIA_MAX; n++ ) {
		if ( p->vid[n].used ) oq_flush(&p->vid[n]);
	}
}

/* ---------------------------------------------------------------- the scene */

LOCAL CONST UB *node_id( CONST T_TADNODE *nd )
{
	CONST T_TADNODE	*c;
	CONST UB	*id;

	if ( nd == NULL ) return NULL;
	id = tad_attr(nd, "id");
	if ( id != NULL ) return id;
	/* a text box: by the id of its <text> */
	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_ELEM && c->name != NULL && c->name[0] == 't' && c->name[1] == 'e'
		  && c->name[2] == 'x' && c->name[3] == 't' && c->name[4] == 0 && tad_attr(c, "id") != NULL ) {
			return tad_attr(c, "id");
		}
	}
	return NULL;
}

LOCAL BOOL named( CONST T_TADNODE *nd, CONST char *name )
{
	INT	i;

	if ( nd == NULL || nd->kind != TAD_ND_ELEM || nd->name == NULL ) return FALSE;
	for ( i = 0; name[i] != 0; i++ ) {
		if ( nd->name[i] != (UB)name[i] ) return FALSE;
	}
	return (BOOL)( nd->name[i] == 0 );
}

LOCAL ITEM *item_new( DTRT *p, UINT kind, INT z )
{
	ITEM	*it;

	if ( p->nitem >= MAX_ITEM ) return NULL;
	it = &p->item[p->nitem];
	knl_memset(it, 0, sizeof(*it));
	it->kind = kind;
	it->z = z;
	it->ord = p->nitem++;
	it->obj = -1;
	return it;
}

/* What the program moves is bound to the box the layout gave it */
LOCAL void bind( DTRT *p, ITEM *it, CONST T_TADNODE *nd, UINT kind )
{
	CONST UB	*id = node_id(nd);

	if ( id != NULL && rt_wants(p->rt, id) ) {
		INT	k = rt_bind(p->rt, id, kind, &it->box, it->z, nd);

		if ( k >= 0 ) it->obj = k;
	}
}

LOCAL void add_figure( DTRT *p, T_TVFIG *f )
{
	INT	i;

	if ( f == NULL || p->nfig >= MAX_FIG ) return;
	p->fig[p->nfig++] = f;
	for ( i = 0; i < f->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &f->sh[i];
		ITEM		*it = item_new(p, IT_FIGSH, s->z);
		UINT		k;

		if ( it == NULL ) return;
		it->fig = f;
		it->sh = i;
		it->box = s->r;
		if ( s->kind == TV_SH_LINE || s->kind == TV_SH_CURVE ) {
			/* a line's box is its points' */
			it->box.right++;
			it->box.bottom++;
		}
		k = ( s->kind == TV_SH_DOC ) ? RT_OB_TEXT : ( s->kind == TV_SH_IMAGE || s->kind == TV_SH_PIXMAP )
		  ? RT_OB_IMAGE : RT_OB_SHAPE;
		bind(p, it, s->node, k);
	}
}

LOCAL void add_document( DTRT *p, CONST T_TADNODE *nd )
{
	T_TVDOC		*d = NULL;
	ITEM		*it;
	CONST T_TADNODE	*c;
	INT		z = 0;

	if ( p->ndoc >= MAX_DOC || tv_doc_of(nd, &d) < E_OK || d == NULL ) return;
	p->doc[p->ndoc++] = d;
	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( named(c, "text") && tad_attr(c, "zIndex") != NULL ) z = rt_int(tad_attr(c, "zIndex"), 0);
	}
	it = item_new(p, IT_DOC, z);
	if ( it == NULL ) return;
	it->doc = d;
	it->box = d->view;
	if ( r_empty(&it->box) ) {
		it->box.left = 0;
		it->box.top = 0;
		it->box.right = p->tw;
		it->box.bottom = p->th;
	}
	bind(p, it, nd, RT_OB_TEXT);
}

/* The children of a <realtime> that are shown: its figures and its text boxes */
LOCAL void add_realtime( DTRT *p, CONST T_TADNODE *rtn )
{
	CONST T_TADNODE	*c;

	for ( c = rtn->first; c != NULL; c = c->next ) {
		if ( named(c, "figure") ) {
			T_TVFIG	*f = NULL;

			if ( tv_fig_of(c, &f) >= E_OK ) add_figure(p, f);
		} else if ( named(c, "document") ) {
			add_document(p, c);
		}
	}
}

/* The pictures and videos the program brings in: each an item of its own */
LOCAL void add_program( DTRT *p )
{
	INT	k;

	for ( k = 0; k < rt_nobj(p->rt); k++ ) {
		RT_OBJ	*o = rt_obj(p->rt, k);
		ITEM	*it;

		if ( ( o->kind != RT_OB_IMAGE && o->kind != RT_OB_VIDEO ) || o->nd == NULL ) continue;
		/* bound by the layout already: a picture of a figure */
		if ( !named(o->nd, "image") && !named(o->nd, "video") ) continue;
		if ( o->kind == RT_OB_IMAGE && o->base_shown ) continue;
		it = item_new(p, IT_OBJ, o->z);
		if ( it == NULL ) return;
		it->obj = k;
		it->box.left = (INT)( o->p[RT_P_LEFT] / RT_ONE );
		it->box.top = (INT)( o->p[RT_P_TOP] / RT_ONE );
		it->box.right = (INT)( o->p[RT_P_RIGHT] / RT_ONE );
		it->box.bottom = (INT)( o->p[RT_P_BOTTOM] / RT_ONE );
	}
}

LOCAL void sort_items( DTRT *p )
{
	INT	i, j;

	for ( i = 1; i < p->nitem; i++ ) {
		ITEM	t = p->item[i];

		j = i - 1;
		while ( j >= 0 && ( p->item[j].z > t.z || ( p->item[j].z == t.z && p->item[j].ord > t.ord ) ) ) {
			p->item[j + 1] = p->item[j];
			j--;
		}
		p->item[j + 1] = t;
	}
}

/* ---------------------------------------------------------------- drawing into pictures */

/*
 * An environment of the program's memory drawing into px, at whose
 * corner 0, 0 stands (at is of the picture's own corner): what is drawn
 * is in px once it is closed
 */
LOCAL INT env_on( UW *px, CONST T_DPRECT *at )
{
	T_DPBMP	b;

	b.px = px;
	b.w = at->right - at->left;
	b.h = at->bottom - at->top;
	b.pitch = b.w * (INT)sizeof(UW);
	return dp_open_mem(&b);
}

/*
 * One item drawn with the point (dx, dy) of the zoomed scene at the
 * corner of the picture: a picture of something that starts off the
 * scene (to slide in) is drawn in its own place, not at a corner the
 * scene leaves out.
 */
LOCAL void item_draw( DTRT *p, INT gid, CONST ITEM *it, INT dx, INT dy )
{
	T_DPRECT	all, box;

	all.left = -dx;
	all.top = -dy;
	all.right = zs(p, p->tw) - dx;
	all.bottom = zs(p, p->th) - dy;
	switch ( it->kind ) {
	case IT_FIGSH: {
		T_TVFIG	sub = *it->fig;
		INT	k;

		sub.sh = &it->fig->sh[it->sh];
		sub.nsh = 1;
		sub.zoom = p->z;
		sub.zoom_den = p->zd;
		sub.bare = TRUE;		/* its pieces of text over the picture of the rest */
		sub.over_on = 0;
		for ( k = 0; k < TV_MAX_OVER; k++ ) sub.over[k] = NULL;
		{
			/* the figure keeps to the box it is given: the scene and the picture together */
			T_DPENV	env;
			INT	ox = all.left, oy = all.top;

			if ( dp_ref(gid, &env) >= E_OK ) r_join(&all, &env.visible);
			(void)tv_fig_draw_at(gid, &sub, &all, all.left - ox, all.top - oy, p->rec, TAD_COL_NONE, 0);
		}
		break;
	}
	case IT_DOC:
	case IT_PAGE:
		box.left = zs(p, it->box.left) - dx;
		box.top = zs(p, it->box.top) - dy;
		box.right = zs(p, it->box.right) - dx;
		box.bottom = zs(p, it->box.bottom) - dy;
		if ( it->kind == IT_PAGE ) {
			INT	pad = 16 * p->z / p->zd;

			box.left += pad;
			box.top += pad;
			box.right -= pad;
		}
		/* the text by itself onto the picture of the rest, magnified as the scene is */
		it->doc->zoom = p->z;
		it->doc->zoom_den = p->zd;
		it->doc->bare = TRUE;
		(void)tv_doc_draw(gid, it->doc, &box, 0, p->rec, TAD_COL_NONE);
		break;
	case IT_OBJ: {
		RT_OBJ	*o = rt_obj(p->rt, it->obj);

		box.left = zs(p, it->box.left) - dx;
		box.top = zs(p, it->box.top) - dy;
		box.right = zs(p, it->box.right) - dx;
		box.bottom = zs(p, it->box.bottom) - dy;
		if ( o != NULL && o->href != NULL ) tv_draw_picture(gid, &box, o->href);
		break;
	}
	default:
		break;
	}
}

/* What the items cover, zoomed, with room round for strokes */
LOCAL void items_box( DTRT *p, INT first, INT n, T_DPRECT *r )
{
	INT	i, m = MARGIN * p->z / p->zd + 2;

	knl_memset(r, 0, sizeof(*r));
	for ( i = first; i < first + n; i++ ) {
		ITEM		*it = &p->item[i];
		T_DPRECT	b;
		INT		lw = 0;

		if ( it->kind == IT_FIGSH ) lw = it->fig->sh[it->sh].line_w;
		b.left = zs(p, it->box.left) - m - zs(p, lw);
		b.top = zs(p, it->box.top) - m - zs(p, lw);
		b.right = zs(p, it->box.right) + m + zs(p, lw);
		b.bottom = zs(p, it->box.bottom) + m + zs(p, lw);
		if ( it->kind == IT_FIGSH && it->fig->sh[it->sh].kind == TV_SH_DOC ) {
			/* letters may hang below a text box's last line */
			b.bottom += zs(p, 24);
		}
		r_join(r, &b);
	}
}

/*
 * Draw a run of items on black and on white: what differs between the
 * two is how much each pixel covers, and the black one, brought up by
 * that, is its colour.
 */
LOCAL UW *render( DTRT *p, INT first, INT n, CONST T_DPRECT *at, CONST UB *href_over )
{
	INT	w = at->right - at->left, h = at->bottom - at->top, i, gid;
	UW	*bk, *wh;
	SZ	np = (SZ)w * h;
	T_DPRECT own;

	/* drawn with the picture's own corner at 0, 0 */
	own.left = 0;
	own.top = 0;
	own.right = w;
	own.bottom = h;

	if ( w <= 0 || h <= 0 ) return NULL;
	bk = (UW *)Kmalloc(sizeof(UW) * np);
	wh = (UW *)Kmalloc(sizeof(UW) * np);
	if ( bk == NULL || wh == NULL ) {
		if ( bk != NULL ) Kfree(bk);
		if ( wh != NULL ) Kfree(wh);
		return NULL;
	}
	for ( i = 0; i < 2; i++ ) {
		UW	*px = ( i == 0 ) ? bk : wh;
		SZ	k;
		INT	j;

		for ( k = 0; k < np; k++ ) px[k] = ( i == 0 ) ? 0x00000000U : 0x00FFFFFFU;
		gid = env_on(px, &own);
		if ( gid < 0 ) continue;
		for ( j = first; j < first + n; j++ ) {
			if ( href_over != NULL && p->item[j].kind != IT_FIGSH && p->item[j].kind != IT_OBJ ) continue;
			if ( href_over != NULL ) {
				T_DPRECT	box;

				box.left = zs(p, p->item[j].box.left) - at->left;
				box.top = zs(p, p->item[j].box.top) - at->top;
				box.right = zs(p, p->item[j].box.right) - at->left;
				box.bottom = zs(p, p->item[j].box.bottom) - at->top;
				tv_draw_picture(gid, &box, href_over);
			} else {
				item_draw(p, gid, &p->item[j], at->left, at->top);
			}
		}
		dp_close(gid);
	}
	for ( np = (SZ)w * h; np > 0; np-- ) {
		UW	b = bk[np - 1], c = wh[np - 1];
		INT	dr = (INT)( ( c >> 16 ) & 0xFF ) - (INT)( ( b >> 16 ) & 0xFF );
		INT	dg = (INT)( ( c >> 8 ) & 0xFF ) - (INT)( ( b >> 8 ) & 0xFF );
		INT	db = (INT)( c & 0xFF ) - (INT)( b & 0xFF );
		INT	d = ( dr > dg ) ? dr : dg, a;

		if ( db > d ) d = db;
		if ( d < 0 ) d = 0;
		a = 255 - d;
		if ( a <= 0 ) {
			bk[np - 1] = 0;
		} else if ( a >= 255 ) {
			bk[np - 1] = 0xFF000000U | ( b & 0x00FFFFFFU );
		} else {
			INT	r = (INT)( ( b >> 16 ) & 0xFF ) * 255 / a, g = (INT)( ( b >> 8 ) & 0xFF ) * 255 / a;
			INT	bl = (INT)( b & 0xFF ) * 255 / a;

			if ( r > 255 ) r = 255;
			if ( g > 255 ) g = 255;
			if ( bl > 255 ) bl = 255;
			bk[np - 1] = ( (UW)a << 24 ) | ( (UW)r << 16 ) | ( (UW)g << 8 ) | (UW)bl;
		}
	}
	Kfree(wh);
	return bk;
}

LOCAL void layer_free( LAYER *L )
{
	INT	k;

	if ( L->px != NULL ) Kfree(L->px);
	if ( L->frame != NULL ) Kfree(L->frame);
	for ( k = 0; k < L->nalt; k++ ) {
		if ( L->alt[k].px != NULL ) Kfree(L->alt[k].px);
	}
	knl_memset(L, 0, sizeof(*L));
}

/* The layers: runs of items that stay, and each item the program moves */
LOCAL BOOL layers_make( DTRT *p )
{
	INT	i = 0, gid;
	SZ	k, np;
	T_DPRECT all;

	p->layer = (LAYER *)Kmalloc(sizeof(LAYER) * (SZ)( p->nitem + 1 ));
	if ( p->layer == NULL ) return FALSE;
	knl_memset(p->layer, 0, sizeof(LAYER) * (SZ)( p->nitem + 1 ));
	all.left = 0;
	all.top = 0;
	all.right = p->fw;
	all.bottom = p->fh;
	np = (SZ)p->fw * p->fh;
	p->base = (UW *)Kmalloc(sizeof(UW) * np);
	p->fb = (UW *)Kmalloc(sizeof(UW) * np);
	if ( p->base == NULL || p->fb == NULL ) return FALSE;
	for ( k = 0; k < np; k++ ) p->base[k] = p->paper & 0x00FFFFFFU;

	tv_src_self(&p->obj);
	/* the ground: what stays and is under everything that moves, drawn straight */
	gid = env_on(p->base, &all);
	while ( i < p->nitem && p->item[i].obj < 0 ) {
		if ( gid >= 0 ) item_draw(p, gid, &p->item[i], 0, 0);
		i++;
	}
	if ( gid >= 0 ) dp_close(gid);

	while ( i < p->nitem ) {
		LAYER	*L = &p->layer[p->nlayer];
		INT	j = i;

		L->cur = -1;
		if ( p->item[i].obj >= 0 ) {
			RT_OBJ	*o = rt_obj(p->rt, p->item[i].obj);

			L->obj = p->item[i].obj;
			L->first = i;
			L->n = 1;
			i++;
			L->base.left = zv(p, o->base[RT_P_LEFT]);
			L->base.top = zv(p, o->base[RT_P_TOP]);
			L->base.right = zv(p, o->base[RT_P_RIGHT]);
			L->base.bottom = zv(p, o->base[RT_P_BOTTOM]);
			L->video = (BOOL)( o->kind == RT_OB_VIDEO );
			if ( L->video ) {
				CONST T_TADNODE	*nd = o->nd;

				L->at = L->base;
				L->lw = ( rt_int(tad_attr(nd, "lineType"), 0) != 0 ) ? rt_int(tad_attr(nd, "lineWidth"), 0) : 0;
				L->lcol = 0x00000000U;
				if ( tad_attr(nd, "l_pat") != NULL ) {
					UW		col = 0;
					CONST UH	*tex = NULL;

					if ( tv_pattern(rt_int(tad_attr(nd, "l_pat"), 0), &col, &tex) ) L->lcol = col;
				}
				/* its poster until its first frame */
				if ( o->href != NULL ) {
					L->px = render(p, L->first, 1, &L->at, o->href);
					L->w = L->at.right - L->at.left;
					L->h = L->at.bottom - L->at.top;
				}
			} else {
				items_box(p, L->first, 1, &L->at);
				L->px = render(p, L->first, 1, &L->at, NULL);
				L->w = L->at.right - L->at.left;
				L->h = L->at.bottom - L->at.top;
			}
			/* the pictures a replace may put in its place, ready now */
			for ( j = 0; L->nalt < MAX_ALT; j++ ) {
				CONST UB	*ref = NULL;
				CONST UB	*href = rt_replace_href(p->rt, j, &ref);
				INT		q;

				if ( href == NULL ) break;
				if ( !same_str(rt_obj(p->rt, L->obj)->id, ref) ) continue;
				for ( q = 0; q < L->nalt && !same_str(L->alt[q].href, href); q++ ) ;
				if ( q < L->nalt ) continue;
				L->alt[L->nalt].href = href;
				L->alt[L->nalt].px = render(p, L->first, 1, &L->at, href);
				L->alt[L->nalt].w = L->w;
				L->alt[L->nalt].h = L->h;
				L->nalt++;
			}
		} else {
			L->obj = -1;
			L->first = i;
			while ( i < p->nitem && p->item[i].obj < 0 ) i++;
			L->n = i - L->first;
			items_box(p, L->first, L->n, &L->at);
			(void)r_cut(&L->at, &all);
			L->px = render(p, L->first, L->n, &L->at, NULL);
			L->w = L->at.right - L->at.left;
			L->h = L->at.bottom - L->at.top;
		}
		p->nlayer++;
	}
	tv_src_self(NULL);
	return TRUE;
}

/* ---------------------------------------------------------------- laying it down */

/* One pixel over another, 'a' of 255 of it showing */
LOCAL void blend( UW *d, UW s, UINT a )
{
	UW	x;
	UINT	r, g, b;

	if ( a == 0 ) return;
	if ( a >= 255 ) {
		*d = s & 0x00FFFFFFU;
		return;
	}
	x = *d;
	r = ( x >> 16 ) & 0xFF;
	g = ( x >> 8 ) & 0xFF;
	b = x & 0xFF;
	r = (UINT)( (INT)r + ( ( (INT)( ( s >> 16 ) & 0xFF ) - (INT)r ) * (INT)a ) / 255 );
	g = (UINT)( (INT)g + ( ( (INT)( ( s >> 8 ) & 0xFF ) - (INT)g ) * (INT)a ) / 255 );
	b = (UINT)( (INT)b + ( ( (INT)( s & 0xFF ) - (INT)b ) * (INT)a ) / 255 );
	*d = ( r << 16 ) | ( g << 8 ) | b;
}

LOCAL void compose_static( DTRT *p, CONST LAYER *L, CONST T_DPRECT *R )
{
	T_DPRECT	c = L->at;
	INT		x, y;

	if ( L->px == NULL || !r_cut(&c, R) ) return;
	for ( y = c.top; y < c.bottom; y++ ) {
		CONST UW	*s = L->px + (SZ)( y - L->at.top ) * L->w + ( c.left - L->at.left );
		UW		*d = p->fb + (SZ)y * p->fw + c.left;

		for ( x = c.left; x < c.right; x++, s++, d++ ) blend(d, *s, *s >> 24);
	}
}

/*
 * Where a sprite goes: the inverse of its move, scale, turn and flip, as
 * an affine map from the pixels of the scene to those of its picture.
 */
typedef struct {
	CONST UW	*bm;
	INT		bw, bh;
	BOOL		solid;		/* a video's frame: every pixel shows */
	INT		alpha;		/* 0 .. 255 */
	D		u0, v0;		/* 16.16: the picture's pixel at bbox's corner */
	D		dux, duy, dvx, dvy;
	T_DPRECT	bbox;		/* the scene's pixels it may reach */
	T_DPRECT	dest;		/* its box now */
} GEO;

#define F16	65536

/* A point of the scene (16.16) to a point of the picture (16.16) */
LOCAL void inv_map( D x, D y, D cdx, D cdy, D cbx, D cby, INT ca, INT sa, D mx, D my, D ax, D ay,
		    D stx, D sty, D *pu, D *pv )
{
	D	qx = x - cdx, qy = y - cdy;
	D	rx = ( qx * ca + qy * sa ) / 16384;
	D	ry = ( -qx * sa + qy * ca ) / 16384;
	D	px = ( mx != 0 ) ? rx * F16 / mx : 0;
	D	py = ( my != 0 ) ? ry * F16 / my : 0;

	*pu = ( cbx + px - ax ) * F16 / stx;
	*pv = ( cby + py - ay ) * F16 / sty;
}

LOCAL BOOL sprite_geo( DTRT *p, CONST LAYER *L, CONST RT_OBJ *o, GEO *g )
{
	CONST T_DPRECT	*at;
	D		cdx, cdy, cbx, cby, mx, my, stx, sty, ax, ay, u1, v1;
	INT		a, ca, sa, k;
	D		bw_s, bh_s, dw, dh;

	knl_memset(g, 0, sizeof(*g));
	if ( !o->shown ) return FALSE;
	a = (INT)( o->p[RT_P_ALPHA] * 255 / RT_ONE );
	if ( a <= 0 ) return FALSE;
	g->alpha = ( a > 255 ) ? 255 : a;
	g->dest.left = zv(p, o->p[RT_P_LEFT]);
	g->dest.top = zv(p, o->p[RT_P_TOP]);
	g->dest.right = zv(p, o->p[RT_P_RIGHT]);
	g->dest.bottom = zv(p, o->p[RT_P_BOTTOM]);
	if ( L->video && L->has_frame && L->frame != NULL ) {
		g->bm = L->frame;
		g->bw = L->fw;
		g->bh = L->fh;
		g->solid = TRUE;
		at = &L->base;
	} else if ( L->cur >= 0 && L->cur < L->nalt && L->alt[L->cur].px != NULL ) {
		g->bm = L->alt[L->cur].px;
		g->bw = L->alt[L->cur].w;
		g->bh = L->alt[L->cur].h;
		at = &L->at;
	} else {
		g->bm = L->px;
		g->bw = L->w;
		g->bh = L->h;
		at = &L->at;
	}
	if ( g->bm == NULL || g->bw <= 0 || g->bh <= 0 ) return FALSE;
	/* the base box to the box now, then its own scale, a flip turning it over */
	bw_s = L->base.right - L->base.left;
	bh_s = L->base.bottom - L->base.top;
	dw = g->dest.right - g->dest.left;
	dh = g->dest.bottom - g->dest.top;
	mx = ( bw_s > 0 ) ? dw * F16 / bw_s : F16;
	my = ( bh_s > 0 ) ? dh * F16 / bh_s : F16;
	mx = mx * o->p[RT_P_SX] / RT_ONE;
	my = my * o->p[RT_P_SY] / RT_ONE;
	if ( mx == 0 || my == 0 ) return FALSE;
	if ( o->flip_h ) mx = -mx;
	if ( o->flip_v ) my = -my;
	cbx = ( (D)( L->base.left + L->base.right ) << 16 ) / 2;
	cby = ( (D)( L->base.top + L->base.bottom ) << 16 ) / 2;
	cdx = ( (D)( g->dest.left + g->dest.right ) << 16 ) / 2;
	cdy = ( (D)( g->dest.top + g->dest.bottom ) << 16 ) / 2;
	ax = (D)at->left << 16;
	ay = (D)at->top << 16;
	stx = ( (D)( at->right - at->left ) << 16 ) / g->bw;
	sty = ( (D)( at->bottom - at->top ) << 16 ) / g->bh;
	if ( stx == 0 ) stx = F16;
	if ( sty == 0 ) sty = F16;
	k = (INT)( ( o->p[RT_P_ROT] / 1000 ) * 4096 / 360000 );
	ca = tv_cos(k);
	sa = tv_sin(k);

	/* the corners of the picture, forward, for what it may reach */
	{
		D	xs[2], ys[2];
		INT	i, j;
		D	minx = 0, miny = 0, maxx = 0, maxy = 0;

		xs[0] = (D)at->left << 16;
		xs[1] = (D)at->right << 16;
		ys[0] = (D)at->top << 16;
		ys[1] = (D)at->bottom << 16;
		for ( i = 0; i < 2; i++ ) {
			for ( j = 0; j < 2; j++ ) {
				D	qx = ( xs[i] - cbx ) * mx / F16, qy = ( ys[j] - cby ) * my / F16;
				D	fx = ( qx * ca - qy * sa ) / 16384 + cdx;
				D	fy = ( qx * sa + qy * ca ) / 16384 + cdy;

				if ( ( i | j ) == 0 || fx < minx ) minx = fx;
				if ( ( i | j ) == 0 || fy < miny ) miny = fy;
				if ( ( i | j ) == 0 || fx > maxx ) maxx = fx;
				if ( ( i | j ) == 0 || fy > maxy ) maxy = fy;
			}
		}
		g->bbox.left = (INT)( minx >> 16 ) - 1;
		g->bbox.top = (INT)( miny >> 16 ) - 1;
		g->bbox.right = (INT)( maxx >> 16 ) + 2;
		g->bbox.bottom = (INT)( maxy >> 16 ) + 2;
	}
	/* the inverse, as an affine map from the corner of bbox: centres of pixels */
	inv_map(( (D)g->bbox.left << 16 ) + F16 / 2, ( (D)g->bbox.top << 16 ) + F16 / 2, cdx, cdy, cbx, cby,
		ca, sa, mx, my, ax, ay, stx, sty, &g->u0, &g->v0);
	inv_map(( (D)( g->bbox.left + 1024 ) << 16 ) + F16 / 2, ( (D)g->bbox.top << 16 ) + F16 / 2, cdx, cdy,
		cbx, cby, ca, sa, mx, my, ax, ay, stx, sty, &u1, &v1);
	g->dux = ( u1 - g->u0 ) / 1024;
	g->dvx = ( v1 - g->v0 ) / 1024;
	inv_map(( (D)g->bbox.left << 16 ) + F16 / 2, ( (D)( g->bbox.top + 1024 ) << 16 ) + F16 / 2, cdx, cdy,
		cbx, cby, ca, sa, mx, my, ax, ay, stx, sty, &u1, &v1);
	g->duy = ( u1 - g->u0 ) / 1024;
	g->dvy = ( v1 - g->v0 ) / 1024;
	return TRUE;
}

LOCAL void compose_sprite( DTRT *p, CONST LAYER *L, CONST T_DPRECT *R )
{
	RT_OBJ		*o = rt_obj(p->rt, L->obj);
	GEO		g;
	T_DPRECT	c;
	INT		x, y;

	if ( o == NULL || !sprite_geo(p, L, o, &g) ) return;
	c = g.bbox;
	if ( !r_cut(&c, R) ) return;
	for ( y = c.top; y < c.bottom; y++ ) {
		D	u = g.u0 + g.dux * ( c.left - g.bbox.left ) + g.duy * ( y - g.bbox.top );
		D	v = g.v0 + g.dvx * ( c.left - g.bbox.left ) + g.dvy * ( y - g.bbox.top );
		UW	*d = p->fb + (SZ)y * p->fw + c.left;

		for ( x = c.left; x < c.right; x++, d++, u += g.dux, v += g.dvx ) {
			INT	iu, iv;
			UW	s;
			UINT	a;

			if ( u < 0 || v < 0 ) continue;
			iu = (INT)( u >> 16 );
			iv = (INT)( v >> 16 );
			if ( iu >= g.bw || iv >= g.bh ) continue;
			s = g.bm[(SZ)iv * g.bw + iu];
			a = g.solid ? 255U : ( s >> 24 );
			blend(d, s, a * (UINT)g.alpha / 255);
		}
	}
	/* a video's border, round its box as it stands */
	if ( L->video && L->lw > 0 ) {
		INT		w = zs(p, L->lw), side;
		T_DPRECT	b[4];

		if ( w < 1 ) w = 1;
		b[0] = g.dest;
		b[0].bottom = b[0].top + w;
		b[1] = g.dest;
		b[1].top = b[1].bottom - w;
		b[2] = g.dest;
		b[2].right = b[2].left + w;
		b[3] = g.dest;
		b[3].left = b[3].right - w;
		for ( side = 0; side < 4; side++ ) {
			if ( !r_cut(&b[side], R) ) continue;
			for ( y = b[side].top; y < b[side].bottom; y++ ) {
				for ( x = b[side].left; x < b[side].right; x++ ) {
					blend(p->fb + (SZ)y * p->fw + x, L->lcol, (UINT)g.alpha);
				}
			}
		}
	}
}

/* A part of the scene made again, and put in the window */
LOCAL void compose( DTRT *p, CONST T_DPRECT *r0 )
{
	T_DPRECT	all, R, shown;
	INT		y, i;

	all.left = 0;
	all.top = 0;
	all.right = p->fw;
	all.bottom = p->fh;
	R = *r0;
	if ( !r_cut(&R, &all) ) return;
	for ( y = R.top; y < R.bottom; y++ ) {
		knl_memcpy(p->fb + (SZ)y * p->fw + R.left, p->base + (SZ)y * p->fw + R.left,
			   (INT)( sizeof(UW) * (SZ)( R.right - R.left ) ));
	}
	for ( i = 0; i < p->nlayer; i++ ) {
		if ( p->layer[i].obj < 0 ) compose_static(p, &p->layer[i], &R);
		else compose_sprite(p, &p->layer[i], &R);
	}
	(void)dp_put_argb(p->gid, p->fit.left + R.left, p->fit.top + R.top, p->fb + (SZ)R.top * p->fw + R.left,
			  p->fw, R.right - R.left, R.bottom - R.top, WM_CLEAR);
	shown.left = p->fit.left + R.left;
	shown.top = p->fit.top + R.top;
	shown.right = p->fit.left + R.right;
	shown.bottom = p->fit.top + R.bottom;
	(void)wm_obj_flush(p->kw, &shown);
}

LOCAL void dirty_add( DTRT *p, CONST T_DPRECT *r )
{
	INT	i;

	if ( r_empty(r) ) return;
	for ( i = 0; i < p->ndirty; i++ ) {
		T_DPRECT	t = p->dirty[i];

		/* one that touches another is joined to it */
		if ( r->left <= t.right && r->right >= t.left && r->top <= t.bottom && r->bottom >= t.top ) {
			r_join(&p->dirty[i], r);
			return;
		}
	}
	if ( p->ndirty < MAX_DIRTY ) {
		p->dirty[p->ndirty++] = *r;
	} else {
		r_join(&p->dirty[MAX_DIRTY - 1], r);
	}
}

/* What the program changed since the last frame: where each sprite was, and is */
LOCAL void track( DTRT *p )
{
	INT	i;

	for ( i = 0; i < p->nlayer; i++ ) {
		LAYER	*L = &p->layer[i];
		RT_OBJ	*o;
		GEO	g;

		if ( L->obj < 0 ) continue;
		o = rt_obj(p->rt, L->obj);
		if ( o == NULL || o->dirty == 0 ) continue;
		if ( o->dirty & RT_D_PICT ) {
			INT	k;

			L->cur = -1;
			for ( k = 0; k < L->nalt; k++ ) {
				if ( same_str(L->alt[k].href, o->href) ) L->cur = k;
			}
		}
		o->dirty = 0;
		dirty_add(p, &L->was);
		if ( sprite_geo(p, L, o, &g) ) {
			L->was = g.bbox;
			dirty_add(p, &g.bbox);
		} else {
			knl_memset(&L->was, 0, sizeof(L->was));
		}
	}
	/* objects the scene does not show: nothing to do but forget the change */
	for ( i = 0; i < rt_nobj(p->rt); i++ ) rt_obj(p->rt, i)->dirty = 0;
}

LOCAL BOOL frame_out( DTRT *p )
{
	INT	i;
	BOOL	any = (BOOL)( p->ndirty > 0 );

	for ( i = 0; i < p->ndirty; i++ ) compose(p, &p->dirty[i]);
	p->ndirty = 0;
	return any;
}

/* ---------------------------------------------------------------- the task */

LOCAL BOOL media_busy( DTRT *p )
{
	INT	k;

	for ( k = 0; k < rt_nmedia(p->rt); k++ ) {
		UINT	s = rt_media(p->rt, k)->state;

		if ( s == RT_MS_LOADING || s == RT_MS_PLAYING || s == RT_MS_READY ) return TRUE;
	}
	return FALSE;
}

/* What the record asks of the slides, kept for the program that plays it; elsewhere nothing */
LOCAL void action_take( DTRT *p )
{
	UINT	a = p->act;

	if ( a == 0 ) return;
	p->act = 0;
	if ( p->slide && a != RT_ACT_DONE ) {
		p->go = a;
		p->go_n = p->act_n;
	}
}

LOCAL void play_body( void *exinf )
{
	DTRT	*p = (DTRT *)exinf;
	BOOL	begun = FALSE;

	p->tid = tk_get_tid();
	lock(p);
	while ( !p->quit ) {
		RT_TIME	now, next = RT_NEVER, wait;
		INT	k;

		if ( p->snd.used && p->snd.start ) helper_start(p, &p->snd);
		for ( k = 0; k < RT_MEDIA_MAX; k++ ) {
			if ( p->vid[k].used && p->vid[k].start ) helper_start(p, &p->vid[k]);
		}
		now = now_us();
		p->step_now = now;
		answers(p);
		if ( !begun ) {
			/* the sound has to be there before the first note is due */
			if ( !p->hold_start || p->snd.up || p->snd.failed || now > p->hold_until ) {
				T_DPRECT	all;

				rt_start(p->rt, now);
				begun = TRUE;
				/* the scene as the program starts it: what fades in is not there yet */
				all.left = 0;
				all.top = 0;
				all.right = p->fw;
				all.bottom = p->fh;
				dirty_add(p, &all);
			}
		}
		if ( begun ) next = rt_step(p->rt, now);
		action_take(p);
		track(p);
		if ( now - p->last_frame >= 15000 || next == RT_NEVER ) {
			if ( frame_out(p) ) p->last_frame = now;
		}
		wait = ( next == RT_NEVER ) ? 100000 : next - now_us();
		if ( p->ndirty > 0 && wait > 15000 ) wait = 15000;
		if ( ( media_busy(p) || !begun ) && wait > 10000 ) wait = 10000;
		if ( wait < 1000 ) wait = 1000;
		unlock(p);
		(void)tk_slp_tsk((TMO)( ( wait + 999 ) / 1000 ));
		lock(p);
	}
	unlock(p);
	p->gone = TRUE;
}

LOCAL void play_task( INT stacd, void *exinf )
{
	(void)stacd;
	play_body(exinf);
	tk_exd_tsk();
}

/* The player's task, as the program says it is made, or one of the kernel's */
LOCAL ER task_start( DTRT *p, CONST RTP_OPEN *o )
{
	T_CTSK	ct;
	T_RTSK	r;
	ID	tid;

	if ( o->spawn != NULL ) return o->spawn(play_body, p);
	/* at the priority of the program's own task: the highest it may have */
	if ( tk_ref_tsk(TSK_SELF, &r) < E_OK ) r.tskbpri = 16;
	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = p;
	ct.tskatr = TA_HLNG | TA_RNG3;
	ct.task = (FP)play_task;
	ct.itskpri = r.tskbpri;
	ct.stksz = RT_TASK_STK;
	tid = tk_cre_tsk(&ct);
	if ( tid < E_OK ) return (ER)tid;
	if ( tk_sta_tsk(tid, 0) < E_OK ) {
		(void)tk_del_tsk(tid);
		return E_SYS;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- opening and closing */

EXPORT BOOL rtp_has( CONST T_TAD *rec )
{
	return (BOOL)( rt_find(rec) != NULL );
}

/* The scene a record is made for: its figure's view, or 1920 by 1080 */
EXPORT void rtp_scene_size( CONST T_TAD *rec, INT *p_w, INT *p_h )
{
	CONST T_TADNODE	*rtn = rt_find(rec), *c;

	*p_w = 1920;
	*p_h = 1080;
	for ( c = ( rtn != NULL ) ? rtn->first : NULL; c != NULL; c = c->next ) {
		if ( named(c, "figure") ) {
			CONST T_TADNODE	*v;

			for ( v = c->first; v != NULL; v = v->next ) {
				if ( named(v, "figView") ) {
					INT	w = rt_int(tad_attr(v, "right"), 0) - rt_int(tad_attr(v, "left"), 0);
					INT	h = rt_int(tad_attr(v, "bottom"), 0) - rt_int(tad_attr(v, "top"), 0);

					if ( w > 0 && h > 0 ) {
						*p_w = w;
						*p_h = h;
						return;
					}
				}
			}
		}
	}
}

/* The scene fitted to area, its shape kept, in the middle */
LOCAL void fit_to( DTRT *p, CONST T_DPRECT *area )
{
	p->area = *area;
	p->z = sl_fit(area->right - area->left, area->bottom - area->top, p->tw, p->th, &p->fit, &p->zd);
	p->fit.left += area->left;
	p->fit.right += area->left;
	p->fit.top += area->top;
	p->fit.bottom += area->top;
	p->fw = p->fit.right - p->fit.left;
	p->fh = p->fit.bottom - p->fit.top;
}

/* The ground of the area, under the scene */
LOCAL void ground_draw( DTRT *p )
{
	if ( p->ground == TAD_COL_NONE ) return;
	(void)dp_fill_rect(p->gid, &p->area, p->ground);
}

LOCAL void close_all( DTRT *p );

EXPORT RTPLAY *rtp_open( CONST TS_UUID *obj, INT recno, CONST RTP_OPEN *o )
{
	DTRT		*p;
	T_TAD		*rec;
	CONST T_TADNODE	*rtn, *body;
	RT_HOST		host;
	T_CSEM		cs;
	T_DPRECT	all;
	INT		i, tw = o->tw, th = o->th;
	UW		paper = o->paper;
	SZ		len = 0;
	UB		*xml;

	if ( recno < 0 ) recno = 0;
	/*
	 * The record read for the player alone: what the drawing keeps of
	 * records may be let go of while the player plays, and the program
	 * points into the tree as long as it runs
	 */
	xml = tv_src_read(obj, recno, &len);
	rec = NULL;
	if ( xml == NULL || tad_parse(xml, len, NULL, &rec) < E_OK || rec == NULL ) {
		if ( xml != NULL ) Kfree(xml);
		return NULL;
	}
	rtn = rt_find(rec);
	p = ( rtn != NULL ) ? (DTRT *)Kmalloc(sizeof(DTRT)) : NULL;
	if ( p == NULL ) {
		tad_free(rec);
		Kfree(xml);
		return NULL;
	}
	knl_memset(p, 0, sizeof(*p));
	p->xml = xml;
	p->gid = o->gid;
	p->kw = o->kw;
	p->port = o->port;
	p->obj = *obj;
	p->recno = recno;
	p->rec = rec;
	p->slide = o->slide;
	p->slide_no = o->slide ? o->slide_no : 0;
	p->ground = o->ground;
	p->track = -1;
	if ( paper == TAD_COL_NONE ) {
		T_VOBJ	v;

		knl_memset(&v, 0, sizeof(v));
		v.target = *obj;
		v.disp = TAD_D_DEFAULT;
		if ( tv_src_look(&v, NULL, 0, &paper) < 0 || paper == TV_PAPER_LOOK ) {
			paper = o->slide ? 0x00FFFFFFU : 0x00000000U;
		}
	}
	p->paper = paper;
	if ( !o->slide || tw <= 0 || th <= 0 ) rtp_scene_size(rec, &tw, &th);
	p->tw = tw;
	p->th = th;
	knl_memset(&host, 0, sizeof(host));
	host.arg = p;
	host.alloc = h_alloc;
	host.free = h_free;
	host.media = h_media;
	host.sound = h_sound;
	host.action = h_action;
	host.log = h_log;
	if ( rt_new(rtn, &host, &p->rt) < E_OK ) {
		p->rt = NULL;
		close_all(p);
		return NULL;
	}
	rt_set_var(p->rt, RT_SV_SLIDE, p->slide_no);
	fit_to(p, &o->area);

	/* what is shown: the page or figure round the <realtime>, its own figures and texts */
	p->item = (ITEM *)Kmalloc(sizeof(ITEM) * MAX_ITEM);
	if ( p->item == NULL ) {
		close_all(p);
		return NULL;
	}
	tv_src_self(obj);
	body = tad_body(rec);
	if ( named(body, "document") ) {
		T_TVDOC	*d = NULL;

		if ( tv_doc(rec, &d) >= E_OK && d != NULL && p->ndoc < MAX_DOC ) {
			ITEM	*it = item_new(p, IT_PAGE, -0x7FFFFFFF);

			p->doc[p->ndoc++] = d;
			if ( it != NULL ) {
				it->doc = d;
				it->box.right = tw;
				it->box.bottom = th;
			}
		}
	} else if ( named(body, "figure") ) {
		T_TVFIG	*f = NULL;

		if ( tv_fig(rec, &f) >= E_OK ) add_figure(p, f);
	}
	add_realtime(p, rtn);
	add_program(p);
	sort_items(p);
	tv_src_prefetch(rec);
	if ( !layers_make(p) ) {
		close_all(p);
		return NULL;
	}
	tell("realtime: %d things shown, %d layers, %d objects, %d media, zoom %d/%d", p->nitem, p->nlayer,
	     rt_nobj(p->rt), rt_nmedia(p->rt), p->z, p->zd);

	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.isemcnt = 1;
	cs.maxsem = 1;
	p->mtx = tk_cre_sem(&cs);
	self_find();
	(void)chan_make(&p->rep_u, &p->rep, OB_OP_READ);
	if ( rt_sounds(p->rt) && snd_helper(p) ) {
		p->hold_start = TRUE;
		p->hold_until = now_us() + 3000000;
	}
	all.left = 0;
	all.top = 0;
	all.right = p->fw;
	all.bottom = p->fh;
	/* where each sprite is drawn now, to be drawn over when it goes or fades out */
	for ( i = 0; i < p->nlayer; i++ ) {
		LAYER	*L = &p->layer[i];
		RT_OBJ	*ob;
		GEO	g;

		if ( L->obj < 0 || ( ob = rt_obj(p->rt, L->obj) ) == NULL ) continue;
		if ( sprite_geo(p, L, ob, &g) ) L->was = g.bbox;
	}
	ground_draw(p);
	compose(p, &all);

	if ( task_start(p, o) < E_OK ) {
		tell("realtime: the player's task could not be started");
		p->gone = TRUE;
	}
	if ( ++rtp_last == 0 ) rtp_last = 1;
	p->no = rtp_last;
	tell("player: hosted %u in a window of process %d%s", p->no, (INT)ts_get_pid(), p->slide ? " as a slide" : "");
	return p;
}

/* Everything it holds let go of; its task has ended, or never began */
LOCAL void close_all( DTRT *p )
{
	INT	i;

	if ( p->rt != NULL ) rt_stop(p->rt);
	helper_end(&p->snd);
	for ( i = 0; i < RT_MEDIA_MAX; i++ ) helper_end(&p->vid[i]);
	if ( p->rep > 0 ) {
		(void)ob_cls_obj(p->rep);
		(void)ob_del_obj(&p->rep_u);
	}
	for ( i = 0; i < p->nlayer; i++ ) layer_free(&p->layer[i]);
	if ( p->layer != NULL ) Kfree(p->layer);
	if ( p->base != NULL ) Kfree(p->base);
	if ( p->fb != NULL ) Kfree(p->fb);
	for ( i = 0; i < p->nfig; i++ ) tv_fig_free(p->fig[i]);
	for ( i = 0; i < p->ndoc; i++ ) tv_doc_free(p->doc[i]);
	if ( p->item != NULL ) Kfree(p->item);
	if ( p->rt != NULL ) rt_delete(p->rt);
	if ( p->rec != NULL ) tad_free(p->rec);
	if ( p->xml != NULL ) Kfree(p->xml);
	if ( p->mtx > 0 ) (void)tk_del_sem(p->mtx);
	Kfree(p);
}

EXPORT void rtp_close( RTPLAY *p )
{
	INT	i;

	if ( p == NULL ) return;
	p->quit = TRUE;
	if ( !p->gone ) {
		for ( i = 0; i < 400 && !p->gone; i++ ) {
			wake(p);
			(void)tk_dly_tsk(5);
		}
	}
	tell("player: %u closed by its process", p->no);
	close_all(p);
}

EXPORT void rtp_fit( RTPLAY *p, T_DPRECT *r )
{
	*r = p->fit;
}

/* ---------------------------------------------------------------- what the person does */

/* The name a key goes by in a <trigger key> */
LOCAL CONST char *key_name( UINT code, char *buf )
{
	if ( code >= 0x04 && code <= 0x1D ) {
		buf[0] = (char)( 'a' + code - 0x04 );
		buf[1] = 0;
		return buf;
	}
	if ( code >= 0x1E && code <= 0x27 ) {
		buf[0] = ( code == 0x27 ) ? '0' : (char)( '1' + code - 0x1E );
		buf[1] = 0;
		return buf;
	}
	switch ( code ) {
	case 0x28:	return "Enter";
	case 0x29:	return "Escape";
	case 0x2A:	return "Backspace";
	case 0x2B:	return "Tab";
	case 0x2C:	return "Space";
	case 0x4A:	return "Home";
	case 0x4B:	return "PageUp";
	case 0x4D:	return "End";
	case 0x4E:	return "PageDown";
	case 0x4F:	return "ArrowRight";
	case 0x50:	return "ArrowLeft";
	case 0x51:	return "ArrowDown";
	case 0x52:	return "ArrowUp";
	default:	return "";
	}
}

/*
 * Something the person did in the window (HID_EV_*), given to the
 * program: TRUE when a trigger or a wait of it took it. With 'look' it
 * is only asked.
 */
LOCAL BOOL input( DTRT *p, UINT type, INT x, INT y, UINT code, BOOL look )
{
	RT_IN	in;
	char	kb[4];
	BOOL	took = FALSE;
	RT_TIME	now = now_us();
	UD	when = (UD)now * 1000U;

	knl_memset(&in, 0, sizeof(in));
	in.x = ( x - p->fit.left ) * p->zd / p->z;
	in.y = ( y - p->fit.top ) * p->zd / p->z;
	lock(p);
	switch ( type ) {
	case HID_EV_BTN_DOWN:
		if ( !look ) {
			UD	gap = (UD)wm_look(WM_LOOK_DBLTIME) * 1000000U;

			p->down = TRUE;
			p->down_x = in.x;
			p->down_y = in.y;
			in.kind = ( when - p->last_click < gap && in.x - p->last_cx < 8 && p->last_cx - in.x < 8 )
				? RT_IN_DBLCLICK : RT_IN_CLICK;
			p->last_click = when;
			p->last_cx = in.x;
			p->last_cy = in.y;
			took = rt_input(p->rt, &in, now);
			if ( in.kind == RT_IN_DBLCLICK ) {
				in.kind = RT_IN_CLICK;
				took = (BOOL)( rt_input(p->rt, &in, now) || took );
			}
		} else {
			in.kind = RT_IN_CLICK;
			took = rt_would_take(p->rt, &in);
		}
		break;
	case HID_EV_BTN_UP:
		if ( !p->down ) break;
		if ( !look ) p->down = FALSE;
		{
			INT	dx = in.x - p->down_x, dy = in.y - p->down_y;
			INT	ax = ( dx < 0 ) ? -dx : dx, ay = ( dy < 0 ) ? -dy : dy;

			if ( ax < SWIPE && ay < SWIPE ) break;
			in.kind = ( ax >= ay ) ? ( dx < 0 ? RT_IN_SWIPE_L : RT_IN_SWIPE_R )
					       : ( dy < 0 ? RT_IN_SWIPE_U : RT_IN_SWIPE_D );
			took = look ? rt_would_take(p->rt, &in) : rt_input(p->rt, &in, now);
		}
		break;
	case HID_EV_MOVE:
		in.kind = RT_IN_MOVE;
		if ( !look ) (void)rt_input(p->rt, &in, now);
		break;
	case HID_EV_KEY_DOWN:
	case HID_EV_KEY_UP:
		in.kind = ( type == HID_EV_KEY_DOWN ) ? RT_IN_KEYDOWN : RT_IN_KEYUP;
		in.key = key_name(code, kb);
		in.code = code;
		took = look ? rt_would_take(p->rt, &in) : rt_input(p->rt, &in, now);
		break;
	default:
		break;
	}
	unlock(p);
	if ( !look ) wake(p);
	return took;
}

/* The window was resized or shown again: everything handed back */
LOCAL void visible( DTRT *p, BOOL on )
{
	RT_IN	in;

	knl_memset(&in, 0, sizeof(in));
	in.kind = RT_IN_VISIBLE;
	in.x = on ? 1 : 0;
	lock(p);
	(void)rt_input(p->rt, &in, now_us());
	unlock(p);
	wake(p);
}

/* ---------------------------------------------------------------- what the program does to it */

/* 再生 / 一時停止: begun when it waits, held, or let go */
LOCAL void toggle( DTRT *p )
{
	RT_TIME	now = now_us();

	lock(p);
	p->step_now = now;
	if ( rt_waiting(p->rt) ) rt_begin(p->rt, now);
	else if ( rt_paused(p->rt) ) rt_resume(p->rt, now);
	else if ( rt_done(p->rt) || !rt_running(p->rt) ) rt_start(p->rt, now);
	else rt_pause(p->rt, now);
	unlock(p);
	wake(p);
}

/* 停止: back to the start, and held there */
LOCAL void stop( DTRT *p )
{
	RT_TIME	now = now_us();

	lock(p);
	p->step_now = now;
	rt_start(p->rt, now);
	rt_pause(p->rt, now);
	unlock(p);
	wake(p);
}

LOCAL void restart( DTRT *p )
{
	RT_TIME	now = now_us();

	lock(p);
	p->step_now = now;
	rt_start(p->rt, now);
	if ( rt_waiting(p->rt) ) rt_begin(p->rt, now);
	unlock(p);
	wake(p);
}

/* Forward or back by so many milliseconds of the program */
LOCAL void seek_by( DTRT *p, INT ms )
{
	RT_TIME	to, now = now_us();

	lock(p);
	to = rt_now(p->rt) + (RT_TIME)ms * 1000;
	p->step_now = now;
	rt_seek(p->rt, ( to > 0 ) ? to : 0, now);
	unlock(p);
	wake(p);
}

/* A label cut to at most max bytes with its terminator, not in the middle of a letter */
LOCAL void label_cut( UB *dst, INT max, CONST UB *src )
{
	INT	n = 0;

	while ( src[n] != 0 && n < max - 1 ) n++;
	if ( src[n] != 0 ) {
		while ( n > 0 && ( src[n] & 0xC0 ) == 0x80 ) n--;
	}
	knl_memcpy(dst, src, n);
	dst[n] = 0;
}

/* The media of the program as a list, with what their titles say */
LOCAL INT tracks( DTRT *p, UB (*names)[WM_LABEL_MAX], INT *idx, INT max )
{
	INT	k, n = 0;

	lock(p);
	for ( k = 0; k < rt_nmedia(p->rt) && n < max; k++ ) {
		RT_MEDIA	*m = rt_media(p->rt, k);
		UB		line[WM_LABEL_MAX * 2];
		INT		j = 0, q;
		CONST UB	*t = ( m->title != NULL ) ? m->title : ( m->id != NULL ) ? m->id : m->href;

		if ( m->kind == RT_MK_PCM ) continue;
		if ( m->trackno > 0 ) {
			line[j++] = (UB)( '0' + m->trackno / 10 % 10 );
			line[j++] = (UB)( '0' + m->trackno % 10 );
			line[j++] = '.';
			line[j++] = ' ';
		}
		for ( q = 0; t != NULL && t[q] != 0 && j < (INT)sizeof(line) - 4; q++ ) line[j++] = t[q];
		if ( m->artist != NULL && j < (INT)sizeof(line) - 8 ) {
			line[j++] = ' ';
			line[j++] = ':';
			line[j++] = ' ';
			for ( q = 0; m->artist[q] != 0 && j < (INT)sizeof(line) - 1; q++ ) line[j++] = m->artist[q];
		}
		line[j] = 0;
		label_cut(names[n], WM_LABEL_MAX, line);
		idx[n++] = k;
	}
	unlock(p);
	return n;
}

/* The medium playing (or paused) now, -1 none */
LOCAL INT playing( DTRT *p )
{
	INT	k, r = -1;

	lock(p);
	for ( k = 0; k < rt_nmedia(p->rt) && r < 0; k++ ) {
		RT_MEDIA	*m = rt_media(p->rt, k);

		if ( m->kind != RT_MK_PCM && ( m->state == RT_MS_PLAYING || m->state == RT_MS_PAUSED ) ) r = k;
	}
	unlock(p);
	return r;
}

/* One of them played now, the others of its kind stopped */
LOCAL void play_track( DTRT *p, INT k )
{
	RT_TIME	now = now_us();

	lock(p);
	p->step_now = now;
	rt_play_only(p->rt, k, now);
	unlock(p);
	wake(p);
}

/* What plays now: its title and who made it */
LOCAL void now_playing( DTRT *p, UB *buf, INT max )
{
	INT	k, n = 0, q;

	buf[0] = 0;
	lock(p);
	for ( k = 0; k < rt_nmedia(p->rt) && n == 0; k++ ) {
		RT_MEDIA	*m = rt_media(p->rt, k);

		if ( m->state != RT_MS_PLAYING || m->title == NULL ) continue;
		for ( q = 0; m->title[q] != 0 && n < max - 1; q++ ) buf[n++] = m->title[q];
		if ( m->artist != NULL && n < max - 4 ) {
			buf[n++] = ' ';
			buf[n++] = '/';
			buf[n++] = ' ';
			for ( q = 0; m->artist[q] != 0 && n < max - 1; q++ ) buf[n++] = m->artist[q];
		}
		buf[n] = 0;
	}
	unlock(p);
}

/* The window's size changed: the scene laid out again at the new size, where the program has it now */
LOCAL void relayout( DTRT *p, CONST T_DPRECT *area )
{
	INT		i;
	T_DPRECT	all;

	lock(p);
	for ( i = 0; i < p->nlayer; i++ ) layer_free(&p->layer[i]);
	if ( p->layer != NULL ) Kfree(p->layer);
	if ( p->base != NULL ) Kfree(p->base);
	if ( p->fb != NULL ) Kfree(p->fb);
	p->layer = NULL;
	p->base = p->fb = NULL;
	p->nlayer = 0;
	p->ndirty = 0;
	fit_to(p, area);
	ground_draw(p);
	tv_src_self(&p->obj);
	if ( layers_make(p) ) {
		for ( i = 0; i < rt_nobj(p->rt); i++ ) rt_obj(p->rt, i)->dirty = RT_D_BOX;
		track(p);
		p->ndirty = 0;
		all.left = 0;
		all.top = 0;
		all.right = p->fw;
		all.bottom = p->fh;
		compose(p, &all);
		/* the videos make their frames for the new size */
		for ( i = 0; i < RT_MEDIA_MAX && i < rt_nmedia(p->rt); i++ ) {
			RT_MEDIA	*m = rt_media(p->rt, i);
			RTM_CMD		c;

			if ( !p->vid[i].used || m->kind != RT_MK_VIDEO ) continue;
			knl_memset(&c, 0, sizeof(c));
			c.op = RTM_SIZE;
			c.inst = i;
			c.gen = m->gen;
			video_box(p, m, &c.u.size.w, &c.u.size.h);
			send(p, &p->vid[i], &c);
		}
	} else {
		(void)wm_obj_flush(p->kw, &p->area);
	}
	unlock(p);
}

/* The ground and the scene drawn again, as they are */
LOCAL void redraw( DTRT *p )
{
	T_DPRECT	all;

	lock(p);
	ground_draw(p);
	all.left = 0;
	all.top = 0;
	all.right = p->fw;
	all.bottom = p->fh;
	if ( p->base != NULL ) compose(p, &all);
	else (void)wm_obj_flush(p->kw, &p->area);
	unlock(p);
}

/* ---------------------------------------------------------------- the panels */

#define PN_LIST		1		/* プレイリスト */
#define PN_GROUND	2		/* 背景色変更 */
#define P_LIST		11
#define P_OK		102
#define K_ENTER		0x28
#define K_KPENTER	0x58

/*
 * One of a list chosen in a panel of the player's data box
 * (SYSDEF_BOX_RTPLAY) whose notices come to the window's channel: its
 * index, or -1 when it was put away. *p_closed is set when the window
 * was asked to close meanwhile, which the program is then to do.
 */
LOCAL INT choose( DTRT *p, INT panel, CONST char *CONST *names, INT n, INT at, BOOL *p_closed )
{
	UI	base, q;
	T_OBNTM	m;
	SZ	asz = 0;
	INT	num, ev, done = 0, got = -1;

	if ( p->port <= 0 ) return -1;
	knl_memset(&base, 0, sizeof(base));
	if ( ts_str_to_uuid(SYSDEF_BOX_RTPLAY, &base.box) < E_OK ) return -1;
	base.port = p->port;
	if ( ui_dlg_open(&base, panel, &q) < E_OK ) return -1;
	ui_names(&q, P_LIST, names, n);
	if ( at >= 0 ) ui_set_value(&q, P_LIST, at + 1);	/* a list's value counts its rows from 1, 0 none */
	while ( done == 0 ) {
		ev = ( ob_rea_tmo(p->port, 0, 0, &m, sizeof(m), &asz, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num)
										     : UI_EV_CLOSE;
		switch ( ev ) {
		case UI_EV_BUTTON:
			done = ( num == P_OK ) ? 1 : -1;
			break;
		case UI_EV_CLOSE:
			done = -1;
			break;
		case UI_EV_NONE:
			if ( m.event == OB_E_KEY && ( m.code == K_ENTER || m.code == K_KPENTER ) ) done = 1;
			break;
		case UI_EV_OTHER:
			if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
				*p_closed = TRUE;
				done = -1;
			}
			break;
		default:
			break;
		}
	}
	if ( done > 0 ) got = ui_value(&q, P_LIST) - 1;
	ui_dlg_close(&q);
	return ( got >= 0 && got < n ) ? got : -1;
}

#define TRACKS		64

/* The medium so many after (before) the one playing, or the one chosen last */
LOCAL void step_track( DTRT *p, INT by )
{
	UB	(*names)[WM_LABEL_MAX] = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * TRACKS);
	INT	idx[TRACKS], n, k, now;

	if ( names == NULL ) return;
	n = tracks(p, names, idx, TRACKS);
	Kfree(names);
	if ( n <= 0 ) return;
	now = playing(p);
	for ( k = 0; k < n && idx[k] != now; k++ ) ;
	if ( k < n ) p->track = k;
	p->track = ( p->track < 0 ) ? 0 : ( ( p->track + by ) % n + n ) % n;
	play_track(p, idx[p->track]);
}

/* プレイリスト表示: the media of the program in a panel, the one chosen played */
LOCAL void list_pick( DTRT *p, BOOL *p_closed )
{
	UB		(*names)[WM_LABEL_MAX] = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * TRACKS);
	CONST char	**list = (CONST char **)Kmalloc(sizeof(char *) * TRACKS);
	INT		idx[TRACKS], n, i, at;

	if ( names != NULL && list != NULL ) {
		n = tracks(p, names, idx, TRACKS);
		if ( n == 0 ) {
			(void)wm_msg_put("プレイリストに曲がありません");
		} else {
			for ( i = 0; i < n; i++ ) list[i] = (CONST char *)names[i];
			at = choose(p, PN_LIST, list, n, p->track, p_closed);
			if ( at >= 0 && at < n ) {
				p->track = at;
				play_track(p, idx[at]);
			}
		}
	}
	if ( names != NULL ) Kfree(names);
	if ( list != NULL ) Kfree((void *)list);
}

/* 背景色変更: what is round the scene, chosen in a panel */
LOCAL void ground_pick( DTRT *p, BOOL *p_closed )
{
	static CONST char *CONST names[4] = { "黒", "白", "グレー", "青" };
	static CONST UW		cols[4] = { 0x00000000U, 0x00FFFFFFU, 0x00808080U, 0x00000080U };
	INT			at = choose(p, PN_GROUND, names, 4, -1, p_closed);

	if ( at >= 0 && at < 4 ) p->ground = cols[at];
}

/* ---------------------------------------------------------------- rtp_do */

/* What each RTP_* is called on the console */
LOCAL CONST char *CONST op_names[] = {
	"state", "toggle", "stop", "restart", "seek", "track", "area", "redraw",
	"ground", "list", "press", "release", "key", "move"
};
#define NOPS	( (INT)( sizeof(op_names) / sizeof(op_names[0]) ) )

/* How the program goes */
LOCAL void state_of( DTRT *p, RTP_HOW *st )
{
	UINT	s = 0;
	RT_TIME	t;

	lock(p);
	if ( rt_waiting(p->rt) ) s |= RTP_S_WAITING;
	else if ( rt_done(p->rt) ) s |= RTP_S_DONE;
	else if ( rt_paused(p->rt) ) s |= RTP_S_PAUSED;
	else s |= RTP_S_PLAYING;
	t = rt_now(p->rt);
	action_take(p);
	unlock(p);
	st->flags = ( st->flags & ( RTP_S_TAKEN | RTP_S_CLOSED ) ) | s;
	if ( p->slide && p->go != 0 ) {
		st->flags |= RTP_S_GO;
		st->go = p->go;
		st->go_n = p->go_n;
		p->go = 0;
	}
	st->ms = (INT)( t / 1000 );
	{
		UB	(*names)[WM_LABEL_MAX] = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * TRACKS);
		INT	idx[TRACKS];

		st->media = ( names != NULL ) ? tracks(p, names, idx, TRACKS) : 0;
		if ( names != NULL ) Kfree(names);
	}
	now_playing(p, st->now, RTP_TEXT_MAX);
}

/* A key: the program's own goes to the record only when the record takes it; any other always */
LOCAL BOOL key_in( DTRT *p, UINT code, UINT how )
{
	UINT	type = ( how & RTP_KEY_UP ) ? HID_EV_KEY_UP : HID_EV_KEY_DOWN;

	if ( input(p, type, 0, 0, code, TRUE) ) {
		(void)input(p, type, 0, 0, code, FALSE);
		return TRUE;
	}
	if ( ( how & RTP_KEY_MINE ) == 0 ) (void)input(p, type, 0, 0, code, FALSE);
	return FALSE;
}

EXPORT ER rtp_do( RTPLAY *p, UINT op, INT a, INT b, RTP_HOW *st )
{
	T_DPRECT	r;
	BOOL		closed = FALSE;

	if ( p == NULL ) return E_NOEXS;
	knl_memset(st, 0, sizeof(*st));
	if ( op > RTP_STATE && (INT)op < NOPS && ( op != RTP_MOVE || !p->moved ) ) {
		if ( op == RTP_MOVE ) p->moved = TRUE;
		tell("player: %u op %s", p->no, op_names[op]);
	}
	switch ( op ) {
	case RTP_STATE:		break;
	case RTP_TOGGLE:	toggle(p);		break;
	case RTP_STOP:		stop(p);		break;
	case RTP_RESTART:	restart(p);		break;
	case RTP_SEEK:		seek_by(p, a);		break;
	case RTP_TRACK:		step_track(p, a);	break;
	case RTP_AREA:
		r.left = 0;
		r.top = 0;
		r.right = ( a > MIN_AREA ) ? a : MIN_AREA;
		r.bottom = ( b > MIN_AREA ) ? b : MIN_AREA;
		relayout(p, &r);
		visible(p, TRUE);
		break;
	case RTP_REDRAW:	redraw(p);		break;
	case RTP_GROUND:	ground_pick(p, &closed); redraw(p); break;
	case RTP_LIST:		list_pick(p, &closed);	break;
	case RTP_PRESS:
	case RTP_RELEASE:
		if ( input(p, ( op == RTP_PRESS ) ? HID_EV_BTN_DOWN : HID_EV_BTN_UP, a, b, 0, FALSE) ) {
			st->flags |= RTP_S_TAKEN;
		}
		break;
	case RTP_MOVE:
		/* the pointer over the scene, for mouseenter and mouseleave */
		(void)input(p, HID_EV_MOVE, a, b, 0, FALSE);
		break;
	case RTP_KEY:
		if ( key_in(p, (UINT)a, (UINT)b) ) st->flags |= RTP_S_TAKEN;
		break;
	default:
		return E_PAR;
	}
	if ( closed ) st->flags |= RTP_S_CLOSED;
	state_of(p, st);
	return E_OK;
}
