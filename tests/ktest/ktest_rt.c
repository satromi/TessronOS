/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_rt.c
 *	実時間制御TAD (design 15): the engine of lib/librtad run on a clock
 *	of the test's own, and its segments in binary TAD
 *
 *	A program is read from xmlTAD and stepped a millisecond at a time;
 *	what it asks of the host -- notes, media, actions -- is written
 *	down with the program time it came at and compared with what the
 *	record says. Media are pretended: a file loads in 5 ms and plays
 *	for 3 s, one named "missing" fails. Then the same records are
 *	taken to binary TAD and back, and must run the same.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tad.h>
#include <ts/rtad.h>
#include <ts/btbk.h>
#include <ts/bpk.h>
#include <ts/slide.h>
#include "../../application/desktop/desktop.h"

#define MS		1000			/* microseconds */
#define NEV		256

/* What the program asked for */
typedef struct {
	UB	kind;			/* 'n' note, 'm' media, 'a' action */
	RT_TIME	at;			/* when it is to be heard, or when it was asked */
	INT	a, b, c;		/* note: pitch, velocity, stream; media: inst, op; action: act, n */
	RT_TIME	dur;
} EV;

typedef struct {
	INT	state;			/* 0 idle, 1 loading, 2 playing, 3 paused */
	RT_TIME	until;			/* loaded at, or playing to */
	INT	gen;
} SIM;

LOCAL EV	*ev;
LOCAL INT	nev;
LOCAL RT_TIME	clk;
LOCAL SIM	sim[RT_MEDIA_MAX];

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL s_eq( CONST UB *a, CONST char *b )
{
	INT	i;

	if ( a == NULL ) return FALSE;
	for ( i = 0; b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 );
}

LOCAL BOOL has_sub( CONST UB *s, CONST char *w )
{
	INT	i, k;

	if ( s == NULL ) return FALSE;
	for ( i = 0; s[i] != 0; i++ ) {
		for ( k = 0; w[k] != 0 && s[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL void add( UB kind, RT_TIME at, INT a, INT b, INT c, RT_TIME dur )
{
	if ( nev >= NEV ) return;
	ev[nev].kind = kind;
	ev[nev].at = at;
	ev[nev].a = a;
	ev[nev].b = b;
	ev[nev].c = c;
	ev[nev].dur = dur;
	nev++;
}

LOCAL void *h_alloc( void *arg, SZ size )
{
	void	*m = Kmalloc(size);

	if ( m != NULL ) knl_memset(m, 0, (INT)size);
	return m;
}

LOCAL void h_free( void *arg, void *p )
{
	Kfree(p);
}

LOCAL RT_TIME media_len( CONST RT_MEDIA *m )
{
	return 3000 * MS;
}

LOCAL void h_media( void *arg, RT *rt, INT k, UINT op, RT_V v )
{
	RT_MEDIA *m = rt_media(rt, k);

	add('m', clk, k, (INT)op, 0, 0);
	sim[k].gen = m->gen;
	switch ( op ) {
	case RT_MO_LOAD:
		sim[k].state = 1;
		sim[k].until = clk + 5 * MS;
		break;
	case RT_MO_PLAY:
		sim[k].state = 2;
		sim[k].until = m->loop ? RT_NEVER : clk + media_len(m) - m->skip;
		break;
	case RT_MO_STOP:
	case RT_MO_CLOSE:
		sim[k].state = 0;
		break;
	case RT_MO_PAUSE:
		if ( sim[k].state == 2 ) {
			sim[k].state = 3;
			sim[k].until -= clk;
		}
		break;
	case RT_MO_RESUME:
		if ( sim[k].state == 3 ) {
			sim[k].state = 2;
			sim[k].until += clk;
		}
		break;
	default:
		break;
	}
}

LOCAL void h_sound( void *arg, RT *rt, CONST RT_SND *s )
{
	if ( s->kind == RT_SND_NOTE ) add('n', s->at, s->pitch, s->velocity, s->stream, s->dur);
	else if ( s->kind == RT_SND_MIDI ) add('i', s->at, s->msg[0][0], s->msg[0][1], s->nmsg, 0);
}

LOCAL void h_action( void *arg, RT *rt, UINT act, INT n )
{
	add('a', clk, (INT)act, n, 0, 0);
}

LOCAL void h_log( void *arg, CONST char *s )
{
}

LOCAL CONST RT_HOST host = { NULL, h_alloc, h_free, h_media, h_sound, h_action, h_log };

/* The media pretended: loaded, failed, ended when their time comes */
LOCAL void media_tick( RT *rt )
{
	INT	k;

	for ( k = 0; k < rt_nmedia(rt) && k < RT_MEDIA_MAX; k++ ) {
		RT_MEDIA *m = rt_media(rt, k);

		if ( sim[k].state == 1 && clk >= sim[k].until ) {
			sim[k].state = 0;
			if ( has_sub(m->href, "missing") ) {
				rt_media_event(rt, k, sim[k].gen, RT_ME_ERROR, RT_ERR_NOTFOUND, clk);
			} else {
				sim[k].state = 4;	/* loaded, not playing */
				rt_media_event(rt, k, sim[k].gen, RT_ME_LOADED, media_len(m), clk);
			}
		}
		if ( sim[k].state == 2 && clk >= sim[k].until ) {
			sim[k].state = 0;
			rt_media_event(rt, k, sim[k].gen, RT_ME_ENDED, 0, clk);
		}
	}
}

/* An input at a given time, for run() */
typedef struct {
	RT_TIME		at;
	UINT		kind;
	CONST char	*key;
} INPUT;

/*
 * The program of xml, its ids bound to boxes at 100,100-200,200, run
 * from 0 until it is done or `until`; the events written down. The
 * program is left for the caller (rt_delete, tad_free) in *p_rt, *p_doc
 */
LOCAL ER load( CONST char *xml, T_TAD **p_doc, RT **p_rt )
{
	static CONST char *const ids[] = { "box1", "ball", "title", "card", NULL };
	ER	er;
	INT	k;

	*p_doc = NULL;
	*p_rt = NULL;
	er = tad_parse((CONST UB *)xml, s_len(xml), NULL, p_doc);
	if ( er < E_OK ) return er;
	er = rt_new(rt_find(*p_doc), &host, p_rt);
	if ( er < E_OK ) {
		tad_free(*p_doc);
		*p_doc = NULL;
		return er;
	}
	for ( k = 0; ids[k] != NULL; k++ ) {
		if ( rt_wants(*p_rt, (CONST UB *)ids[k]) && rt_obj_find(*p_rt, (CONST UB *)ids[k]) < 0 ) {
			T_DPRECT r;

			r.left = 100; r.top = 100; r.right = 200; r.bottom = 200;
			(void)rt_bind(*p_rt, (CONST UB *)ids[k], RT_OB_SHAPE, &r, 10, NULL);
		}
	}
	return E_OK;
}

LOCAL void unload( T_TAD *doc, RT *rt )
{
	if ( rt != NULL ) rt_delete(rt);
	if ( doc != NULL ) tad_free(doc);
}

LOCAL void reset( void )
{
	nev = 0;
	clk = 0;
	knl_memset(sim, 0, sizeof(sim));
}

/* Steps rt from where clk is to `until` a millisecond at a time; the inputs given when their time comes */
LOCAL void run_to( RT *rt, RT_TIME until, CONST INPUT *in, INT nin )
{
	INT	i;

	for ( ; clk <= until; clk += MS ) {
		media_tick(rt);
		for ( i = 0; i < nin; i++ ) {
			if ( in[i].at == clk ) {
				RT_IN	e;

				knl_memset(&e, 0, sizeof(e));
				e.kind = in[i].kind;
				e.x = e.y = 150;
				e.key = in[i].key;
				(void)rt_input(rt, &e, clk);
			}
		}
		(void)rt_step(rt, clk);
		if ( rt_done(rt) ) return;
	}
}

LOCAL INT run( CONST char *xml, RT_TIME until, CONST INPUT *in, INT nin, UINT seed, INT *vars )
{
	T_TAD	*doc;
	RT	*rt;

	reset();
	if ( load(xml, &doc, &rt) < E_OK ) return -1;
	if ( seed != 0 ) rt_seed(rt, seed);
	rt_start(rt, 0);
	if ( rt_waiting(rt) ) rt_begin(rt, 0);
	run_to(rt, until, in, nin);
	if ( vars != NULL ) {
		vars[0] = rt_var(rt, 1);
		vars[1] = rt_var(rt, 2);
		vars[2] = rt_var(rt, 0x10);
		vars[3] = rt_var(rt, 0x11);
	}
	unload(doc, rt);
	return nev;
}

/* The n-th note written down, NULL when there are fewer */
LOCAL CONST EV *note( INT n )
{
	INT	i;

	for ( i = 0; i < nev; i++ ) {
		if ( ev[i].kind == 'n' && n-- == 0 ) return &ev[i];
	}
	return NULL;
}

LOCAL INT notes( void )
{
	INT	i, k = 0;

	for ( i = 0; i < nev; i++ ) k += ( ev[i].kind == 'n' );
	return k;
}

#define NOTE_AT(i, p, t)	do { CONST EV *_e = note(i); KT_ASSERT(_e != NULL); \
				     if ( _e != NULL ) { KT_ASSERT_EQ(_e->a, p); KT_ASSERT_EQ(_e->at, t); } } while (0)

/* ---------------------------------------------------------------- the records */

LOCAL CONST char prog_flow[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
	"<realtime autoplay=\"true\">\n"
	"<realData>\n"
	"<stream number=\"1\"><deviceName>inst:Piano</deviceName></stream>\n"
	"<assign var=\"0x0001\"><expr><const value=\"3\"/><const value=\"4\"/><op type=\"mul\"/></expr></assign>\n"
	"<if>\n"
	"<expr><var id=\"0x0001\"/><const value=\"10\"/><op type=\"gt\"/></expr>\n"
	"<then><note pitch=\"C4\" duration=\"100\"/></then>\n"
	"<elseif><expr><const value=\"1\"/></expr><then><note pitch=\"D4\" duration=\"100\"/></then></elseif>\n"
	"<else><note pitch=\"E4\" duration=\"100\"/></else>\n"
	"</if>\n"
	"<realGroup type=\"serial\" stream=\"1\">\n"
	"<timeDelta value=\"100\"/>\n"
	"<note pitch=\"F4\" duration=\"100\"/>\n"
	"<timeAbsolute value=\"300\"/>\n"
	"<note pitch=\"G4\" duration=\"100\"/>\n"
	"<timeAbsolute value=\"50\"/>\n"
	"<note pitch=\"A4\" duration=\"100\"/>\n"
	"</realGroup>\n"
	"<note pitch=\"B4\" duration=\"100\"/>\n"
	"<unitSystem id=\"9\"><primaryUnit><dimension>s</dimension><derivation ratio=\"1/2\" sexp=\"-1\"/></primaryUnit>\n"
	"<auxiliaryUnit number=\"0\" name=\"tick\" derivation=\"1/480\"/><auxiliaryUnit number=\"1\" name=\"8\" derivation=\"1/2\"/></unitSystem>\n"
	"<unitSpec unitId=\"9\" unitNumber=\"0\"/>\n"
	"<timeDelta value=\"480\"/>\n"
	"<note pitch=\"60\" duration=\"480\"/>\n"
	"<unitSpecTemp unitId=\"9\" unitNumber=\"1\"/>\n"
	"<timeDelta value=\"1\"/>\n"
	"<note pitch=\"62\" duration=\"480\"/>\n"
	"<timeDelta value=\"480\"/>\n"
	"<unitScale bunitId=\"2\" texp=\"0\" ratio=\"1/2\"/>\n"
	"<note pitch=\"64\" duration=\"480\"/>\n"
	"<timeDelta value=\"480\"/>\n"
	"<absoluteUnitCoeff unitId=\"9\" ratioSI=\"1/1\"/>\n"
	"<note pitch=\"65\" duration=\"480\"/>\n"
	"<timeDelta value=\"480\" unit=\"ms\"/>\n"
	"<assign var=\"0x0002\"><expr><const value=\"0\"/></expr></assign>\n"
	"<while>\n"
	"<expr><var id=\"0x0002\"/><const value=\"3\"/><op type=\"lt\"/></expr>\n"
	"<do>\n"
	"<note pitch=\"70\" duration=\"10\" unit=\"ms\"/>\n"
	"<assign var=\"0x0002\"><expr><var id=\"0x0002\"/><const value=\"1\"/><op type=\"add\"/></expr></assign>\n"
	"<timeDelta value=\"10\" unit=\"ms\"/>\n"
	"</do>\n"
	"</while>\n"
	"<interpolate function=\"linear\" divisions=\"3\">\n"
	"<note pitch=\"60\" velocity=\"80\" duration=\"120\" />\n"
	"<note pitch=\"72\" velocity=\"100\" duration=\"120\" />\n"
	"</interpolate>\n"
	"<label id=\"7\" name=\"seven\"/>\n"
	"<assign var=\"0x0010\"><expr><var id=\"0x0010\"/><const value=\"1\"/><op type=\"add\"/></expr></assign>\n"
	"<if><expr><var id=\"0x0010\"/><const value=\"2\"/><op type=\"lt\"/></expr><then><jump target=\"seven\"/></then></if>\n"
	"</realData>\n"
	"</realtime>\n"
	"</tad>\n";

/* A figure moved by an interpolation that gives no time between its two places */
LOCAL CONST char prog_glide[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
	"<realtime autoplay=\"true\">\n"
	"<figure>\n"
	"<figView left=\"0\" top=\"0\" right=\"1280\" bottom=\"720\"/>\n"
	"<ellipse id=\"ball\" left=\"0\" top=\"100\" right=\"120\" bottom=\"220\"/>\n"
	"<rect id=\"card\" left=\"500\" top=\"100\" right=\"700\" bottom=\"300\"/>\n"
	"</figure>\n"
	"<realData>\n"
	"<realGroup type=\"parallel\">\n"
	"<timeDelta value=\"0\"/>\n"
	"<interpolate function=\"linear\" divisions=\"10\">\n"
	"<figureRef id=\"ball\" x=\"0\" y=\"100\"/>\n"
	"<figureRef id=\"ball\" x=\"400\" y=\"100\"/>\n"
	"</interpolate>\n"
	"</realGroup>\n"
	"<realGroup type=\"parallel\">\n"
	"<timeDelta value=\"1500\" unit=\"ms\"/>\n"
	"<interpolate function=\"linear\" divisions=\"30\">\n"
	"<shapeControl ref=\"card\" command=\"setAlpha\" value=\"0.0\"/>\n"
	"<shapeControl ref=\"card\" command=\"setAlpha\" value=\"1.0\"/>\n"
	"</interpolate>\n"
	"</realGroup>\n"
	"<realGroup type=\"parallel\">\n"
	"<timeDelta value=\"500\" unit=\"ms\"/>\n"
	"<interpolate function=\"spline\" divisions=\"30\">\n"
	"<shapeControl ref=\"title\" command=\"setPosition\" left=\"-100\" top=\"300\" right=\"0\" bottom=\"400\"/>\n"
	"<shapeControl ref=\"title\" command=\"setPosition\" left=\"50\" top=\"300\" right=\"150\" bottom=\"400\"/>\n"
	"</interpolate>\n"
	"</realGroup>\n"
	"</realData>\n"
	"</realtime>\n"
	"</tad>\n";

LOCAL CONST char prog_media[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
	"<realtime autoplay=\"true\">\n"
	"<figure>\n"
	"<figView left=\"0\" top=\"0\" right=\"1280\" bottom=\"720\"/>\n"
	"<rect id=\"box1\" left=\"0\" top=\"0\" right=\"100\" bottom=\"100\"/>\n"
	"</figure>\n"
	"<realData>\n"
	"<onerror type=\"notfound\">\n"
	"<assign var=\"0x0010\"><expr><const value=\"7\"/></expr></assign>\n"
	"</onerror>\n"
	"<trigger event=\"keydown\" key=\"Space\" id=\"sp\">\n"
	"<assign var=\"0x0001\"><expr><var id=\"0x0001\"/><const value=\"1\"/><op type=\"add\"/></expr></assign>\n"
	"<groupControl ref=\"anim\" command=\"pause\"/>\n"
	"</trigger>\n"
	"<trigger event=\"keydown\" key=\"ArrowRight\" once=\"true\">\n"
	"<groupControl ref=\"anim\" command=\"resume\"/>\n"
	"<triggerControl ref=\"sp\" command=\"disable\"/>\n"
	"</trigger>\n"
	"<realGroup id=\"anim\" type=\"parallel\" stream=\"1\">\n"
	"<keyframes ref=\"box1\" property=\"left\" duration=\"1000\" function=\"linear\">\n"
	"<keyframe offset=\"0%\" value=\"0\"/>\n"
	"<keyframe offset=\"50%\" value=\"200\"/>\n"
	"<keyframe offset=\"100%\" value=\"100\"/>\n"
	"</keyframes>\n"
	"<timeDelta value=\"1500\"/>\n"
	"<shapeControl ref=\"box1\" command=\"setAlpha\" value=\"0.5\" duration=\"500\"/>\n"
	"</realGroup>\n"
	"<audio id=\"bad\" href=\"missing.mp3\"/>\n"
	"<audio id=\"bad2\" href=\"missing2.mp3\"><fallback href=\"ok.mp3\" condition=\"error\"/></audio>\n"
	"<audio id=\"bad3\" href=\"missing3.mp3\"><onerror retry=\"1\" retryDelay=\"200\"><assign var=\"0x0011\"><expr><const value=\"9\"/></expr></assign></onerror></audio>\n"
	"<wait event=\"click\"/>\n"
	"<note pitch=\"C4\"/>\n"
	"<wait ref=\"bad2\" event=\"ended\"/>\n"
	"<note pitch=\"D4\"/>\n"
	"</realData>\n"
	"</realtime>\n"
	"</tad>\n";

LOCAL CONST char prog_random[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
	"<realtime autoplay=\"true\">\n"
	"<realData>\n"
	"<assign var=\"0x0001\"><expr><const value=\"0\"/></expr></assign>\n"
	"<while><expr><var id=\"0x0001\"/><const value=\"16\"/><op type=\"lt\"/></expr><do>\n"
	"<random exclude=\"last\">\n"
	"<note pitch=\"60\" duration=\"5\" unit=\"ms\"/>\n"
	"<note pitch=\"62\" duration=\"5\" unit=\"ms\"/>\n"
	"<note pitch=\"64\" duration=\"5\" unit=\"ms\"/>\n"
	"</random>\n"
	"<timeDelta value=\"10\" unit=\"ms\"/>\n"
	"<assign var=\"0x0001\"><expr><var id=\"0x0001\"/><const value=\"1\"/><op type=\"add\"/></expr></assign>\n"
	"</do></while>\n"
	"</realData>\n"
	"</realtime>\n"
	"</tad>\n";

LOCAL CONST char prog_expr[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
	"<realtime autoplay=\"true\">\n"
	"<realData>\n"
	"<assign var=\"0x0001\"><expr><const value=\"7\"/><const value=\"0\"/><op type=\"div\"/></expr></assign>\n"
	"<assign var=\"0x0002\"><expr><const value=\"2\"/><const value=\"9\"/><op type=\"sub\"/></expr></assign>\n"
	"<assign var=\"0x0010\"><expr><const value=\"5\"/><const value=\"7\"/><op type=\"lt\"/>"
	"<const value=\"2\"/><const value=\"2\"/><op type=\"eq\"/><op type=\"and\"/></expr></assign>\n"
	"<assign var=\"0x0011\"><expr><const value=\"2\"/><const value=\"10\"/><op type=\"swap\"/><op type=\"sub\"/>"
	"<op type=\"dup\"/><op type=\"mul\"/><const value=\"10\"/><op type=\"mod\"/></expr></assign>\n"
	"</realData>\n"
	"</realtime>\n"
	"</tad>\n";

LOCAL CONST char prog_slide[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
	"<document>\n"
	"<p>表紙</p>\n"
	"<realtime autoplay=\"true\">\n"
	"<realData>\n"
	"<note pitch=\"60\" duration=\"10\" unit=\"ms\"/>\n"
	"<timeDelta value=\"100\" unit=\"ms\"/>\n"
	"<jump target=\"next_slide\"/>\n"
	"</realData>\n"
	"</realtime>\n"
	"<p>終わり</p>\n"
	"</document>\n"
	"</tad>\n";

/* ---------------------------------------------------------------- the tests */

/* Numbers, names and the small conversions */
LOCAL void test_num( void )
{
	RT_V	v = 0;
	UB	b[8];
	RT_EASE	e;

	KT_ASSERT(rt_num((CONST UB *)"0.5", &v));
	KT_ASSERT_EQ(v, 500000);
	KT_ASSERT(rt_num((CONST UB *)"-12.25", &v));
	KT_ASSERT_EQ(v, -12250000);
	KT_ASSERT_EQ(rt_int((CONST UB *)"0x10", 0), 16);
	KT_ASSERT_EQ(rt_pitch((CONST UB *)"C4"), 60);
	KT_ASSERT_EQ(rt_pitch((CONST UB *)"F#3"), 54);
	KT_ASSERT_EQ(rt_pitch((CONST UB *)"Bb2"), 46);
	KT_ASSERT_EQ(rt_pitch((CONST UB *)"72"), 72);
	KT_ASSERT_EQ(rt_op_code((CONST UB *)"add"), 0x8000);
	KT_ASSERT_EQ(rt_op_code((CONST UB *)"pick"), 0x8052);
	KT_ASSERT(rt_op_name(0x8014) != NULL && s_eq((CONST UB *)rt_op_name(0x8014), "eq"));
	KT_ASSERT_EQ(rt_base64((CONST UB *)"TWFu", b, sizeof(b)), 3);
	KT_ASSERT(b[0] == 'M' && b[1] == 'a' && b[2] == 'n');
	KT_ASSERT(rt_ease_parse((CONST UB *)"linear", &e));
	KT_ASSERT_EQ(rt_ease(&e, 32768), 32768);
	KT_ASSERT(rt_ease_parse((CONST UB *)"ease-in-out", &e));
	KT_ASSERT_EQ(rt_ease(&e, 0), 0);
	KT_ASSERT_EQ(rt_ease(&e, 65536), 65536);
	KT_ASSERT(rt_ease(&e, 16384) < 16384);
	KT_ASSERT(rt_ease_parse((CONST UB *)"cubic-bezier(0.25, 0.1, 0.25, 1.0)", &e));
	KT_ASSERT_EQ(e.kind, RT_EASE_BEZIER);
}

/* A record read: what it declares */
LOCAL void test_parse( void )
{
	T_TAD	*doc;
	RT	*rt;
	CONST RT_STREAM *st;

	KT_ASSERT_ER(load(prog_media, &doc, &rt), E_OK);
	if ( rt == NULL ) return;
	KT_ASSERT_EQ(rt_nmedia(rt), 3);
	KT_ASSERT(rt_autoplay(rt));
	KT_ASSERT(rt_obj_find(rt, (CONST UB *)"box1") >= 0);
	KT_ASSERT(s_eq(rt_media(rt, 1)->href, "missing2.mp3"));
	unload(doc, rt);

	KT_ASSERT_ER(load(prog_flow, &doc, &rt), E_OK);
	if ( rt == NULL ) return;
	KT_ASSERT_EQ(rt_nstream(rt), 1);
	st = rt_stream_of(rt, 1);
	KT_ASSERT(st != NULL && s_eq(st->device, "inst:Piano"));
	KT_ASSERT(rt_sounds(rt));
	unload(doc, rt);

	/* a <realtime> inside a <document> is found */
	KT_ASSERT_ER(tad_parse((CONST UB *)prog_slide, s_len(prog_slide), NULL, &doc), E_OK);
	KT_ASSERT(rt_find(doc) != NULL);
	tad_free(doc);
}

/* Expressions: division by 0, negative, logic, the stack */
LOCAL void test_expr( void )
{
	INT	v[4];

	KT_ASSERT(run(prog_expr, 100 * MS, NULL, 0, 0, v) >= 0);
	KT_ASSERT_EQ(v[0], 0);		/* 7 / 0 */
	KT_ASSERT_EQ(v[1], -7);		/* 2 - 9 */
	KT_ASSERT_EQ(v[2], 1);		/* 5 < 7 and 2 == 2 */
	KT_ASSERT_EQ(v[3], 4);		/* (10 - 2)^2 mod 10 */
}

/* Branches, groups, time, loops, labels: when each note sounds */
LOCAL void test_flow( void )
{
	INT	v[4];

	KT_ASSERT(run(prog_flow, 10000 * MS, NULL, 0, 0, v) >= 0);
	KT_ASSERT_EQ(v[0], 12);
	KT_ASSERT_EQ(v[2], 2);			/* the jump back went once */
	NOTE_AT(0, 60, 0);			/* if: 12 > 10 */
	NOTE_AT(1, 65, 100 * MS);		/* the serial group: timeDelta */
	NOTE_AT(2, 67, 300 * MS);		/* timeAbsolute 300 */
	NOTE_AT(3, 69, 300 * MS);		/* timeAbsolute 50, earlier: not gone back */
	NOTE_AT(4, 71, 300 * MS);		/* after the group: its end */
	KT_ASSERT_EQ(note(1)->c, 1);		/* the group's stream */
	KT_ASSERT_EQ(note(4)->c, 0);
}

/* Units: a system of half seconds, ticks of 1/480, a temporary unit, a scale */
LOCAL void test_units( void )
{
	KT_ASSERT(run(prog_flow, 10000 * MS, NULL, 0, 0, NULL) >= 0);
	NOTE_AT(5, 60, 800 * MS);		/* 480 ticks of 1/480 of 1/2 s after 300 ms */
	KT_ASSERT_EQ(note(5)->dur, 500 * MS);
	NOTE_AT(6, 62, 1050 * MS);		/* one unit "8" (1/4 s), then ticks again */
	NOTE_AT(7, 64, 1550 * MS);
	KT_ASSERT_EQ(note(7)->dur, 250 * MS);	/* the scale halves the time */
	NOTE_AT(8, 65, 1800 * MS);		/* absoluteUnitCoeff: 1/1 s again */
	NOTE_AT(9, 70, 2280 * MS);		/* the while: three times, 10 ms apart */
	NOTE_AT(10, 70, 2290 * MS);
	NOTE_AT(11, 70, 2300 * MS);
}

/* Interpolation of notes: divisions + 1 steps, pitch and velocity between the two */
LOCAL void test_interp( void )
{
	static CONST INT pitch[5] = { 60, 63, 66, 69, 72 }, vel[5] = { 80, 85, 90, 95, 100 };
	INT	i;

	KT_ASSERT(run(prog_flow, 10000 * MS, NULL, 0, 0, NULL) >= 0);
	KT_ASSERT_EQ(notes(), 17);
	for ( i = 0; i < 5; i++ ) {
		CONST EV *e = note(12 + i);

		KT_ASSERT(e != NULL);
		if ( e == NULL ) return;
		KT_ASSERT_EQ(e->a, pitch[i]);
		KT_ASSERT_EQ(e->b, vel[i]);
		KT_ASSERT_EQ(e->at, ( 2310 + 125 * i ) * MS);
	}
}

/*
 * A figure's interpolation with no time between its places goes from one
 * to the next in a second, in its divisions, rather than in a frame
 */
LOCAL void test_glide( void )
{
	T_TAD	*doc;
	RT	*rt;
	RT_OBJ	*o;
	INT	x;

	reset();
	KT_ASSERT_ER(load(prog_glide, &doc, &rt), E_OK);
	if ( rt == NULL ) return;
	rt_start(rt, 0);
	run_to(rt, 50 * MS, NULL, 0);
	o = rt_obj(rt, rt_obj_find(rt, (CONST UB *)"ball"));
	KT_ASSERT(o != NULL);
	if ( o == NULL ) {
		unload(doc, rt);
		return;
	}
	KT_ASSERT_EQ(o->p[RT_P_LEFT], 0);
	run_to(rt, 500 * MS, NULL, 0);
	x = o->p[RT_P_LEFT] / RT_ONE;
	tm_printf((UB *)"  glide: at 0.5 s the ball is at %d of 0 to 400\n", x);
	KT_ASSERT(x > 100 && x < 300);
	run_to(rt, 1100 * MS, NULL, 0);
	KT_ASSERT_EQ(o->p[RT_P_LEFT], 400 * RT_ONE);

	/* what fades in later is not seen before its fade */
	o = rt_obj(rt, rt_obj_find(rt, (CONST UB *)"card"));
	KT_ASSERT(o != NULL);
	if ( o != NULL ) {
		tm_printf((UB *)"  glide: the card's opacity at 1.1 s %d of %d\n", o->p[RT_P_ALPHA], RT_ONE);
		KT_ASSERT_EQ(o->p[RT_P_ALPHA], 0);
		run_to(rt, 2000 * MS, NULL, 0);
		KT_ASSERT(o->p[RT_P_ALPHA] > 0 && o->p[RT_P_ALPHA] < RT_ONE);
		run_to(rt, 2600 * MS, NULL, 0);
		KT_ASSERT_EQ(o->p[RT_P_ALPHA], RT_ONE);
	}

	/* a box slid in by setPosition from off the scene */
	o = rt_obj(rt, rt_obj_find(rt, (CONST UB *)"title"));
	KT_ASSERT(o != NULL);
	if ( o != NULL ) {
		run_to(rt, 3500 * MS, NULL, 0);
		tm_printf((UB *)"  glide: the slid box at 3.5 s: %d %d %d %d, shown %d\n", o->p[RT_P_LEFT] / RT_ONE,
			  o->p[RT_P_TOP] / RT_ONE, o->p[RT_P_RIGHT] / RT_ONE, o->p[RT_P_BOTTOM] / RT_ONE, o->shown);
		KT_ASSERT_EQ(o->p[RT_P_LEFT], 50 * RT_ONE);
		KT_ASSERT_EQ(o->p[RT_P_RIGHT], 150 * RT_ONE);
	}
	unload(doc, rt);
}

/* <random>: the same seed the same choices, never one twice running with exclude="last" */
LOCAL void test_random( void )
{
	INT	a[16], i, n, diff = 0;

	KT_ASSERT(run(prog_random, 1000 * MS, NULL, 0, 12345, NULL) >= 0);
	n = notes();
	KT_ASSERT_EQ(n, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		a[i] = note(i)->a;
		KT_ASSERT(a[i] == 60 || a[i] == 62 || a[i] == 64);
		KT_ASSERT_EQ(note(i)->at, (RT_TIME)i * 10 * MS);
		if ( i > 0 ) KT_ASSERT(a[i] != a[i - 1]);
	}
	KT_ASSERT(run(prog_random, 1000 * MS, NULL, 0, 12345, NULL) >= 0);
	for ( i = 0; i < n && i < 16; i++ ) KT_ASSERT_EQ(note(i)->a, a[i]);
	KT_ASSERT(run(prog_random, 1000 * MS, NULL, 0, 777, NULL) >= 0);
	for ( i = 0; i < n && i < 16 && note(i) != NULL; i++ ) diff += ( note(i)->a != a[i] );
	KT_ASSERT(diff > 0);
}

/* Media, errors, triggers, waiting, keyframes, a group paused and resumed */
LOCAL void test_media( void )
{
	static CONST INPUT in[3] = {
		{ 200 * MS, RT_IN_KEYDOWN, "Space" },
		{ 900 * MS, RT_IN_KEYDOWN, "ArrowRight" },
		{ 2500 * MS, RT_IN_CLICK, NULL }
	};
	T_TAD	*doc;
	RT	*rt;
	RT_OBJ	*o;
	INT	v[4], i, plays = 0;

	/* keyframes: left goes 0 -> 200 in the first half, back to 100 in the second */
	reset();
	KT_ASSERT_ER(load(prog_media, &doc, &rt), E_OK);
	if ( rt == NULL ) return;
	rt_start(rt, 0);
	run_to(rt, 250 * MS, NULL, 0);
	o = rt_obj(rt, rt_obj_find(rt, (CONST UB *)"box1"));
	KT_ASSERT_EQ(o->p[RT_P_LEFT], 100 * RT_ONE);
	KT_ASSERT_EQ(o->p[RT_P_RIGHT], 200 * RT_ONE);
	/* while anything moves the engine asks for the next frame */
	KT_ASSERT(rt_step(rt, clk) <= clk + RT_FRAME_US);
	run_to(rt, 1000 * MS, NULL, 0);
	KT_ASSERT_EQ(o->p[RT_P_LEFT], 100 * RT_ONE);
	/* then nothing until the setAlpha at 1.5 s */
	KT_ASSERT_EQ(rt_step(rt, clk), 1500 * MS);
	unload(doc, rt);

	KT_ASSERT(run(prog_media, 6000 * MS, in, 3, 0, v) >= 0);
	KT_ASSERT_EQ(v[0], 1);			/* Space once; the trigger was disabled after */
	KT_ASSERT_EQ(v[2], 7);			/* the record's onerror: notfound */
	KT_ASSERT_EQ(v[3], 9);			/* the medium's own onerror, after its retry */
	for ( i = 0; i < nev; i++ ) {
		if ( ev[i].kind == 'm' && ev[i].a == 1 && ev[i].b == RT_MO_PLAY ) plays++;
	}
	KT_ASSERT_EQ(plays, 1);			/* the fallback played */
	NOTE_AT(0, 60, 2500 * MS);		/* after the click */
	NOTE_AT(1, 62, 3010 * MS);		/* after the fallback ended: 10 ms + 3 s */
}

/* Paused, resumed, sought: program time against the clock */
LOCAL void test_clock( void )
{
	T_TAD	*doc;
	RT	*rt;

	reset();
	KT_ASSERT_ER(load(prog_flow, &doc, &rt), E_OK);
	if ( rt == NULL ) return;
	rt_start(rt, 0);
	run_to(rt, 500 * MS, NULL, 0);
	KT_ASSERT_EQ(notes(), 5);
	rt_pause(rt, clk);
	KT_ASSERT(rt_paused(rt));
	clk += 1000 * MS;
	rt_resume(rt, clk);
	run_to(rt, 2000 * MS, NULL, 0);
	NOTE_AT(5, 60, 1800 * MS);		/* 800 ms of program time, a second later */
	unload(doc, rt);

	reset();
	KT_ASSERT_ER(load(prog_flow, &doc, &rt), E_OK);
	if ( rt == NULL ) return;
	rt_start(rt, 0);
	run_to(rt, 10 * MS, NULL, 0);
	nev = 0;
	rt_seek(rt, 2000 * MS, clk);
	run_to(rt, 1000 * MS, NULL, 0);
	/* what lay before 2 s was not sounded; the loop's notes came 280 ms on */
	KT_ASSERT(note(0) != NULL && note(0)->a == 70);
	KT_ASSERT_EQ(note(0)->at, 11 * MS + 280 * MS);
	unload(doc, rt);
}

/* A slide that is a program: shown as slides, and its jump to next_slide asks for the next */
LOCAL void test_slide( void )
{
	T_TAD	*doc;
	INT	i, w = 0, h = 0, acts = 0;

	KT_ASSERT_ER(tad_parse((CONST UB *)prog_slide, s_len(prog_slide), NULL, &doc), E_OK);
	KT_ASSERT(sl_takes(doc));
	tad_free(doc);
	KT_ASSERT_ER(tad_parse((CONST UB *)prog_media, s_len(prog_media), NULL, &doc), E_OK);
	sl_scene_size(doc, &w, &h);
	KT_ASSERT_EQ(w, 1280);
	KT_ASSERT_EQ(h, 720);
	tad_free(doc);

	KT_ASSERT(run(prog_slide, 1000 * MS, NULL, 0, 0, NULL) >= 0);
	for ( i = 0; i < nev; i++ ) {
		if ( ev[i].kind == 'a' && ev[i].a == RT_ACT_NEXT ) {
			acts++;
			KT_ASSERT_EQ(ev[i].at, 100 * MS);
		}
	}
	KT_ASSERT_EQ(acts, 1);
}

/* ---------------------------------------------------------------- binary TAD */

LOCAL const char *bin_target( void *c, INT n )
{
	return "01234567-89ab-7cde-8f01-23456789abcd";
}

LOCAL void bin_vobjid( void *c, char *o )
{
	knl_memcpy(o, "01234567-89ab-7cde-8f01-000000000000", 37);
}

LOCAL ER bin_picture( void *c, INT r, INT n, const UB *p, INT l )
{
	return E_OK;
}

/* xmlTAD to binary into a new buffer: its length, or an error */
LOCAL INT to_bin( BK_TADW *w, CONST char *xml, INT n, UB **p_out )
{
	INT	len = 0;
	ER	er;

	*p_out = NULL;
	er = bk_tad_from_xml(w, (CONST UB *)xml, n, NULL, 0, &len, NULL, NULL);
	if ( er < E_OK ) return er;
	*p_out = (UB *)Kmalloc((SZ)len + 16);
	if ( *p_out == NULL ) return E_NOMEM;
	er = bk_tad_from_xml(w, (CONST UB *)xml, n, *p_out, len + 16, &len, NULL, NULL);
	return ( er < E_OK ) ? er : len;
}

LOCAL ER to_xml( CONST UB *bin, INT len, BPKBUF *o )
{
	static CONST BPKHOOK hook = { bin_target, bin_vobjid, bin_picture, NULL };
	BPKCONV	*cv = (BPKCONV *)Kmalloc(sizeof(BPKCONV));
	ER	er;

	if ( cv == NULL ) return E_NOMEM;
	bpk_conv_init(cv, &hook);
	bpk_buf_init(o);
	er = bpk_tad_to_xml(cv, "t", "01234567-89ab-7cde-8f01-23456789abcd", 0, bin, (UINT)len, o);
	KT_ASSERT_EQ(cv->nskipped, 0);
	Kfree(cv);
	return er;
}

/* The events of two runs the same */
LOCAL BOOL same_run( CONST EV *a, INT na )
{
	INT	i;

	if ( na != nev ) return FALSE;
	for ( i = 0; i < na; i++ ) {
		if ( a[i].kind != ev[i].kind || a[i].at != ev[i].at || a[i].a != ev[i].a || a[i].b != ev[i].b ) return FALSE;
	}
	return TRUE;
}

LOCAL void round_trip( BK_TADW *w, CONST char *xml, CONST INPUT *in, INT nin, RT_TIME until )
{
	UB	*b1 = NULL, *b2 = NULL;
	INT	l1, l2, ns = 0, nc = 0, na, i;
	BPKBUF	x1, x2;
	EV	*keep = (EV *)Kmalloc(sizeof(EV) * NEV);
	BOOL	same;

	if ( keep == NULL ) return;
	bpk_buf_init(&x1);
	bpk_buf_init(&x2);
	l1 = to_bin(w, xml, s_len(xml), &b1);
	KT_ASSERT(l1 > 0);
	if ( l1 <= 0 ) goto out;
	KT_ASSERT_ER(bk_tad_check(b1, l1, &ns, &nc), E_OK);
	KT_ASSERT(ns > 4);
	KT_ASSERT_ER(to_xml(b1, l1, &x1), E_OK);
	if ( x1.s == NULL ) goto out;
	l2 = to_bin(w, x1.s, x1.n, &b2);
	KT_ASSERT_EQ(l2, l1);
	if ( l2 == l1 ) {
		for ( same = TRUE, i = 0; i < l1; i++ ) same = (BOOL)( same && b1[i] == b2[i] );
		KT_ASSERT(same);
	}
	KT_ASSERT_ER(to_xml(b2, l2, &x2), E_OK);
	KT_ASSERT(x2.s != NULL && x1.n == x2.n);

	/* the record from binary runs as the one it came from */
	KT_ASSERT(run(xml, until, in, nin, 4242, NULL) > 0);
	na = nev;
	for ( i = 0; i < na; i++ ) keep[i] = ev[i];
	KT_ASSERT(run(x1.s, until, in, nin, 4242, NULL) > 0);
	KT_ASSERT(same_run(keep, na));
out:
	bpk_buf_free(&x1);
	bpk_buf_free(&x2);
	if ( b1 != NULL ) Kfree(b1);
	if ( b2 != NULL ) Kfree(b2);
	Kfree(keep);
}

/* Every element of the records to its segment and back */
LOCAL void test_binary( void )
{
	static CONST INPUT in[3] = {
		{ 200 * MS, RT_IN_KEYDOWN, "Space" },
		{ 900 * MS, RT_IN_KEYDOWN, "ArrowRight" },
		{ 2500 * MS, RT_IN_CLICK, NULL }
	};
	BK_TADW	*w = (BK_TADW *)Kmalloc(sizeof(BK_TADW));
	UB	*b = NULL;
	INT	l;
	BPKBUF	x;

	if ( w == NULL ) return;
	KT_ASSERT(bpk_rt_by_name("realGroup", 9, TRUE)->sub == 2);
	KT_ASSERT(bpk_rt_by_name("realGroup", 9, FALSE)->sub == 0);
	KT_ASSERT(bpk_rt_by_name("then", 4, FALSE) == NULL);
	KT_ASSERT(bpk_rt_is_end(TS_BRLP, 6));
	KT_ASSERT(!bpk_rt_seg(TS_RMEDIA, FALSE));

	round_trip(w, prog_flow, NULL, 0, 10000 * MS);
	round_trip(w, prog_media, in, 3, 6000 * MS);
	round_trip(w, prog_random, NULL, 0, 1000 * MS);
	round_trip(w, prog_slide, NULL, 0, 1000 * MS);

	/* the first segment after TS_INFO of a 実時間TAD: <realtime>, TS_REAL sub 2, elements inside */
	l = to_bin(w, prog_random, s_len(prog_random), &b);
	KT_ASSERT(l > 20);
	if ( l > 20 ) {
		KT_ASSERT_EQ(b[10] | ( b[11] << 8 ), TS_REAL);
		KT_ASSERT_EQ(b[14] | ( b[15] << 8 ), ( 2 << 8 ) | BPK_RT_KIDS);
	}
	if ( b != NULL ) Kfree(b);

	/* a shape's id is kept for the program to name it */
	l = to_bin(w, prog_media, s_len(prog_media), &b);
	if ( l > 0 && to_xml(b, l, &x) >= E_OK && x.s != NULL ) {
		KT_ASSERT(has_sub((CONST UB *)x.s, "<rect id=\"box1\""));
		KT_ASSERT(has_sub((CONST UB *)x.s, "<keyframe offset=\"50%\" value=\"200\"/>"));
	} else {
		KT_ASSERT(FALSE);
	}
	bpk_buf_free(&x);
	if ( b != NULL ) Kfree(b);
	Kfree(w);
}

EXPORT void ktest_rt( void )
{
	ev = (EV *)Kmalloc(sizeof(EV) * NEV);
	if ( ev == NULL ) return;
	KT_RUN(test_num);
	KT_RUN(test_parse);
	KT_RUN(test_expr);
	KT_RUN(test_flow);
	KT_RUN(test_units);
	KT_RUN(test_interp);
	KT_RUN(test_glide);
	KT_RUN(test_random);
	KT_RUN(test_media);
	KT_RUN(test_clock);
	KT_RUN(test_slide);
	KT_RUN(test_binary);
	Kfree(ev);
	ev = NULL;
}
