/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obdisp.c
 *	The screen as an object, 画面 (design 18.7)
 *
 *	A device of fixed UUID (ob_uuid_display), there when a screen was
 *	found at start. Record 1 is its mode as "key value" lines: the
 *	size, the depth, the row stride, the sizes it can be set to and
 *	what drives it. Written "WIDTHxHEIGHT" it sets the screen to that
 *	size through the display driver: the bochs display of QEMU, or on
 *	the Raspberry Pi the firmware, asked for a framebuffer of that
 *	size (the old size is kept when it refuses). Record 2 is the pixels of
 *	the back buffer, which is what is on the screen: rows of the row
 *	stride, four bytes a pixel.
 *
 *	Protection: everyone reads the mode, the administrators set it
 *	(rw-rw-r-- with their group), and the pixels are reached only with
 *	a key that may also control the device, x, which by default only an
 *	administrator's has: what is on the screen is what the person at it
 *	is reading.
 *
 *	The system object's record 3 is this object's record 1 seen from
 *	there, with a line naming this object. The drawing layers below the
 *	windows (dp, wm) keep the display driver's own calls: they draw
 *	into the back buffer thousands of times a second and are the
 *	screen's driver's users, not its readers.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/disp.h>
#include <ts/dispout.h>
#include <ts/conf.h>
#include "obj.h"


#if defined(RPI5) || defined(RPI4)
#define DRIVER		"firmware framebuffer (HDMI)"
#elif defined(X64)
/*
 * A PC: what drives the screen, the sizes it offers and how a size of
 * another shape is laid on its panel come from the driver (disp_gop.c)
 */
IMPORT CONST char *knl_disp_hw_driver( void );
IMPORT INT knl_disp_hw_modes( UH (*m)[2], INT max );
IMPORT INT knl_disp_hw_fit( INT fit, BOOL now );
#define DRIVER		knl_disp_hw_driver()
#define FIT_ASPECT	0
#define FIT_STRETCH	1

LOCAL BOOL word_at( CONST UB *b, CONST char *w )
{
	while ( *w != '\0' ) {
		if ( *b++ != (UB)*w++ ) return FALSE;
	}
	return TRUE;
}
#else
#define DRIVER		"bochs-display"
#endif

#ifndef X64
/* The sizes it can be set to */
LOCAL CONST UH	modes[][2] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
	{ 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 1920, 1200 }
};
#endif

LOCAL INT put_line( UB *t, INT n, INT max, CONST char *key, D v )
{
	n = knl_oj_put(t, n, max, key);
	n = knl_oj_put(t, n, max, "\t");
	n = knl_oj_put_num(t, n, max, v);
	return knl_oj_put(t, n, max, "\n");
}

LOCAL INT put_size( UB *t, INT n, INT max, UINT w, UINT h )
{
	n = knl_oj_put_num(t, n, max, w);
	n = knl_oj_put(t, n, max, "x");
	return knl_oj_put_num(t, n, max, h);
}

/*
 * The sizes the screen can be set to, every one, in a list taken for
 * them (*p_m, Kfree it): how many, or E_NOMEM. The driver is asked how
 * many first, and again with more room where it offers more meanwhile.
 */
LOCAL INT modes_take( UH (**p_m)[2] )
{
	UH	(*m)[2];
	INT	room, n;

#ifdef X64
	room = knl_disp_hw_modes(NULL, 0);
#else
	room = (INT)( sizeof(modes) / sizeof(modes[0]) );
#endif
	for ( ;; ) {
		m = (UH (*)[2])Kmalloc(sizeof(UH) * 2 * (SZ)( ( room > 0 ) ? room : 1 ));
		if ( m == NULL ) {
			*p_m = NULL;
			return E_NOMEM;
		}
#ifdef X64
		n = knl_disp_hw_modes(m, room);
#else
		knl_memcpy(m, modes, sizeof(modes));
		n = room;
#endif
		if ( n <= room ) {
			break;
		}
		Kfree(m);
		room = n;
	}
	*p_m = m;
	return ( n > 0 ) ? n : 0;
}

/* ---------------------------------------------------------------- the outputs */

/*
 * The outputs (docs/tessronos 10.4.9, 10.4.10). Each one is a device
 * object of its own, under its connector's name ("Virtual-2", "HDMI-A-1")
 * in the device box, which the screen's record 0 links to: record 1 says
 * what it is and the modes it offers, record 2 is its EDID. The screen's
 * record 1 has a line for each (OUT0, OUT1, ...) where there is more than
 * one; written with such lines, the outputs are set to them together
 * (ts_disp_commit): "OUT1 on 1024x768 right 0", "OUT1 off". The side is
 * right, below, left or above the screens before it. The table of
 * their objects is taken at start for as many as there are.
 */
LOCAL TS_UUID	*out_uuid = NULL;
LOCAL INT	out_n = 0;

LOCAL CONST char *side_word( INT side )
{
	return ( side == DISP_SIDE_BELOW ) ? "below" : ( side == DISP_SIDE_LEFT ) ? "left"
		: ( side == DISP_SIDE_ABOVE ) ? "above" : ( side == DISP_SIDE_MIRROR ) ? "mirror" : "right";
}

/* The side a setting names by its first letter ("right", "below", "left", "above", "mirror"), or -1 */
LOCAL INT side_of( UB c )
{
	return ( c == 'r' ) ? DISP_SIDE_RIGHT : ( c == 'b' ) ? DISP_SIDE_BELOW
		: ( c == 'l' ) ? DISP_SIDE_LEFT : ( c == 'a' ) ? DISP_SIDE_ABOVE
		: ( c == 'm' ) ? DISP_SIDE_MIRROR : -1;
}

LOCAL INT put_hex( UB *t, INT n, INT max, UW v )
{
	char	h[9];
	INT	k;

	for ( k = 0; k < 8; k++ ) {
		h[k] = "0123456789abcdef"[( v >> ( 28 - 4 * k ) ) & 0xF];
	}
	h[8] = 0;
	return knl_oj_put(t, n, max, h);
}

/*
 * Every mode of output i, in a list taken for them (*p_m, Kfree it; NULL
 * when it has none): how many, E_NOEXS, E_NOMEM. Asked how many first,
 * and again with more room where it offers more meanwhile.
 */
LOCAL INT out_modes_take( INT i, T_DISPMODE **p_m )
{
	T_DISPMODE	*m;
	INT		room, n;

	*p_m = NULL;
	room = ts_disp_out_modes(i, NULL, 0);
	for ( ;; ) {
		if ( room <= 0 ) {
			return room;
		}
		m = (T_DISPMODE *)Kmalloc(sizeof(T_DISPMODE) * (SZ)room);
		if ( m == NULL ) {
			return E_NOMEM;
		}
		n = ts_disp_out_modes(i, m, room);
		if ( n <= room ) {
			break;
		}
		Kfree(m);
		room = n;
	}
	if ( n <= 0 ) {
		Kfree(m);
		return n;
	}
	*p_m = m;
	return n;
}

/* Output i's EDID, all of it, the same way: how many bytes (*p_e, Kfree it; NULL when 0), or an error */
LOCAL INT out_edid_take( INT i, UB **p_e )
{
	UB	*e;
	INT	room, n;

	*p_e = NULL;
	room = ts_disp_out_edid(i, NULL, 0);
	for ( ;; ) {
		if ( room <= 0 ) {
			return room;
		}
		e = (UB *)Kmalloc((SZ)room);
		if ( e == NULL ) {
			return E_NOMEM;
		}
		n = ts_disp_out_edid(i, e, room);
		if ( n <= room ) {
			break;
		}
		Kfree(e);
		room = n;
	}
	if ( n <= 0 ) {
		Kfree(e);
		return n;
	}
	*p_e = e;
	return n;
}

/* The sizes of an output's modes, once each, its preferred first */
LOCAL INT put_out_modes( UB *t, INT n, INT max, INT i )
{
	T_DISPMODE	*m;
	INT		nm = out_modes_take(i, &m), k, j;

	if ( nm == E_NOMEM ) {
		return E_NOMEM;
	}
	for ( k = 0; k < nm; k++ ) {
		for ( j = 0; j < k && ( m[j].hdisp != m[k].hdisp || m[j].vdisp != m[k].vdisp ); j++ ) ;
		if ( j < k ) continue;
		if ( k > 0 ) n = knl_oj_put(t, n, max, " ");
		n = put_size(t, n, max, m[k].hdisp, m[k].vdisp);
	}
	if ( m != NULL ) Kfree(m);
	return n;
}

/*
 * How many outputs, from the main one, can be turned on: an output its
 * driver cannot set yet (an Intel port before its link is made) is not
 * offered, nor are those after it
 */
LOCAL INT outs_usable( void )
{
	T_DISPOUTINFO	oi;
	INT		i, no = ts_disp_outputs();

	for ( i = 0; i < no && ts_disp_output(i, &oi) >= E_OK && oi.usable; i++ ) ;
	return i;
}

/*
 * The screen's lines for the outputs, where there is more than one:
 * OUTPUTS, then for each "OUTi name on|off WIDTHxHEIGHT side offset ident
 * maker model" and "OUTMODESi" with the sizes it offers, and for each
 * besides the main one "OUTMIRRORi WIDTHxHEIGHT ok|no": the mode it would
 * mirror the main screen in, and whether it can (ts_disp_mirror_mode)
 */
LOCAL INT outs_text( UB *t, INT n, INT max )
{
	T_DISPOUTINFO	oi;
	T_DISPSTATE	st;
	INT		i, no = outs_usable();
	char		key[24];
	ER		er;

	if ( no <= 1 ) {
		return n;
	}
	er = ts_disp_state(&st);
	if ( er < E_OK ) {
		return ( er == E_NOMEM ) ? E_NOMEM : n;
	}
	n = put_line(t, n, max, "OUTPUTS", no);
	for ( i = 0; i < no && i < st.n; i++ ) {
		if ( ts_disp_output(i, &oi) < E_OK ) break;
		(void)tm_sprintf((UB *)key, (CONST UB *)"OUT%d", i);
		n = knl_oj_put(t, n, max, key);
		n = knl_oj_put(t, n, max, "\t");
		n = knl_oj_put(t, n, max, oi.name);
		n = knl_oj_put(t, n, max, st.out[i].on ? " on " : " off ");
		n = put_size(t, n, max, st.out[i].w, st.out[i].h);
		n = knl_oj_put(t, n, max, " ");
		n = knl_oj_put(t, n, max, side_word(st.out[i].side));
		n = knl_oj_put(t, n, max, " ");
		n = knl_oj_put_num(t, n, max, st.out[i].offset);
		n = knl_oj_put(t, n, max, " ");
		n = put_hex(t, n, max, oi.ident);
		if ( oi.maker[0] != 0 ) {
			n = knl_oj_put(t, n, max, " ");
			n = knl_oj_put(t, n, max, oi.maker);
			n = knl_oj_put(t, n, max, " ");
			n = knl_oj_put(t, n, max, oi.model);
		}
		n = knl_oj_put(t, n, max, "\n");
		(void)tm_sprintf((UB *)key, (CONST UB *)"OUTMODES%d", i);
		n = knl_oj_put(t, n, max, key);
		n = knl_oj_put(t, n, max, "\t");
		n = put_out_modes(t, n, max, i);
		if ( n < -1 ) break;
		n = knl_oj_put(t, n, max, "\n");
		if ( i > 0 ) {
			/* the mode it would mirror the main screen in, and whether it can */
			UINT	mw = 0, mh = 0;
			ER	ok = ts_disp_mirror_mode(i, &mw, &mh);

			(void)tm_sprintf((UB *)key, (CONST UB *)"OUTMIRROR%d", i);
			n = knl_oj_put(t, n, max, key);
			n = knl_oj_put(t, n, max, "\t");
			n = put_size(t, n, max, mw, mh);
			n = knl_oj_put(t, n, max, ( ok >= E_OK ) ? " ok\n" : " no\n");
		}
	}
	ts_disp_state_free(&st);
	return n;
}

/* A number of a line, from *p_i; FALSE when there is none */
LOCAL BOOL num_at( CONST UB *b, SZ size, SZ *p_i, INT *p_v )
{
	SZ	i = *p_i;
	INT	v = 0;

	while ( i < size && ( b[i] == ' ' || b[i] == '\t' ) ) i++;
	if ( i >= size || b[i] < '0' || b[i] > '9' ) return FALSE;
	while ( i < size && b[i] >= '0' && b[i] <= '9' && v < 100000 ) v = v * 10 + ( b[i++] - '0' );
	*p_i = i;
	*p_v = v;
	return TRUE;
}

/* A word of a line, from *p_i: whether it is w */
LOCAL BOOL word_is( CONST UB *b, SZ size, SZ *p_i, CONST char *w )
{
	SZ	i = *p_i, k;

	while ( i < size && ( b[i] == ' ' || b[i] == '\t' ) ) i++;
	for ( k = 0; w[k] != 0; k++ ) {
		if ( i + k >= size || b[i + k] != (UB)w[k] ) return FALSE;
	}
	if ( i + k < size && b[i + k] != ' ' && b[i + k] != '\t' && b[i + k] != '\n' && b[i + k] != '\r' ) return FALSE;
	*p_i = i + k;
	return TRUE;
}

/*
 * Lines "OUTi on WIDTHxHEIGHT side offset" and "OUTi off" written to the
 * screen's record 1: the outputs set to them together, those not named
 * left as they are. E_PAR for a line not of that form.
 */
LOCAL ER outs_write( CONST UB *b, SZ size )
{
	T_DISPSTATE	st;
	SZ		i = 0;
	ER		er;

	er = ts_disp_state(&st);
	if ( er < E_OK ) {
		return er;
	}
	while ( i < size ) {
		INT	o, w, h, off = 0;
		SZ	e;

		while ( i < size && ( b[i] == '\n' || b[i] == '\r' || b[i] == ' ' ) ) i++;
		if ( i >= size ) break;
		if ( i + 3 >= size || b[i] != 'O' || b[i + 1] != 'U' || b[i + 2] != 'T' ) goto bad;
		i += 3;
		if ( b[i] < '0' || b[i] > '9' || !num_at(b, size, &i, &o) ) goto bad;
		if ( o >= st.n ) {
			ts_disp_state_free(&st);
			return E_NOEXS;
		}
		if ( word_is(b, size, &i, "off") ) {
			st.out[o].on = FALSE;
		} else if ( word_is(b, size, &i, "on") ) {
			if ( !num_at(b, size, &i, &w) || i >= size || ( b[i] != 'x' && b[i] != 'X' ) ) goto bad;
			i++;
			if ( !num_at(b, size, &i, &h) ) goto bad;
			st.out[o].on = TRUE;
			st.out[o].w = (UH)w;
			st.out[o].h = (UH)h;
			if ( word_is(b, size, &i, "right") ) {
				st.out[o].side = DISP_SIDE_RIGHT;
			} else if ( word_is(b, size, &i, "below") ) {
				st.out[o].side = DISP_SIDE_BELOW;
			} else if ( word_is(b, size, &i, "left") ) {
				st.out[o].side = DISP_SIDE_LEFT;
			} else if ( word_is(b, size, &i, "above") ) {
				st.out[o].side = DISP_SIDE_ABOVE;
			} else if ( word_is(b, size, &i, "mirror") ) {
				st.out[o].side = DISP_SIDE_MIRROR;
			}
			if ( num_at(b, size, &i, &off) ) st.out[o].offset = off;
		} else {
			goto bad;
		}
		for ( e = i; e < size && b[e] != '\n'; e++ ) ;
		i = e;
	}
	er = ts_disp_commit(&st);
	ts_disp_state_free(&st);
	knl_obdisp_changed();
	return er;

bad:
	ts_disp_state_free(&st);
	return E_PAR;
}

/* An output's record 1: what it is and its modes, as key and value lines */
LOCAL INT out_make( void *ctx, UB *t, INT max )
{
	INT		i = (INT)(UBINT)ctx, n = 0;
	T_DISPOUTINFO	oi;

	if ( ts_disp_output(i, &oi) < E_OK ) {
		return knl_oj_put(t, 0, max, "OUTPUT\tnone\n");
	}
	n = knl_oj_put(t, n, max, "NAME\t");
	n = knl_oj_put(t, n, max, oi.name);
	n = knl_oj_put(t, n, max, "\n");
	n = put_line(t, n, max, "KIND", oi.kind);
	n = put_line(t, n, max, "STATUS", oi.status);
	n = put_line(t, n, max, "SCREEN", oi.screen);
	n = knl_oj_put(t, n, max, "SIZE\t");
	n = put_size(t, n, max, oi.mode.hdisp, oi.mode.vdisp);
	n = knl_oj_put(t, n, max, "\n");
	n = put_line(t, n, max, "REFRESH", oi.mode.vrefresh);
	if ( oi.maker[0] != 0 ) {
		n = knl_oj_put(t, n, max, "MAKER\t");
		n = knl_oj_put(t, n, max, oi.maker);
		n = knl_oj_put(t, n, max, "\nMODEL\t");
		n = knl_oj_put(t, n, max, oi.model);
		n = knl_oj_put(t, n, max, "\n");
		n = put_line(t, n, max, "PRODUCT", oi.product);
	}
	n = knl_oj_put(t, n, max, "IDENT\t");
	n = put_hex(t, n, max, oi.ident);
	n = knl_oj_put(t, n, max, "\n");
	n = put_line(t, n, max, "MM_W", oi.mm_w);
	n = put_line(t, n, max, "MM_H", oi.mm_h);
	n = put_line(t, n, max, "HDMI", oi.hdmi ? 1 : 0);
	n = put_line(t, n, max, "AUDIO", oi.audio ? 1 : 0);
	n = put_line(t, n, max, "EDID", oi.edid_len);
	n = knl_oj_put(t, n, max, "MODES\t");
	n = put_out_modes(t, n, max, i);
	if ( n < -1 ) {
		return n;
	}
	return knl_oj_put(t, n, max, "\n");
}

LOCAL INT out_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	n = knl_oj_put(t, n, max, "<p>An output the screen is shown on. Record 1: what it is, as key and value "
			  "lines (NAME, KIND, STATUS, SCREEN, SIZE, REFRESH, MAKER, MODEL, IDENT, MM_W, MM_H, "
			  "HDMI, AUDIO, EDID, MODES). Record 2: its EDID as the display gave it. It is set "
			  "through the screen's record 1 (OUTi lines).</p>");
	return knl_obdev_link(t, n, max, &ob_uuid_display, (CONST UB *)"画面");
}

LOCAL INT out_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(j, n, max, ",\"virtual\":false");
}

LOCAL ER out_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	INT	i = (INT)(UBINT)ctx;

	(void)pos;
	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	if ( recno == 2 ) {
		UB	*e;
		INT	n = out_edid_take(i, &e);
		SZ	k = 0;

		if ( n < 0 ) return n;
		if ( off < n ) {
			k = (SZ)( n - off );
			if ( k > size ) k = size;
			knl_memcpy(buf, e + off, (INT)k);
		}
		if ( e != NULL ) Kfree(e);
		if ( p_asize != NULL ) *p_asize = k;
		return E_OK;
	}
	if ( recno != 1 ) {
		return E_NOEXS;
	}
	return knl_oj_make_rea(out_make, ctx, off, buf, size, p_asize);
}

LOCAL ER out_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	(void)ctx; (void)pos; (void)recno; (void)off; (void)buf; (void)size; (void)p_asize;
	return E_RONLY;
}

LOCAL UD out_size( void *ctx, INT recno )
{
	INT	n;

	if ( recno == 2 ) {
		n = ts_disp_out_edid((INT)(UBINT)ctx, NULL, 0);
		return ( n > 0 ) ? (UD)n : 0;
	}
	return knl_oj_make_len(out_make, ctx);
}

LOCAL CONST T_OBDVOPS out_ops = {
	OB_S_DISPLAY, "display output", 3, 0444, 0, 0,
	out_text, out_attr, out_rea, out_wri, out_size, NULL
};

IMPORT BOOL wm_dev_conf_get( CONST char *key, char *v, INT max ) __attribute__((weak));
LOCAL BOOL parse_size( CONST UB *b, SZ size, UINT *p_w, UINT *p_h );

/*
 * A display plugged into an output that shows no screen turned on: as
 * the machine's settings keep it for that display (DISPLAY.<ident>, "on
 * 1024x768 right 0", the side right, below, left or above, or "off"),
 * else in the first of its modes, the
 * display's preferred first, that the output takes, right of the main
 * screen. One taken out of an output that shows a screen turned off: its
 * windows come back into the view.
 */
LOCAL void out_policy_on( INT i, CONST T_DISPOUTINFO *pi, T_DISPSTATE *pst );

LOCAL void out_policy( INT i )
{
	T_DISPOUTINFO	oi;
	T_DISPSTATE	st;
	ER		er_policy;

	if ( i <= 0 || ts_disp_output(i, &oi) < E_OK ) return;
	er_policy = ts_disp_state(&st);
	if ( er_policy < E_OK ) {
		tm_printf((UB *)"ob: %s: the outputs' state not read (%d)\n", oi.name, (INT)er_policy);
		return;
	}
	if ( i < st.n ) {
		out_policy_on(i, &oi, &st);
	}
	ts_disp_state_free(&st);
}

LOCAL void out_policy_on( INT i, CONST T_DISPOUTINFO *pi, T_DISPSTATE *pst )
{
	T_DISPOUTINFO	oi = *pi;
	T_DISPSTATE	st = *pst;
	T_DISPMODE	*m;
	char		key[24], v[CF_VAL_MAX];
	INT		nm, k;
	ER		er = E_NOSPT;

	if ( oi.status == DISP_ST_OFF && st.out[i].on ) {
		st.out[i].on = FALSE;
		er = ts_disp_commit(&st);
		tm_printf((UB *)"ob: %s taken out: off (%d)\n", oi.name, (INT)er);
		return;
	}
	if ( oi.status != DISP_ST_ON || st.out[i].on || oi.edid_len == 0 ) return;
	st.out[i].on = TRUE;
	st.out[i].side = DISP_SIDE_RIGHT;
	st.out[i].offset = 0;
	tm_sprintf((UB *)key, (CONST UB *)"DISPLAY.%08x", oi.ident);
	if ( wm_dev_conf_get != NULL && wm_dev_conf_get(key, v, sizeof(v)) ) {
		UINT	w = 0, h = 0;
		SZ	j;

		if ( v[0] == 'o' && v[1] == 'f' ) {
			tm_printf((UB *)"ob: %s plugged in: kept off (%s)\n", oi.name, key);
			return;
		}
		for ( j = 0; v[j] != 0 && ( v[j] < '0' || v[j] > '9' ); j++ ) ;
		if ( parse_size((CONST UB *)v + j, (SZ)knl_strlen(v + j), &w, &h) ) {
			st.out[i].w = (UH)w;
			st.out[i].h = (UH)h;
			for ( ; v[j] != 0 && v[j] != ' '; j++ ) ;
			while ( v[j] == ' ' ) j++;
			if ( side_of((UB)v[j]) >= 0 ) st.out[i].side = side_of((UB)v[j]);
			er = ts_disp_commit(&st);
			tm_printf((UB *)"ob: %s plugged in: as kept, %s (%d)\n", oi.name, v, (INT)er);
			if ( er >= E_OK ) return;
			if ( st.out[i].side == DISP_SIDE_MIRROR && ts_disp_mirror_mode(i, &w, &h) >= E_OK ) {
				/* kept mirroring in a size it no longer takes (the main screen's changed): the size mirroring takes now */
				st.out[i].w = (UH)w;
				st.out[i].h = (UH)h;
				er = ts_disp_commit(&st);
				tm_printf((UB *)"ob: %s plugged in: mirroring the main screen in %d x %d (%d)\n", oi.name,
					  (INT)w, (INT)h, (INT)er);
				if ( er >= E_OK ) return;
			}
			st.out[i].side = DISP_SIDE_RIGHT;
		}
	}
	nm = out_modes_take(i, &m);
	if ( nm < 0 ) {
		tm_printf((UB *)"ob: %s plugged in: its modes not read (%d)\n", oi.name, nm);
		return;
	}
	for ( k = 0; k < nm; k++ ) {
		st.out[i].w = m[k].hdisp;
		st.out[i].h = m[k].vdisp;
		if ( ts_disp_check(&st) < E_OK ) continue;
		er = ts_disp_commit(&st);
		tm_printf((UB *)"ob: %s plugged in: %d x %d right of the main screen (%d)\n", oi.name,
			  (INT)m[k].hdisp, (INT)m[k].vdisp, (INT)er);
		if ( er >= E_OK ) break;
	}
	if ( m != NULL ) Kfree(m);
	if ( k == nm ) {
		tm_printf((UB *)"ob: %s plugged in: none of its %d modes could be turned on (%d)\n", oi.name, nm, (INT)er);
	}
}

/* Something plugged into an output or taken out: turned on or off, and told on it and on the screen */
LOCAL void out_seen( INT i )
{
	out_policy(i);
	if ( i >= 0 && i < out_n ) {
		knl_obdev_post(&out_uuid[i], 1, OB_E_CHANGE, NULL);
	}
	knl_obdisp_changed();
}

/* The outputs as objects, where there is more than one */
LOCAL void outs_start( void )
{
	T_DISPOUTINFO	oi;
	INT		i, no = outs_usable();

	knl_disp_out_watch(out_seen);		/* a display plugged in later is turned on (out_policy) */
	if ( no <= 1 ) {
		return;
	}
	out_uuid = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)no);
	if ( out_uuid == NULL ) {
		tm_printf((UB *)"ob: no room for the objects of the %d outputs\n", no);
		return;
	}
	for ( i = 0; i < no; i++ ) {
		if ( ts_disp_output(i, &oi) < E_OK ) break;
		if ( knl_obdev_add((CONST UB *)oi.name, NULL, &out_ops, (void *)(UBINT)i, &out_uuid[i]) < E_OK ) break;
		out_n = i + 1;
	}
	knl_disp_out_watch(out_seen);
}

/* The screen's record 0: a link to each output */
LOCAL INT outs_links( UB *t, INT n, INT max )
{
	T_DISPOUTINFO	oi;
	INT		i;

	for ( i = 0; i < out_n; i++ ) {
		if ( ts_disp_output(i, &oi) < E_OK ) break;
		n = knl_obdev_link(t, n, max, &out_uuid[i], (CONST UB *)oi.name);
	}
	return n;
}

/* Record 1: what the screen is now */
LOCAL INT mode_text( UB *t, INT n, INT max )
{
	T_DISPSPEC	s;
	INT		i, nm;

	if ( ts_disp_ref(&s) < E_OK ) {
		return knl_oj_put(t, n, max, "SCREEN\tnone\n");
	}
	n = knl_oj_put(t, n, max, "SIZE\t");
	/* the main screen's size, which is what a size written sets; the back buffer's after it */
	n = put_size(t, n, max, ( s.main_w > 0 ) ? s.main_w : s.width, ( s.main_h > 0 ) ? s.main_h : s.height);
	n = knl_oj_put(t, n, max, "\n");
	n = put_line(t, n, max, "WIDTH", s.width);
	n = put_line(t, n, max, "HEIGHT", s.height);
	n = put_line(t, n, max, "DEPTH", s.bpp);
	n = put_line(t, n, max, "BPP", s.fb_bpp);
	n = put_line(t, n, max, "PITCH", s.pitch);
	n = knl_oj_put(t, n, max, "DRIVER\t");
	n = knl_oj_put(t, n, max, DRIVER);
	n = knl_oj_put(t, n, max, "\n");
	n = knl_oj_put(t, n, max, "MODES\t");
	{
		UH	(*ml)[2];

		nm = modes_take(&ml);
		if ( nm < 0 ) {
			return nm;
		}
		for ( i = 0; i < nm; i++ ) {
			if ( i > 0 ) n = knl_oj_put(t, n, max, " ");
			n = put_size(t, n, max, ml[i][0], ml[i][1]);
		}
		Kfree(ml);
	}
	n = knl_oj_put(t, n, max, "\n");
#ifdef X64
	i = knl_disp_hw_fit(-1, FALSE);
	if ( i >= 0 ) {
		n = knl_oj_put(t, n, max, ( i == FIT_STRETCH ) ? "FIT\tstretch\n" : "FIT\taspect\n");
	}
#endif
	/* the scale it is drawn at, the view the windows are laid in, and the scales it can take */
	n = put_line(t, n, max, "SCALE", ( s.scale >= 100 ) ? s.scale : 100);
	n = knl_oj_put(t, n, max, "VIEW\t");
	n = put_size(t, n, max, ( s.view_w > 0 ) ? s.view_w : s.width, ( s.view_h > 0 ) ? s.view_h : s.height);
	n = knl_oj_put(t, n, max, "\nSCALES\t");
	{
		CONST UINT	sc[DISP_NSCALE] = DISP_SCALES;
		INT		k, m = 0;

		for ( k = 0; k < DISP_NSCALE; k++ ) {
			if ( disp_view_len(s.width, sc[k]) >= DISP_VIEW_MIN_W
			  && disp_view_len(s.height, sc[k]) >= DISP_VIEW_MIN_H ) {
				if ( m++ > 0 ) n = knl_oj_put(t, n, max, " ");
				n = knl_oj_put_num(t, n, max, sc[k]);
			}
		}
	}
	n = knl_oj_put(t, n, max, "\n");
	/*
	 * more than one screen: how many, and each one's place and size in the
	 * view (SCREEN0 the main one), with the name and the display's ident of
	 * the output that shows it, where one output shows it (not with the
	 * columns of one output, screens=N): what a program tells the screens
	 * apart by and finds one again by when they change
	 */
	nm = ts_disp_screens();
	if ( nm > 1 ) {
		n = put_line(t, n, max, "SCREENS", nm);
		for ( i = 0; i < nm; i++ ) {
			T_DISPOUTINFO	oi;
			INT		x, y, w, h, k;
			char		key[24];

			if ( ts_disp_screen(i, &x, &y, &w, &h) < E_OK ) break;
			(void)tm_sprintf((UB *)key, (CONST UB *)"SCREEN%d", i);
			n = knl_oj_put(t, n, max, key);
			n = knl_oj_put(t, n, max, "\t");
			n = knl_oj_put_num(t, n, max, x);
			n = knl_oj_put(t, n, max, " ");
			n = knl_oj_put_num(t, n, max, y);
			n = knl_oj_put(t, n, max, " ");
			n = put_size(t, n, max, (UINT)w, (UINT)h);
			for ( k = 0; ts_disp_output(k, &oi) >= E_OK; k++ ) {
				if ( oi.screen != i || ts_disp_outputs() <= 1 ) continue;
				n = knl_oj_put(t, n, max, " ");
				n = knl_oj_put(t, n, max, oi.name);
				n = knl_oj_put(t, n, max, " ");
				n = put_hex(t, n, max, oi.ident);
				break;
			}
			n = knl_oj_put(t, n, max, "\n");
		}
	}
	return outs_text(t, n, max);
}

/* "150%": a scale written with the size or alone; 0 when none is */
LOCAL UINT parse_scale( CONST UB *b, SZ size )
{
	SZ	i, j;

	for ( i = 0; i < size; i++ ) {
		if ( b[i] != '%' ) continue;
		for ( j = i; j > 0 && b[j - 1] >= '0' && b[j - 1] <= '9'; j-- ) ;
		if ( j < i ) {
			UINT	v = 0;

			for ( ; j < i; j++ ) v = v * 10 + (UINT)( b[j] - '0' );
			return v;
		}
	}
	return 0;
}

LOCAL INT disp_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	n = knl_oj_put(t, n, max, "<p>Record 1: the mode, as key and value lines (SIZE, WIDTH, HEIGHT, "
			  "DEPTH, BPP, PITCH, DRIVER, MODES, SCALE, VIEW, SCALES); written WIDTHxHEIGHT, "
			  "the screen is set to that size where the hardware can, and written with a scale "
			  "(150%), alone or after the size, it is drawn at that scale. Record 2: the pixels, rows of PITCH "
			  "bytes, four bytes a pixel (XRGB8888), read with a key that may control the "
			  "device. With more than one output, a line for each (OUTi, OUTMODESi); written "
			  "with OUTi lines (OUT1 on 1024x768 right 0, OUT1 off) the outputs are set to them "
			  "together.</p>");
	n = outs_links(t, n, max);
	return knl_obdev_link(t, n, max, &ob_uuid_system, (CONST UB *)"system");
}

LOCAL INT disp_attr( void *ctx, UB *j, INT n, INT max )
{
	T_DISPSPEC	s;
	INT		i, nm;

	(void)ctx;
	n = knl_oj_put(j, n, max, ",\"virtual\":false");
	if ( ts_disp_ref(&s) < E_OK ) {
		return n;
	}
	n = knl_oj_put(j, n, max, ",\"width\":");
	n = knl_oj_put_num(j, n, max, s.width);
	n = knl_oj_put(j, n, max, ",\"height\":");
	n = knl_oj_put_num(j, n, max, s.height);
	n = knl_oj_put(j, n, max, ",\"depth\":");
	n = knl_oj_put_num(j, n, max, s.bpp);
	n = knl_oj_put(j, n, max, ",\"driver\":\"");
	n = knl_oj_put(j, n, max, DRIVER);
	n = knl_oj_put(j, n, max, "\",\"modes\":[");
	{
		UH	(*ml)[2];

		nm = modes_take(&ml);
		if ( nm < 0 ) {
			return nm;
		}
		for ( i = 0; i < nm; i++ ) {
			n = knl_oj_put(j, n, max, ( i > 0 ) ? ",\"" : "\"");
			n = put_size(j, n, max, ml[i][0], ml[i][1]);
			n = knl_oj_put(j, n, max, "\"");
		}
		Kfree(ml);
	}
	return knl_oj_put(j, n, max, "]");
}

LOCAL UD pixels_size( void )
{
	T_DISPSPEC	s;

	return ( ts_disp_ref(&s) >= E_OK ) ? (UD)s.pitch * s.height : 0;
}

/* Record 1, made as long as the sizes and scales it lists need */
LOCAL INT mode_make( void *ctx, UB *t, INT max )
{
	(void)ctx;
	return mode_text(t, 0, max);
}

LOCAL ER disp_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	SZ	n = 0;

	(void)ctx;
	(void)pos;
	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	if ( recno == OB_DSP_PIXELS ) {
		UB	*back = (UB *)ts_disp_buffer();
		UD	all = pixels_size();

		if ( back == NULL || all == 0 ) {
			return E_NOEXS;
		}
		if ( (UD)off < all ) {
			n = (SZ)( all - (UD)off );
			if ( n > size ) n = size;
			knl_memcpy(buf, back + off, (INT)n);
		}
		if ( p_asize != NULL ) *p_asize = n;
		return E_OK;
	}
	if ( recno != OB_DSP_MODE ) {
		return E_NOEXS;
	}
	return knl_oj_make_rea(mode_make, NULL, off, buf, size, p_asize);
}

/* "1280x800", ended by anything that is not a digit after the height */
LOCAL BOOL parse_size( CONST UB *b, SZ size, UINT *p_w, UINT *p_h )
{
	UINT	w = 0, h = 0;
	SZ	i = 0;

	while ( i < size && ( b[i] == ' ' || b[i] == '\t' ) ) i++;
	if ( i >= size || b[i] < '0' || b[i] > '9' ) return FALSE;
	while ( i < size && b[i] >= '0' && b[i] <= '9' ) w = w * 10 + ( b[i++] - '0' );
	if ( i >= size || ( b[i] != 'x' && b[i] != 'X' ) ) return FALSE;
	i++;
	if ( i >= size || b[i] < '0' || b[i] > '9' ) return FALSE;
	while ( i < size && b[i] >= '0' && b[i] <= '9' ) h = h * 10 + ( b[i++] - '0' );
	*p_w = w;
	*p_h = h;
	return (BOOL)( w > 0 && h > 0 );
}

LOCAL ER disp_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	UINT	w, h;
	ER	er;

	(void)ctx;
	(void)pos;
	if ( recno == OB_DSP_PIXELS ) {
		return E_RONLY;
	}
	if ( recno != OB_DSP_MODE ) {
		return E_NOEXS;
	}
	if ( off != 0 ) {
		return E_PAR;			/* what is asked is written whole */
	}
	if ( size >= 3 && ((CONST UB *)buf)[0] == 'O' && ((CONST UB *)buf)[1] == 'U' && ((CONST UB *)buf)[2] == 'T' ) {
		er = outs_write((CONST UB *)buf, size);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
		return er;
	}
	if ( !parse_size((CONST UB *)buf, size, &w, &h) ) {
		UINT	sc = parse_scale((CONST UB *)buf, size);

		if ( sc == 0 ) {
			return E_PAR;
		}
		er = ts_disp_setscale(sc);	/* the scale alone */
		if ( er < E_OK ) {
			return er;
		}
		knl_obdisp_changed();
		if ( p_asize != NULL ) *p_asize = size;
		return E_OK;
	}
#ifdef X64
	/*
	 * "WIDTHxHEIGHT aspect" or "... stretch": how it is laid on the
	 * panel, taken with the new size, or laid now when the size stays
	 */
	{
		CONST UB	*b = (CONST UB *)buf;
		T_DISPSPEC	cur;
		BOOL		now = (BOOL)( ts_disp_ref(&cur) >= E_OK && cur.main_w == w && cur.main_h == h );
		SZ		i;

		for ( i = 0; i + 6 <= size; i++ ) {
			if ( word_at(b + i, "aspect") ) (void)knl_disp_hw_fit(FIT_ASPECT, now);
			if ( i + 7 <= size && word_at(b + i, "stretch") ) (void)knl_disp_hw_fit(FIT_STRETCH, now);
		}
	}
#endif
	er = ts_disp_setmode(w, h);
	if ( er < E_OK ) {
		return er;
	}
	{
		UINT	sc = parse_scale((CONST UB *)buf, size);

		/* a scale written with the size, taken at the new size */
		if ( sc != 0 ) {
			ER	es = ts_disp_setscale(sc);

			if ( es < E_OK ) {
				knl_obdisp_changed();
				return es;
			}
		}
	}
	knl_obdisp_changed();
	if ( p_asize != NULL ) *p_asize = size;
	return E_OK;
}

LOCAL UD disp_size( void *ctx, INT recno )
{
	(void)ctx;
	if ( recno == OB_DSP_PIXELS ) {
		return pixels_size();
	}
	return knl_oj_make_len(mode_make, NULL);
}

LOCAL CONST T_OBDVOPS disp_ops = {
	OB_S_DISPLAY, "display", OB_DSP_NREC, 0664, OB_DSP_PIXELS, OB_OP_READ,
	disp_text, disp_attr, disp_rea, disp_wri, disp_size, NULL
};

LOCAL BOOL	disp_there = FALSE;

/*
 * The screen came to another size: told on the system object's record
 * 3, the view of it (the write to record 1 is told on this object by
 * the name manager)
 */
EXPORT void knl_obdisp_changed( void )
{
	knl_obsys_changed(OB_SYS_DISPLAY);
}

/* The system object's record 3: this object named, and its record 1 */
EXPORT INT knl_obdisp_text( UB *t, INT max )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n = 0;

	if ( !disp_there ) {
		return knl_oj_put(t, 0, max, "SCREEN\tnone\n");
	}
	(void)ts_uuid_to_str(&ob_uuid_display, us, sizeof(us));
	n = knl_oj_put(t, n, max, "OBJECT\t");
	n = knl_oj_put(t, n, max, us);
	n = knl_oj_put(t, n, max, "\n");
	return mode_text(t, n, max);
}

EXPORT void knl_obdisp_start( void )
{
	T_DISPSPEC	s;
	ER		er;

	if ( ts_disp_ref(&s) < E_OK ) {
		return;				/* no screen, no object */
	}
	er = knl_obdev_add((CONST UB *)"画面", &ob_uuid_display, &disp_ops, NULL, NULL);
	if ( er < E_OK ) {
		tm_printf((UB *)"ob: no screen object (%d)\n", (INT)er);
		return;
	}
	disp_there = TRUE;
	outs_start();
}
