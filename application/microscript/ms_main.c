/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_main.c
 *	マイクロスクリプト: the program (design 17.15)
 *
 *	It is started with the UUID of a figure as its argument, from the
 *	figure's execute menu. It reads the figure (the stage and its
 *	segments) and the text objects linked into it whose names begin with
 *	SCRIPT (the script, joined in the order of the links), makes a window
 *	with the figure's name, and runs the script: PROLOGUE first, then the
 *	procedures what happens in the window starts.
 *
 *	The loop takes what happened to the window, gives each thread that
 *	may run its turn, and draws the stage when something changed. It
 *	sleeps only when no thread wants to run before the next thing it
 *	waits for. Below the stage is a strip where MESG writes.
 */

#include "ms.h"
#include "ms_run.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ts/sysdef.h>
#include <ts/json.h>

IMPORT ER ts_get_mono( UD *p_ns );

MSWIN	ms_win;
MSFIG	ms_fig;
MSPROG	ms_prog;

/* A guess at the frame, for the first size; the work area is set exactly after */
#define TITLE_H		34
#define FRAME_W		16
#define TICK_MS		10
#define DCLICK_MS	300

/* HID usages of the keys that are not letters */
#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_TAB		0x2B
#define K_SPACE		0x2C
#define K_DEL		0x4C
#define K_RIGHT		0x4F
#define K_LEFT		0x50
#define K_DOWN		0x51
#define K_UP		0x52
#define M_CTRL		0x11
#define M_SHIFT		0x22
#define M_ALT		0x44

static TS_UUID	win, ch;
static ID	kw, port;
static INT	gid = -1;
static BOOL	dirty = TRUE, closing;
static char	*source;
static INT	sourcelen;

/* ---------------------------------------------------------------- time */

UD ms_now_ms( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000;
}

void ms_dirty( void )
{
	dirty = TRUE;
}

/* ---------------------------------------------------------------- messages */

/*
 * MESG and what the program has to say go to the system's message line,
 * the band at the foot of the screen, a line at a time: the last line
 * finished, or the one being written. They go to the console as well,
 * where every line of them stays.
 */
static MSBUF	msg_cur;

/*
 * The console (CONSOLE 1): the messages written over the whole window,
 * the newest at the foot, as many lines as the window holds. Every line
 * said is kept here, the last CON_LINES of them, whether the console is
 * shown or not.
 */
#define CON_LINES	500
#define CON_PX		14
#define CON_LH		18
#define CON_INK		0x00000000U
#define CON_GROUND	0x00FFFFFFU

static char	*con_line[CON_LINES];
static INT	con_first, con_count;
static MSBUF	con_cur;			/* the line being written */
static BOOL	console_on;

static void con_push( const char *s, INT n )
{
	INT	at = ( con_first + con_count ) % CON_LINES;
	char	*l = ms_alloc((size_t)n + 1);

	memcpy(l, s, (size_t)n);
	l[n] = 0;
	if ( con_count == CON_LINES ) {
		ms_free(con_line[con_first]);
		con_line[con_first] = l;
		con_first = ( con_first + 1 ) % CON_LINES;
	} else {
		con_line[at] = l;
		con_count++;
	}
}

static void con_put( const char *utf8 )
{
	INT	i;

	for ( i = 0; utf8[i] != 0; i++ ) {
		if ( utf8[i] == '\n' ) {
			con_push(con_cur.s != NULL ? con_cur.s : "", con_cur.n);
			mb_free(&con_cur);
			mb_init(&con_cur);
		} else if ( utf8[i] != '\r' ) {
			mb_putc(&con_cur, utf8[i]);
		}
	}
	if ( console_on ) ms_dirty();
}

void ms_console( BOOL on )
{
	console_on = on;
	ms_dirty();
}

/* A line cut into what fits in w dots, each piece given to put */
static INT con_rows( const char *s, INT w, INT gid, INT x, INT y, BOOL draw )
{
	INT	n = (INT)strlen(s), i = 0, rows = 0;
	char	buf[1024];

	if ( n == 0 ) return 1;
	while ( i < n ) {
		INT	k = i, last = i;
		UW	cp;

		/* as many characters as fit */
		while ( k < n ) {
			INT	m = ms_utf8_dec((const UB *)s + k, n - k, &cp);

			if ( m <= 0 ) { k = n; break; }
			if ( k + m - i >= (INT)sizeof(buf) ) break;
			memcpy(buf, s + i, (size_t)( k + m - i ));
			buf[k + m - i] = 0;
			if ( dp_text_width((const UB *)buf, CON_PX) > w && k > i ) break;
			k += m;
			last = k;
		}
		if ( last == i ) last = n;
		if ( draw ) {
			memcpy(buf, s + i, (size_t)( last - i ));
			buf[last - i] = 0;
			(void)dp_text(gid, x, y + rows * CON_LH + CON_PX, (const UB *)buf, CON_INK, CON_PX);
		}
		rows++;
		i = last;
	}
	return rows;
}

static void con_draw( INT gid, INT w, INT h )
{
	T_DPRECT	r;
	INT		rows = 0, i, y;
	INT		fit = ( h - 8 ) / CON_LH;
	INT		tw = w - 12;

	r.left = 0;
	r.top = 0;
	r.right = w;
	r.bottom = h;
	(void)dp_fill_rect(gid, &r, CON_GROUND);
	/* from the newest back, until the window is full */
	for ( i = con_count - 1 + ( con_cur.n > 0 ? 1 : 0 ); i >= 0 && rows < fit; i-- ) {
		const char *l = ( i == con_count ) ? con_cur.s : con_line[( con_first + i ) % CON_LINES];

		rows += con_rows(l != NULL ? l : "", tw, gid, 0, 0, FALSE);
	}
	y = 4 - ( ( rows > fit ) ? ( rows - fit ) * CON_LH : 0 );
	for ( i = i + 1; i < con_count + ( con_cur.n > 0 ? 1 : 0 ); i++ ) {
		const char *l = ( i == con_count ) ? con_cur.s : con_line[( con_first + i ) % CON_LINES];

		y += con_rows(l != NULL ? l : "", tw, gid, 6, y, TRUE) * CON_LH;
	}
}

void ms_message( const char *utf8 )
{
	INT	i;

	tm_putstring((const UB *)utf8);
	con_put(utf8);
	for ( i = 0; utf8[i] != 0; i++ ) {
		if ( utf8[i] == '\n' ) {
			if ( msg_cur.n > 0 ) {
				mb_putc(&msg_cur, 0);
				(void)wm_msg_put(msg_cur.s);
			}
			mb_free(&msg_cur);
			mb_init(&msg_cur);
			continue;
		}
		mb_putc(&msg_cur, utf8[i]);
	}
	if ( msg_cur.n > 0 ) {
		mb_putc(&msg_cur, 0);
		(void)wm_msg_put(msg_cur.s);
		msg_cur.n--;			/* the 0 put for the call is not part of the line */
	}
}

void ms_message_clear( void )
{
	INT	i;

	mb_free(&msg_cur);
	mb_init(&msg_cur);
	(void)wm_msg_put(NULL);
	for ( i = 0; i < con_count; i++ ) ms_free(con_line[( con_first + i ) % CON_LINES]);
	con_first = con_count = 0;
	mb_free(&con_cur);
	mb_init(&con_cur);
	if ( console_on ) ms_dirty();
}

/*
 * Asking whether a host may be reached: a box over the window with the
 * host and two buttons, answered with them or with Y and N (Enter and
 * Esc). The thread that asks waits for the answer; the window goes on
 * reading what the person does.
 */
#define ASK_W		360
#define ASK_H		120
#define ASK_BW		96
#define ASK_BH		26

static struct {
	BOOL	pending;
	INT	answer;			/* 1 yes, 0 no, -1 not yet */
	char	host[256];
	T_DPRECT box, yes, no;
} ask;

static void ask_layout( INT w, INT h )
{
	INT	l = ( w - ASK_W ) / 2, t = ( h - ASK_H ) / 2;

	if ( l < 0 ) l = 0;
	if ( t < 0 ) t = 0;
	ask.box.left = l;
	ask.box.top = t;
	ask.box.right = l + ASK_W;
	ask.box.bottom = t + ASK_H;
	ask.yes.left = l + ASK_W / 2 - ASK_BW - 10;
	ask.yes.top = t + ASK_H - ASK_BH - 14;
	ask.yes.right = ask.yes.left + ASK_BW;
	ask.yes.bottom = ask.yes.top + ASK_BH;
	ask.no.left = l + ASK_W / 2 + 10;
	ask.no.top = ask.yes.top;
	ask.no.right = ask.no.left + ASK_BW;
	ask.no.bottom = ask.yes.bottom;
}

static void ask_button( INT gid, const T_DPRECT *b, const char *label )
{
	T_DPRECT	r = *b;
	INT		tw = dp_text_width((const UB *)label, CON_PX);

	(void)dp_fill_rect(gid, &r, 0x00000000U);
	r.left++; r.top++; r.right--; r.bottom--;
	(void)dp_fill_rect(gid, &r, 0x00E0E0E0U);
	(void)dp_text(gid, b->left + ( ( b->right - b->left ) - tw ) / 2, b->top + ( ASK_BH + CON_PX ) / 2 - 2,
		      (const UB *)label, CON_INK, CON_PX);
}

static void ask_draw( INT gid, INT w, INT h )
{
	T_DPRECT	r;
	char		line[320];

	ask_layout(w, h);
	r = ask.box;
	(void)dp_fill_rect(gid, &r, 0x00000000U);
	r.left += 2; r.top += 2; r.right -= 2; r.bottom -= 2;
	(void)dp_fill_rect(gid, &r, 0x00FFFFF0U);
	(void)dp_text(gid, ask.box.left + 14, ask.box.top + 12 + CON_PX, (const UB *)"接続の許可", CON_INK, CON_PX);
	snprintf(line, sizeof(line), "%s に接続します。よろしいですか。", ask.host);
	(void)dp_text(gid, ask.box.left + 14, ask.box.top + 38 + CON_PX, (const UB *)line, CON_INK, CON_PX);
	ask_button(gid, &ask.yes, "許可(Y)");
	ask_button(gid, &ask.no, "拒否(N)");
}

static BOOL in_rect( const T_DPRECT *r, INT x, INT y )
{
	return (BOOL)( x >= r->left && x < r->right && y >= r->top && y < r->bottom );
}

INT ms_net_ask( const char *host )
{
	INT	a;

	if ( ask.pending ) return 0;		/* one question at a time: the other thread is refused */
	snprintf(ask.host, sizeof(ask.host), "%s", host);
	ask.answer = -1;
	ask.pending = TRUE;
	ms_dirty();
	while ( ask.answer < 0 ) {
		if ( rt.cur != NULL && th_stop(rt.cur) ) break;
		ms_yield(ms_now_ms() + 50);
	}
	a = ask.answer;
	ask.pending = FALSE;
	ms_dirty();
	return a;
}

/* ---------------------------------------------------------------- the window */

static void work_size( INT *p_w, INT *p_h )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	*p_w = 640;
	*p_h = 480;
	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && asz >= (SZ)sizeof(wp) ) {
		*p_w = wp.wright - wp.wleft;
		*p_h = wp.wbottom - wp.wtop;
		ms_win.wdx = wp.wleft;
		ms_win.wdy = wp.wtop;
	}
	if ( *p_w < 40 ) *p_w = 40;
	if ( *p_h < 40 ) *p_h = 40;
}

static void draw_all( void )
{
	INT	w, h;

	if ( gid < 0 ) return;
	work_size(&w, &h);
	ms_win.wdw = w;
	ms_win.wdh = h;
	if ( console_on ) con_draw(gid, w, h);
	else st_render(gid, w, h, NULL);
	if ( ask.pending ) ask_draw(gid, w, h);
	(void)wm_obj_flush(kw, NULL);
	dirty = FALSE;
}

void ms_render_now( void )
{
	draw_all();
}

void ms_win_resize( INT w, INT h )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	/* the frame stays as wide as it is: the work area becomes w by h */
	wp.right = wp.left + w + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
	wp.bottom = wp.top + h + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
	(void)ob_wri_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	dirty = TRUE;
}

void ms_win_move( INT x, INT y )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		w, h;

	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	w = wp.right - wp.left;
	h = wp.bottom - wp.top;
	/* (x, y) is where the work area goes */
	x -= wp.wleft - wp.left;
	y -= wp.wtop - wp.top;
	wp.left = x;
	wp.top = y;
	wp.right = x + w;
	wp.bottom = y + h;
	(void)ob_wri_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	dirty = TRUE;
}

/*
 * An effect, drawn frame by frame in the thread: `show` coming in and
 * `hide` going out, over the whole window (SCENE) or the rectangle round
 * them. Where the effect has not reached yet the window still shows
 * what it showed before.
 */
#define FX_RECTS	64

static void set_shown( MSSEG **v, INT n, BOOL on )
{
	INT	i;

	for ( i = 0; i < n; i++ ) v[i]->visible = on;
}

void ms_transition( MSSEG **show, INT nshow, MSSEG **hide, INT nhide, MSSTR *effect, INT steps, BOOL whole )
{
	T_DPRECT	box, rects[FX_RECTS];
	INT		strip = ( strncmp(effect->s, "STRP", 4) == 0 );
	INT		frames = strip ? 12 : ( steps < 1 ) ? 1 : ( steps > 60 ) ? 60 : steps;
	INT		f, i, w, h, nr;

	work_size(&w, &h);
	box.left = 0;
	box.top = 0;
	box.right = w;
	box.bottom = h;
	if ( !whole ) {
		/* the rectangle round what comes and goes, in the window */
		BOOL	any = FALSE;

		for ( i = 0; i < nshow + nhide; i++ ) {
			MSSEG	*g = ( i < nshow ) ? show[i] : hide[i - nshow];
			INT	l = (INT)( g->x - st_vx() ), t = (INT)( g->y - st_vy() );

			if ( !any || l < box.left ) box.left = l;
			if ( !any || t < box.top ) box.top = t;
			if ( !any || l + (INT)g->w > box.right ) box.right = l + (INT)g->w;
			if ( !any || t + (INT)g->h > box.bottom ) box.bottom = t + (INT)g->h;
			any = TRUE;
		}
	}
	for ( f = 0; f <= frames && gid >= 0; f++ ) {
		double	ratio = (double)f / frames;

		set_shown(show, nshow, TRUE);
		set_shown(hide, nhide, FALSE);
		st_render(gid, w, h, &box);
		nr = st_effect_rects(effect->s, ratio, &box, steps, rects, FX_RECTS);
		set_shown(show, nshow, FALSE);
		set_shown(hide, nhide, TRUE);
		for ( i = 0; i < nr; i++ ) st_render(gid, w, h, &rects[i]);
		(void)wm_obj_flush(kw, NULL);
		ms_yield(ms_now_ms() + 16);
		if ( rt.cur != NULL && th_stop(rt.cur) ) break;
	}
	set_shown(show, nshow, TRUE);
	set_shown(hide, nhide, FALSE);
	dirty = TRUE;
}

/* ---------------------------------------------------------------- sound */

/*
 * A tone on the sound device: freq Hz for dur ms. 0 stops what sounds;
 * a negative number is a short tune of that number, played dur times.
 */
void ms_beep( double freq, double dur )
{
	static const UH	tunes[5][5] = {
		{ 523, 659, 784, 0, 0 }, { 784, 659, 523, 0, 0 }, { 523, 784, 523, 784, 0 },
		{ 659, 659, 659, 0, 0 }, { 523, 587, 659, 698, 784 }
	};
	TS_UUID	dev;
	INT	cnt = 0, rate = 48000, n, i, k, rep;
	ID	kd;
	H	*pcm;
	SZ	asz;
	UH	notes[80];
	INT	nn = 0, ndur;

	if ( freq == 0 ) return;
	if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, &dev, 1, &cnt) < E_OK || cnt < 1 ) return;
	kd = ob_opn_obj(&dev, OB_OP_WRITE);
	if ( kd <= 0 ) return;
	if ( freq < 0 ) {
		INT	t = ( (INT)( -freq ) - 1 ) % 5;

		rep = ( dur < 1 ) ? 1 : ( dur > 16 ) ? 16 : (INT)dur;
		for ( k = 0; k < rep; k++ ) {
			for ( i = 0; i < 5 && tunes[t][i] != 0 && nn < 80; i++ ) notes[nn++] = tunes[t][i];
		}
		ndur = 120;
	} else {
		notes[nn++] = (UH)( ( freq > 20000 ) ? 20000 : freq );
		ndur = ( dur < 1 ) ? 1 : ( dur > 5000 ) ? 5000 : (INT)dur;
	}
	n = rate * ndur / 1000;
	pcm = ms_alloc(sizeof(H) * 2 * (size_t)n);
	for ( k = 0; k < nn; k++ ) {
		/* a square wave, quiet */
		INT	half = rate / ( 2 * ( notes[k] > 0 ? notes[k] : 1 ) );

		for ( i = 0; i < n; i++ ) {
			H	v = ( ( i / ( half > 0 ? half : 1 ) ) & 1 ) ? 3000 : -3000;

			pcm[2 * i] = v;
			pcm[2 * i + 1] = v;
		}
		(void)ob_wri_rec(kd, OB_SND_PCM, 0, pcm, sizeof(H) * 2 * (size_t)n, &asz);
	}
	ms_free(pcm);
	ob_cls_obj(kd);
}

/* ---------------------------------------------------------------- the figure's metadata */

static char	meta[OB_ATR_MAX + 1];
static SZ	metalen;

static void meta_read( void )
{
	SZ	asz = 0;

	metalen = 0;
	if ( ob_get_atr(ms_win.kfig, (UB *)meta, OB_ATR_MAX, &asz) >= E_OK ) {
		metalen = asz;
		meta[metalen] = 0;
	}
}

static UW parse_colour( const char *s, UW dflt )
{
	UW	v = 0;
	INT	i;

	if ( s == NULL || s[0] != '#' ) return dflt;
	for ( i = 1; i <= 6; i++ ) {
		char	c = s[i];

		if ( c >= '0' && c <= '9' ) v = v * 16 + (UW)( c - '0' );
		else if ( c >= 'a' && c <= 'f' ) v = v * 16 + (UW)( c - 'a' + 10 );
		else if ( c >= 'A' && c <= 'F' ) v = v * 16 + (UW)( c - 'A' + 10 );
		else return dflt;
	}
	return v;
}

/* $SV: kept in the figure's metadata as "microscriptSV", 50 numbers */
void ms_sv_load( double *sv, BOOL *p_had )
{
	T_JSON	root, arr, it;
	INT	i = 0;

	*p_had = FALSE;
	if ( metalen <= 0 || js_parse((const UB *)meta, (INT)metalen, &root) < E_OK ) return;
	if ( js_get(&root, "microscriptSV", &arr) < E_OK || js_type(&arr) != JS_ARRAY ) return;
	it.s = NULL;
	while ( i < 50 && js_next(&arr, &it) ) {
		D	d = 0;

		if ( js_num(&it, &d) >= E_OK ) sv[i] = (double)d;
		i++;
	}
	*p_had = TRUE;
}

/* A key of the metadata's top object given the value (JSON text), and the metadata written */
static void meta_put( const char *key, const MSBUF *a )
{
	T_JSON	root, v;
	MSBUF	b;
	ID	k;

	if ( metalen <= 0 || js_parse((const UB *)meta, (INT)metalen, &root) < E_OK ) return;
	mb_init(&b);
	if ( js_get(&root, key, &v) >= E_OK ) {
		INT	at = (INT)( v.s - (const UB *)meta );

		mb_putn(&b, meta, at);
		mb_putn(&b, a->s, a->n);
		mb_putn(&b, meta + at + v.len, (INT)metalen - at - v.len);
	} else {
		INT	end = (INT)metalen;

		while ( end > 0 && meta[end - 1] != '}' ) end--;
		if ( end <= 0 ) { mb_free(&b); return; }
		mb_putn(&b, meta, end - 1);
		mb_printf(&b, ",\"%s\":", key);
		mb_putn(&b, a->s, a->n);
		mb_putc(&b, '}');
	}
	/*
	 * An object is written through one key alone: the figure's own key,
	 * open for reading while the script runs, is let go for the write
	 * and taken again after.
	 */
	if ( ms_win.kfig > 0 ) ob_cls_obj(ms_win.kfig);
	k = ob_opn_obj(&ms_win.fig, OB_OP_R | OB_OP_ATRWR);
	if ( k > 0 ) {
		if ( b.n <= OB_ATR_MAX && ob_set_atr(k, (const UB *)b.s, b.n) >= E_OK ) {
			memcpy(meta, b.s, (size_t)b.n);
			metalen = b.n;
			meta[metalen] = 0;
		}
		ob_cls_obj(k);
	}
	ms_win.kfig = ob_opn_obj(&ms_win.fig, OB_OP_R);
	mb_free(&b);
}

/* "networkGrants": the hosts the person allowed, as TADjs keeps them */
BOOL ms_net_granted( const char *host )
{
	T_JSON	root, arr, it;
	UB	h[256];

	if ( metalen <= 0 || js_parse((const UB *)meta, (INT)metalen, &root) < E_OK ) return FALSE;
	if ( js_get(&root, "networkGrants", &arr) < E_OK || js_type(&arr) != JS_ARRAY ) return FALSE;
	it.s = NULL;
	while ( js_next(&arr, &it) ) {
		if ( js_str(&it, h, sizeof(h)) >= 0 && strcmp((const char *)h, host) == 0 ) return TRUE;
	}
	return FALSE;
}

void ms_net_grant( const char *host )
{
	T_JSON	root, arr, it;
	MSBUF	a;
	BOOL	any = FALSE;
	INT	i;

	if ( ms_net_granted(host) ) return;
	mb_init(&a);
	mb_putc(&a, '[');
	if ( metalen > 0 && js_parse((const UB *)meta, (INT)metalen, &root) >= E_OK
	  && js_get(&root, "networkGrants", &arr) >= E_OK && js_type(&arr) == JS_ARRAY ) {
		it.s = NULL;
		while ( js_next(&arr, &it) ) {
			if ( any ) mb_putc(&a, ',');
			mb_putn(&a, (const char *)it.s, it.len);
			any = TRUE;
		}
	}
	if ( any ) mb_putc(&a, ',');
	mb_putc(&a, '"');
	for ( i = 0; host[i] != 0; i++ ) {
		if ( host[i] == '"' || host[i] == '\\' ) mb_putc(&a, '\\');
		mb_putc(&a, host[i]);
	}
	mb_puts(&a, "\"]");
	meta_put("networkGrants", &a);
	mb_free(&a);
}

void ms_sv_store( const double *sv )
{
	MSBUF	a;
	INT	i;

	mb_init(&a);
	mb_putc(&a, '[');
	for ( i = 0; i < 50; i++ ) mb_printf(&a, "%s%d", i > 0 ? "," : "", ms_int32(sv[i]));
	mb_putc(&a, ']');
	meta_put("microscriptSV", &a);
	mb_free(&a);
}

/*
 * How the window opens next time, kept as "microscriptWindow": the mode
 * (WSIZE 0 0, WSIZE 0 1, FULLWIND), where WSAVE left it, and the place
 * the figure's own window had then. When the figure is opened and its
 * window moved or sized, that place no longer matches and all of it is
 * forgotten.
 */
enum { WM_NORMAL, WM_PLAIN, WM_WIDE, WM_FULL };

static struct {
	INT	mode;
	BOOL	saved;
	INT	x, y, w, h;		/* the frame's top left, the work area's size */
} wnext;
static INT	wbase[4];		/* the figure's window: pos.x, pos.y, width, height */

static void wnext_read( void )
{
	T_JSON	root, wj, pos, o, arr, it;
	INT	b[4], i = 0;

	wbase[0] = wbase[1] = 150;
	wbase[2] = 640;
	wbase[3] = 480;
	if ( metalen <= 0 || js_parse((const UB *)meta, (INT)metalen, &root) < E_OK ) return;
	if ( js_get(&root, "window", &wj) >= E_OK ) {
		if ( js_get(&wj, "pos", &pos) >= E_OK ) {
			wbase[0] = (INT)js_get_num(&pos, "x", wbase[0]);
			wbase[1] = (INT)js_get_num(&pos, "y", wbase[1]);
		}
		wbase[2] = (INT)js_get_num(&wj, "width", wbase[2]);
		wbase[3] = (INT)js_get_num(&wj, "height", wbase[3]);
	}
	if ( js_get(&root, "microscriptWindow", &o) < E_OK ) return;
	if ( js_get(&o, "base", &arr) < E_OK || js_type(&arr) != JS_ARRAY ) return;
	it.s = NULL;
	while ( i < 4 && js_next(&arr, &it) ) {
		D	d = 0;

		(void)js_num(&it, &d);
		b[i++] = (INT)d;
	}
	if ( i < 4 || memcmp(b, wbase, sizeof(b)) != 0 ) return;
	wnext.mode = (INT)js_get_num(&o, "mode", WM_NORMAL);
	if ( js_get(&o, "x", &it) >= E_OK ) {
		wnext.saved = TRUE;
		wnext.x = (INT)js_get_num(&o, "x", 0);
		wnext.y = (INT)js_get_num(&o, "y", 0);
		wnext.w = (INT)js_get_num(&o, "w", wbase[2]);
		wnext.h = (INT)js_get_num(&o, "h", wbase[3]);
	}
}

static void wnext_write( void )
{
	MSBUF	a;

	mb_init(&a);
	mb_printf(&a, "{\"mode\":%d,\"base\":[%d,%d,%d,%d]", wnext.mode, wbase[0], wbase[1], wbase[2], wbase[3]);
	if ( wnext.saved ) mb_printf(&a, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d", wnext.x, wnext.y, wnext.w, wnext.h);
	mb_putc(&a, '}');
	meta_put("microscriptWindow", &a);
	mb_free(&a);
}

/* WSAVE: where the window is and how big, for the next start */
void ms_win_save( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	wnext.saved = TRUE;
	wnext.x = wp.left;
	wnext.y = wp.top;
	wnext.w = wp.wright - wp.wleft;
	wnext.h = wp.wbottom - wp.wtop;
	wnext_write();
}

/* WSIZE 0 0 (1), WSIZE 0 1 (2), FULLWIND (3): the window that opens next time */
void ms_win_mode( INT mode )
{
	wnext.mode = mode;
	wnext_write();
}

/* ---------------------------------------------------------------- the script */

typedef struct {
	char	*name;
	TS_UUID	uuid;
} LNAME;

static LNAME	*lnames;
static INT	nlnames;

BOOL ms_link_by_name( const char *name, TS_UUID *u )
{
	INT	i;

	for ( i = nlnames - 1; i >= 0; i-- ) {
		if ( strcmp(lnames[i].name, name) == 0 ) { *u = lnames[i].uuid; return TRUE; }
	}
	return FALSE;
}

/* An object's name and its first record */
static BOOL read_object( const TS_UUID *u, char *name, INT nmax, char **p_rec, INT *p_len )
{
	static char	atr[OB_ATR_MAX + 1];
	SZ		asz = 0;
	ID		k = ob_opn_obj(u, OB_OP_R);
	T_JSON		root;
	T_OBREC		r[4];
	INT		cnt = 0;

	name[0] = 0;
	*p_rec = NULL;
	*p_len = 0;
	if ( k <= 0 ) return FALSE;
	if ( ob_get_atr(k, (UB *)atr, OB_ATR_MAX, &asz) >= E_OK ) {
		atr[asz] = 0;
		if ( js_parse((const UB *)atr, (INT)asz, &root) >= E_OK ) {
			(void)js_get_str(&root, "name", (UB *)name, nmax);
		}
	}
	if ( p_rec != NULL && ob_lst_rec(k, r, 4, &cnt) >= E_OK && cnt > 0 ) {
		*p_rec = ms_alloc((size_t)r[0].size + 1);
		if ( ob_rea_rec(k, 0, 0, *p_rec, (SZ)r[0].size, &asz) >= E_OK ) {
			*p_len = (INT)asz;
			(*p_rec)[asz] = 0;
		}
	}
	ob_cls_obj(k);
	return TRUE;
}

/* The name normalised as the script's names are */
static void norm_name( const char *in, char *out, INT max )
{
	INT	i, k = 0, n = (INT)strlen(in);

	for ( i = 0; i < n && k < max - 5; ) {
		UW	cp;
		INT	d = ms_utf8_dec((const UB *)in + i, n - i, &cp);

		if ( d <= 0 ) break;
		k += ms_utf8_enc(ms_norm_cp(cp), (UB *)out + k);
		i += d;
	}
	out[k] = 0;
}

/* ---------------------------------------------------------------- paths */

/*
 * The objects the virtual objects of an object point to, in the order
 * they stand in its first record; and their names when `names` is not
 * NULL (MS_NAME bytes each).
 */
INT ms_links_of( const TS_UUID *u, TS_UUID *out, char (*names)[MS_NAME], INT max )
{
	char	name[MS_NAME];
	char	*rec = NULL;
	INT	len = 0, n = 0;
	MSXDOC	d;
	MSX	*e;

	if ( !read_object(u, name, sizeof(name), &rec, &len) || rec == NULL ) {
		ms_free(rec);
		return -1;
	}
	msx_parse(&d, rec, len);
	/* every <link>, in the order of the text */
	for ( e = d.root->first; e != NULL && n < max; ) {
		if ( msx_is(e, "link") ) {
			TS_UUID	t;

			if ( ms_uuid(msx_attr(e, "id"), &t) || ms_uuid(msx_attr(e, "link_id"), &t) ) {
				out[n] = t;
				if ( names != NULL ) {
					char	*r2 = NULL;
					INT	l2;

					names[n][0] = 0;
					(void)read_object(&t, names[n], MS_NAME, &r2, &l2);
					ms_free(r2);
				}
				n++;
			}
		}
		if ( e->first != NULL ) { e = e->first; continue; }
		while ( e != NULL && e->next == NULL ) e = e->parent;
		if ( e != NULL ) e = e->next;
	}
	ms_free(rec);
	return n;
}

/*
 * The object a path names: 【/】【name/】name, each name with ":n" after
 * it for the n-th of that name (from 0); '\' takes the next character as
 * it is. From "/" it starts at the system box, otherwise at the figure.
 * A name of a virtual object of the figure, or a UUID, names that object.
 */
BOOL ms_path_resolve( const char *path, TS_UUID *out )
{
	TS_UUID	cur, *ids;
	char	(*nm)[MS_NAME];
	char	comp[MS_NAME];
	const char *p = path;
	BOOL	ok = TRUE;

	if ( path == NULL || path[0] == 0 ) return FALSE;
	if ( ms_link_by_name(path, out) ) return TRUE;
	if ( strchr(path, '/') == NULL && ms_uuid(path, out) ) return TRUE;
	if ( *p == '/' ) {
		if ( !ms_uuid(SYSDEF_SYSTEM_BOX, &cur) ) return FALSE;
		p++;
	} else {
		cur = ms_win.fig;
	}
	ids = ms_alloc(sizeof(TS_UUID) * 256);
	nm = ms_alloc(sizeof(*nm) * 256);
	while ( ok && *p != 0 ) {
		INT	k = 0, occ = 0, n, i, seen = 0;
		char	*colon;

		while ( *p != 0 && *p != '/' && k < MS_NAME - 1 ) {
			if ( *p == '\\' && p[1] != 0 ) p++;
			comp[k++] = *p++;
		}
		comp[k] = 0;
		if ( *p == '/' ) p++;
		colon = strrchr(comp, ':');
		if ( colon != NULL && colon[1] >= '0' && colon[1] <= '9' ) {
			occ = atoi(colon + 1);
			*colon = 0;
			while ( colon > comp && colon[-1] == ' ' ) *--colon = 0;
		}
		n = ms_links_of(&cur, ids, nm, 256);
		ok = FALSE;
		for ( i = 0; i < n; i++ ) {
			if ( strcmp(nm[i], comp) == 0 && seen++ == occ ) {
				cur = ids[i];
				ok = TRUE;
				break;
			}
		}
	}
	ms_free(ids);
	ms_free(nm);
	if ( ok ) *out = cur;
	return ok;
}

/* The text of a script: each paragraph a line, the tags that are not text left out */
static void script_text( const char *xml, INT len, MSBUF *out )
{
	MSXDOC	d;
	MSX	*doc, *p;

	msx_parse(&d, xml, len);
	doc = msx_find(d.root, "document");
	if ( doc == NULL ) return;
	for ( p = doc->first; p != NULL; p = p->next ) {
		MSBUF	l;
		INT	i, n;
		static const char *const skip[] = {
			"link", "paper", "docmargin", "tab-format", "docView", "docDraw", "docScale",
			"figView", "figDraw", "figScale", "font", "text", "pattern", "mask",
			"underline", "strikethrough", "realtime", "realdata", "realgroup",
			"interpolate", "unitsystem", NULL
		};

		if ( !msx_is(p, "p") ) continue;
		mb_init(&l);
		mb_putn(&l, "", 0);
		{
			/* the text inside, the elements that are not text left out */
			MSX	*stack[32], *c;
			INT	sp = 0;

			c = p->first;
			while ( c != NULL || sp > 0 ) {
				if ( c == NULL ) { c = stack[--sp]->next; continue; }
				if ( c->tag == NULL ) {
					if ( c->text != NULL ) mb_puts(&l, c->text);
					c = c->next;
					continue;
				}
				if ( msx_is(c, "br") ) { mb_putc(&l, ' '); c = c->next; continue; }
				for ( i = 0; skip[i] != NULL && !msx_is(c, skip[i]); i++ ) ;
				if ( skip[i] != NULL || c->first == NULL || sp >= 32 ) { c = c->next; continue; }
				stack[sp++] = c;
				c = c->first;
			}
		}
		/* newlines inside the paragraph are spaces; no blanks at its end */
		for ( i = 0; i < l.n; i++ ) if ( l.s[i] == '\r' || l.s[i] == '\n' ) l.s[i] = ' ';
		n = l.n;
		while ( n > 0 && ( l.s[n - 1] == ' ' || l.s[n - 1] == '\t' ) ) n--;
		if ( out->n > 0 ) mb_putc(out, '\n');
		mb_putn(out, l.s, n);
		mb_free(&l);
	}
}

/* SCRIPT..., the name of a script */
static BOOL is_script_name( const char *nn )
{
	return (BOOL)( ( nn[0] == 'S' || nn[0] == 's' ) && strlen(nn) >= 6
		    && strncmp(ms_upper(ms_intern(nn, 6))->s, "SCRIPT", 6) == 0 );
}

/* The virtual objects of the figure that are not scripts: segments by their objects' names */
static void add_vobjs( void )
{
	INT	i;

	for ( i = 0; i < ms_fig.nlink; i++ ) {
		char	name[256], nn[256];
		char	*rec = NULL;
		INT	len = 0;

		if ( !read_object(&ms_fig.link[i].target, name, sizeof(name), &rec, &len) ) continue;
		ms_free(rec);
		norm_name(name, nn, sizeof(nn));
		if ( nn[0] == 0 || is_script_name(nn) || ( nn[0] == '@' && nn[1] == '@' ) ) continue;
		ms_fig_add_vobj(&ms_fig, i, ms_intern_z(nn));
	}
	ms_fig_order(&ms_fig);
}

static INT load_scripts( MSBUF *src )
{
	INT	i, nsrc = 0;

	for ( i = 0; i < ms_fig.nlink && nsrc < 32; i++ ) {
		char	name[256], nn[256];
		char	*rec;
		INT	len;

		if ( !read_object(&ms_fig.link[i].target, name, sizeof(name), &rec, &len) ) continue;
		if ( name[0] != 0 ) {
			lnames = ms_realloc(lnames, sizeof(LNAME) * (size_t)( nlnames + 1 ));
			lnames[nlnames].name = ms_strdup(name);
			lnames[nlnames].uuid = ms_fig.link[i].target;
			nlnames++;
		}
		norm_name(name, nn, sizeof(nn));
		if ( is_script_name(nn) ) {
			if ( rec != NULL && len > 0 ) {
				if ( src->n > 0 ) mb_putc(src, '\n');
				script_text(rec, len, src);
				nsrc++;
			}
		} else if ( nn[0] == '@' && nn[1] == '@' && rec != NULL ) {
			/* a figure of more segments */
			MSFIG	*sub = ms_alloc(sizeof(MSFIG));
			INT	k;

			if ( ms_fig_read(sub, rec, len) >= E_OK ) {
				for ( k = 0; k < sub->nseg; k++ ) {
					if ( st_seg(sub->seg[k].name) != NULL ) continue;
					(void)st_add(sub->seg[k].name, &sub->seg[k]);
				}
			}
			continue;		/* its text stays: the pieces point into it */
		}
		ms_free(rec);
	}
	return nsrc;
}

/* ---------------------------------------------------------------- the menu */

/* 再起動: the program started again on the figure, as read now; this run ends without its epilogue */
static void restart( void )
{
	T_OBCRE	c;
	TS_UUID	prog, pu;
	MSTH	*t;

	if ( !ms_uuid(SYSDEF_PROG_MICROSCRIPT, &prog) ) return;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &ms_win.fig;
	c.argsz = sizeof(ms_win.fig);
	if ( ob_cre_obj(&c, &pu) < E_OK ) {
		ms_message("再起動できません\n");
		return;
	}
	rt.finished = TRUE;
	rt.epilogue_done = TRUE;
	for ( t = rt.threads; t != NULL; t = t->next ) t->terminated = TRUE;
	closing = TRUE;
}

static BOOL sel_is( const T_MNSEL *sel, const char *code )
{
	return (BOOL)( strcmp((const char *)sel->code, code) == 0 );
}

/* ソース表示: the script's objects opened as a double click opens them */
static void show_source( void )
{
	INT	i, pid, opened = 0;
	char	nn[256];

	for ( i = 0; i < nlnames; i++ ) {
		norm_name(lnames[i].name, nn, sizeof(nn));
		if ( is_script_name(nn) && ms_vopen_obj(&lnames[i].uuid, &pid) >= E_OK ) {
			opened++;
		}
	}
	if ( opened == 0 ) {
		ms_message("台本の実身を開けませんでした\n");
	}
}

static void menu( const T_OBNTM *m )
{
	TS_UUID	def, opdef;
	T_MNSEL	sel;
	ID	mid;
	INT	n = rt_menu_count(), i;

	if ( !ms_uuid(SYSDEF_MENU_MICROSCRIPT, &def) || mn_cre_men(&def, &mid) < E_OK ) return;
	if ( ms_uuid(SYSDEF_MENU_MS_OPS, &opdef) && n > 0 ) {
		UB	*labels = ms_alloc((size_t)( n * 64 ));

		for ( i = 0; i < n; i++ ) {
			INT	ln, k = 0, j;
			const UW *l = rt_menu_label(i, &ln);

			/* "@X..." gives the item the letter X; the rest is its label */
			j = ( ln >= 2 && l[0] == '@' ) ? 2 : 0;
			for ( ; j < ln && k < 58; j++ ) k += ms_utf8_enc(l[j], labels + i * 64 + k);
			labels[i * 64 + k] = 0;
		}
		(void)mn_set_lst(mid, &opdef, "op", labels, 64, n);
		ms_free(labels);
	}
	if ( mn_pop_men(mid, kw, m->x, m->y, m->when, &sel) >= E_OK ) {
		if ( sel_is(&sel, "close") ) {
			closing = TRUE;
		} else if ( sel_is(&sel, "restart") ) {
			restart();
		} else if ( sel_is(&sel, "stop") ) {
			MSTH	*t;

			for ( t = rt.threads; t != NULL; t = t->next ) t->terminated = TRUE;
		} else if ( sel_is(&sel, "source") ) {
			show_source();
		} else if ( sel_is(&sel, "op") ) {
			rt_menu(sel.index);
		}
	}
	(void)mn_del_men(mid);
}

/* ---------------------------------------------------------------- keys and the pointer */

static UW key_code( UINT code, UINT mods, UB *p_ascii )
{
	UB	c;

	*p_ascii = 0;
	switch ( code ) {
	case K_ENTER:	return 0x0A;
	case K_BS:	return 0x08;
	case K_DEL:	return 0x7F;
	case K_TAB:	return 0x09;
	case K_ESC:	return 0x1B;
	case K_UP:	return 0x100;
	case K_DOWN:	return 0x101;
	case K_RIGHT:	return 0x102;
	case K_LEFT:	return 0x103;
	}
	c = wm_key_char(code, mods);
	*p_ascii = c;
	if ( c == ' ' ) return 0x2121;
	if ( c >= 0x21 && c <= 0x7E ) return 0x2300 + c;
	return c;
}

static UW meta_of( UINT mods )
{
	return ( ( mods & M_SHIFT ) ? 0x10U : 0 ) | ( ( mods & M_CTRL ) ? 0x40U : 0 ) | ( ( mods & M_ALT ) ? 0x80U : 0 );
}

static BOOL is_vobj( const MSSEG *s )
{
	return (BOOL)( s->fs != NULL && s->fs->isvobj );
}

static BOOL has_pointer_handler( MSSEG *s )
{
	return (BOOL)( rt_has_any_handler(s) || is_vobj(s) );
}

/* A double click: a virtual object segment is opened, then the script's own procedures run */
static void fire_dclick( MSSEG *s, INT x, INT y )
{
	if ( is_vobj(s) && !rt.finished ) {
		INT	pid;

		if ( ms_vopen_obj(&s->fs->target, &pid) >= E_OK && pid > 0 ) s->pid = pid;
	}
	rt_event(EV_DCLICK, s, x, y);
}

/* A click waiting to see whether a second makes it a double click */
static MSSEG	*pend_seg;
static UD	pend_at;
static INT	pend_x, pend_y;

static void fire_click( MSSEG *s, INT x, INT y )
{
	rt_event(EV_CLICK, s, x, y);
	rt_event(EV_QPRESS, s, x, y);
}

static BOOL	ime_pressed;		/* the press was on the candidates: its release is theirs */

static void pointer( const T_OBNTM *m )
{
	MSSEG	*s;

	ms_win.pdx = m->x;
	ms_win.pdy = m->y;
	if ( m->event == OB_E_MOVE ) return;
	if ( ask.pending ) {
		if ( m->event == OB_E_RELEASE && m->code != 1 ) {
			if ( in_rect(&ask.yes, m->x, m->y) ) ask.answer = 1;
			else if ( in_rect(&ask.no, m->x, m->y) ) ask.answer = 0;
		}
		return;
	}
	if ( m->event == OB_E_PRESS ) {
		if ( m->code == 1 ) {
			menu(m);
			return;
		}
		ime_pressed = ms_ime_press(m->x, m->y);
		if ( ime_pressed ) return;
		ms_win.pdb = 1;
		s = st_hit(m->x, m->y, has_pointer_handler);
		if ( s != NULL ) rt_event(EV_PRESS, s, m->x, m->y);
		return;
	}
	/* released: a click on what is under it */
	if ( m->code == 1 ) return;
	if ( ime_pressed ) {
		ime_pressed = FALSE;
		return;
	}
	ms_win.pdb = 0;
	s = st_hit(m->x, m->y, has_pointer_handler);
	if ( s == NULL ) return;
	if ( rt_has_dclick(s) || is_vobj(s) ) {
		if ( pend_seg == s && ms_now_ms() - pend_at < DCLICK_MS ) {
			pend_seg = NULL;
			fire_dclick(s, m->x, m->y);
		} else {
			pend_seg = s;
			pend_at = ms_now_ms();
			pend_x = m->x;
			pend_y = m->y;
		}
		return;
	}
	fire_click(s, m->x, m->y);
}

static void key( const T_OBNTM *m )
{
	UB	ascii;
	UW	code = key_code(m->code, m->mods, &ascii), meta = meta_of(m->mods);

	if ( ( m->mods & M_CTRL ) && ( ascii == 'e' || ascii == 'E' ) ) {
		closing = TRUE;
		return;
	}
	if ( ask.pending ) {
		if ( ascii == 'y' || ascii == 'Y' || code == 0x0A || code == 0x0D ) ask.answer = 1;
		else if ( ascii == 'n' || ascii == 'N' || code == 0x1B ) ask.answer = 0;
		return;
	}
	if ( st_input_seg() != NULL ) {
		UW	ch = ( code == 0x0A || code == 0x08 || code == 0x7F || code == 0x102 || code == 0x103 ) ? code : ascii;

		/* KINPUT: the converter first */
		if ( st_input_kanji() && ms_ime_key(m->code, m->mods) ) return;

		if ( code == 0x09 ) {
			rt_key(0x09, meta);
			return;
		}
		if ( ch != 0 && st_input_key(ch) ) return;
	}
	rt_key(code, meta);
}

/* ---------------------------------------------------------------- start */

static ER open_window( const char *title )
{
	T_OBCRE	c;
	T_OBNTF	req;
	char	json[192];
	INT	l, t, w, h, n;
	T_OBWPOS wp;
	SZ	asz = 0;
	char	fid[40];

	wnext_read();
	l = wbase[0];
	t = wbase[1];
	w = wbase[2];
	h = wbase[3];
	if ( wnext.saved ) {
		l = wnext.x;
		t = wnext.y;
		w = wnext.w;
		h = wnext.h;
	}
	switch ( wnext.mode ) {
	case WM_FULL:
		snprintf(json, sizeof(json), "{\"rect\":[0,0,%d,%d],\"attr\":0,\"full\":1}", w, h);
		break;
	case WM_PLAIN:
		/* no frame: the work area is the window */
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":0}", l, t, l + w, t + h);
		break;
	case WM_WIDE:
		snprintf(json, sizeof(json), "{\"rect\":[0,%d,%d,%d],\"attr\":0,\"wide\":1}", t, w, t + h);
		break;
	default:
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7}", l, t, l + w + FRAME_W, t + h + TITLE_H);
		break;
	}
	/* and the figure it shows, for those who look for the windows it is open in */
	n = (INT)strlen(json);
	ms_uuid_str(&ms_win.fig, fid);
	if ( n > 0 ) {
		snprintf(json + n - 1, sizeof(json) - (size_t)n + 1, ",\"shows\":\"%s\"}", fid);
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (const UB *)title;
	c.json = (const UB *)json;
	c.jsonsz = (INT)strlen(json);
	if ( ob_cre_obj(&c, &win) < E_OK ) return E_OBJ;
	kw = ob_opn_obj(&win, OB_OP_ALL);
	gid = ( kw > 0 ) ? wm_obj_gid(kw) : E_ID;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	port = ( ob_cre_obj(&c, &ch) >= E_OK ) ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE;
	if ( kw <= 0 || gid < 0 || port <= 0 || ob_ntf_evt(kw, OB_REC_ANY, &req, port) <= 0 ) return E_OBJ;
	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && wp.sw > 0 ) {
		ms_win.scrw = wp.sw;
		ms_win.scrh = wp.sh;
	}
	if ( wnext.mode == WM_NORMAL ) ms_win_resize(w, h);
	return E_OK;
}

static void close_window( void )
{
	if ( port > 0 ) {
		ob_cls_obj(port);
		(void)ob_del_obj(&ch);
	}
	if ( kw > 0 ) ob_cls_obj(kw);
	(void)ob_del_obj(&win);
	port = kw = 0;
	gid = -1;
}

static void preset_numbers( void )
{
	MSSEG	*s;

	/* a segment named N and a number starts $SV of that number less one with its value */
	for ( s = st_first(); s != NULL; s = s->next ) {
		const char	*n = s->name->s;
		INT		i, v = 0;
		MSV		val;

		if ( n[0] != 'N' || n[1] == 0 ) continue;
		for ( i = 1; n[i] >= '0' && n[i] <= '9'; i++ ) v = v * 10 + ( n[i] - '0' );
		if ( n[i] != 0 ) continue;
		val = seg_state(s, ms_intern_z("V"));
		if ( val.t == V_NUM && val.u.n != MS_INVALID ) rt_preset_sv(v - 1, ms_int32(val.u.n));
	}
}

static BOOL boot( void )
{
	char	name[256];
	char	*rec = NULL;
	INT	len = 0, i, nsrc;
	MSBUF	src;
	MSTOKS	toks;
	char	err[256];

	memset(&toks, 0, sizeof(toks));
	if ( !read_object(&ms_win.fig, name, sizeof(name), &rec, &len) || rec == NULL ) {
		ms_message("図形データが読めません。\n");
		return FALSE;
	}
	if ( ms_fig_read(&ms_fig, rec, len) < E_OK ) {
		ms_message("[図形TADエラー] <figure> 要素が見つかりません\n");
		return FALSE;
	}
	{
		T_JSON	root, wj;
		UB	bg[16];

		ms_fig.paper = 0x00FFFFFF;
		if ( metalen > 0 && js_parse((const UB *)meta, (INT)metalen, &root) >= E_OK
		  && js_get(&root, "window", &wj) >= E_OK
		  && js_get_str(&wj, "backgroundColor", bg, sizeof(bg)) > 0 ) {
			ms_fig.paper = parse_colour((const char *)bg, 0x00FFFFFF);
		}
	}
	add_vobjs();
	st_init(&ms_fig, ms_fig.paper);
	for ( i = 0; i < ms_fig.nseg; i++ ) {
		MSSEG	*g = st_add(ms_fig.seg[i].name, &ms_fig.seg[i]);

		if ( ms_fig.seg[i].isvobj ) g->pid = -1;	/* nothing started from it yet */
	}
	preset_numbers();

	mb_init(&src);
	mb_putn(&src, "", 0);
	nsrc = load_scripts(&src);
	if ( ms_fig.nseg == 0 && ms_fig.nlink == 0 ) {
		ms_message("役者セグメントが見つかりません。グループに「@名前」文字枠を含めてください。\n");
	}
	if ( nsrc == 0 ) {
		ms_message("SCRIPT: で始まる文章実身(台本)が貼り込まれていません。図形に SCRIPT:〜 仮身を貼り込んでください。\n");
		return FALSE;
	}
	source = src.s;
	sourcelen = src.n;
	if ( ms_lex(source, sourcelen, &toks, err, sizeof(err)) < E_OK ) {
		ms_message(err);
		ms_message("\n");
		return FALSE;
	}
	if ( ms_parse(&toks, &ms_prog, err, sizeof(err)) < E_OK ) {
		ms_message(err);
		ms_message("\n");
		return FALSE;
	}
	(void)rt_load(&ms_prog);
	st_sort_by_place();
	rt_start();
	return TRUE;
}

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	char	name[256];
	char	*rec;
	INT	len;
	UD	close_at = 0;

	mb_init(&msg_cur);
	ms_win.update = TRUE;
	ms_win.start_ms = ms_now_ms();
	ms_win.scrw = 1280;
	ms_win.scrh = 800;
	if ( ts_get_arg(&ms_win.fig, sizeof(ms_win.fig)) < (SZ)sizeof(ms_win.fig) ) {
		tm_putstring((const UB *)"microscript: started without a figure\n");
		return 1;
	}
	ms_win.kfig = ob_opn_obj(&ms_win.fig, OB_OP_R);
	if ( ms_win.kfig <= 0 ) {
		tm_putstring((const UB *)"microscript: the figure cannot be opened\n");
		return 2;
	}
	meta_read();
	name[0] = 0;
	(void)read_object(&ms_win.fig, name, sizeof(name), &rec, &len);
	ms_free(rec);
	if ( open_window(name[0] != 0 ? name : "マイクロスクリプト") < E_OK ) {
		tm_putstring((const UB *)"microscript: no window\n");
		close_window();
		return 3;
	}
	ms_win.wid = (INT)( ( (UW *)&win )[3] & 0x7FFFFF ) | 1;
	(void)boot();
	draw_all();

	for ( ;; ) {
		UD	now, wake;

		while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
			switch ( m.event ) {
			case OB_E_CLOSE:
			case OB_E_DELETE:
				closing = TRUE;
				break;
			case OB_E_PRESS:
			case OB_E_RELEASE:
			case OB_E_MOVE:
				pointer(&m);
				break;
			case OB_E_KEY:
				key(&m);
				break;
			case OB_E_REDRAW:
				dirty = TRUE;
				break;
			default:
				break;
			}
		}
		if ( closing && close_at == 0 ) {
			rt_stop();
			close_at = ms_now_ms() + 3000;
		}
		if ( pend_seg != NULL && ms_now_ms() - pend_at >= DCLICK_MS ) {
			MSSEG	*s = pend_seg;

			pend_seg = NULL;
			fire_click(s, pend_x, pend_y);
		}
		rt_run_threads();
		if ( dirty && ms_win.update ) draw_all();
		if ( rt.finished && ( rt_epilogue_done() || ( close_at != 0 && ms_now_ms() >= close_at ) ) ) break;
		if ( rt.finished && close_at == 0 ) close_at = ms_now_ms() + 3000;
		if ( rt_ready() ) continue;
		now = ms_now_ms();
		wake = rt_next_wake();
		tk_dly_tsk(( wake > now + TICK_MS || wake <= now ) ? TICK_MS : (TMO)( wake - now ));
	}
	ms_ime_end();
	ms_net_end();
	ms_vobj_end();
	ms_shared_end();
	close_window();
	ob_cls_obj(ms_win.kfig);
	return 0;
}
