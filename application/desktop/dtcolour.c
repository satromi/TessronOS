/*
 * dtcolour.c - choosing a colour
 *
 * Every place of the system that asks for a colour shows the colour as
 * a part that is pressed (dt_pn_colour) rather than a box to type a
 * code into. Pressed, the pop-up here opens under it. Its inputs follow
 * each other: the plane of saturation and brightness with its bar of
 * hues, the dropper that takes a colour off the screen, the boxes of the
 * numbers in the form chosen (RGB, HSL or HEX), and the chart of ready
 * colours. A press outside it or Enter keeps the colour; Escape leaves
 * the caller's colour as it was.
 *
 * A program of its own process asks with DT_RQ_COLOUR (ts/dtreq.h), and
 * the pop-up opens in the desktop's task over the program's window.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include "desktop.h"

#define KEY_ENTER	0x28
#define KEY_ESC		0x29

/* The parts */
#define CP_MAP		1
#define CP_HUE		2
#define CP_SHOWN	3
#define CP_DROPPER	4
#define CP_N1		5		/* the three boxes of RGB or HSL */
#define CP_HEX		8		/* the one box of HEX */
#define CP_L1		9		/* the words under them */
#define CP_LHEX		12
#define CP_FORM		13		/* RGB, HSL, HEX: three choices */
#define CP_CHART	20

#define FORM_RGB	0
#define FORM_HSL	1
#define FORM_HEX	2

#define CHART_COLS	8
#define CHART_ROWS	6
#define CHART_N		( CHART_COLS * CHART_ROWS )

/* The pop-up and where its parts stand on it */
#define POP_W		232
#define MAP_H		137
#define ROW_Y		143		/* the dropper, the colour shown and the hue bar */
#define BOX_Y		183
#define BOX_W		52
#define BOX_X(i)	( 24 + (i) * 59 )
#define LABEL_Y		212
#define FORM_Y		234
#define CHART_Y		264
#define CHART_DX	27
#define CHART_DY	15
#define POP_H		( CHART_Y + CHART_ROWS * CHART_DY + 6 )

#define ANS_DROPPER	WM_ANS_ACT

LOCAL UW hsv_rgb( INT h, INT s, INT v )
{
	INT	f = h % 60;
	INT	p = v * ( 255 - s ) / 255;
	INT	q = v * ( 255 - s * f / 60 ) / 255;
	INT	t = v * ( 255 - s * ( 60 - f ) / 60 ) / 255;
	INT	r, g, b;

	switch ( h / 60 ) {
	case 0:		r = v; g = t; b = p;	break;
	case 1:		r = q; g = v; b = p;	break;
	case 2:		r = p; g = v; b = t;	break;
	case 3:		r = p; g = q; b = v;	break;
	case 4:		r = t; g = p; b = v;	break;
	default:	r = v; g = p; b = q;	break;
	}

	return ( (UW)r << 16 ) | ( (UW)g << 8 ) | (UW)b;
}

/* A grey has no hue and black no saturation: those keep what they had, so the marks stay put */
LOCAL void rgb_hsv( UW c, INT *h, INT *s, INT *v )
{
	INT	r = (INT)( ( c >> 16 ) & 0xFF ), g = (INT)( ( c >> 8 ) & 0xFF ), b = (INT)( c & 0xFF );
	INT	mx = r, mn = r, d;

	if ( g > mx ) mx = g;
	if ( b > mx ) mx = b;
	if ( g < mn ) mn = g;
	if ( b < mn ) mn = b;
	d = mx - mn;
	*v = mx;
	if ( mx == 0 ) {
		return;
	}
	*s = d * 255 / mx;
	if ( d == 0 ) {
		return;
	}
	if ( mx == r ) {
		*h = ( 360 + 60 * ( g - b ) / d ) % 360;
	} else if ( mx == g ) {
		*h = 120 + 60 * ( b - r ) / d;
	} else {
		*h = 240 + 60 * ( r - g ) / d;
	}
}

/* Saturation and lightness in hundredths, as HSL says them */
LOCAL void rgb_sl( UW c, INT *s, INT *l )
{
	INT	r = (INT)( ( c >> 16 ) & 0xFF ), g = (INT)( ( c >> 8 ) & 0xFF ), b = (INT)( c & 0xFF );
	INT	mx = r, mn = r, sum;

	if ( g > mx ) mx = g;
	if ( b > mx ) mx = b;
	if ( g < mn ) mn = g;
	if ( b < mn ) mn = b;
	sum = mx + mn;
	*l = ( sum * 100 + 255 ) / 510;
	if ( mx == mn ) {
		*s = 0;
	} else if ( sum <= 255 ) {
		*s = ( ( mx - mn ) * 200 + sum ) / ( 2 * sum );
	} else {
		*s = ( ( mx - mn ) * 200 + 510 - sum ) / ( 2 * ( 510 - sum ) );
	}
}

/* HSL with saturation and lightness in hundredths, as a colour */
LOCAL UW hsl_rgb( INT h, INT s, INT l )
{
	INT	m = ( l < 100 - l ) ? l : 100 - l;
	INT	v = l * 100 + s * m;			/* in ten-thousandths */

	if ( v == 0 ) {
		return 0;
	}

	return hsv_rgb(h, 2 * ( v - l * 100 ) * 255 / v, ( v * 255 + 5000 ) / 10000);
}

/* The chart: a row of greys, then eight hues from pale through pure to dark */
LOCAL UW chart_colour( INT k )
{
	LOCAL CONST UW	greys[CHART_COLS] = {
		0x000000U, 0x333333U, 0x555555U, 0x808080U,
		0xA0A0A0U, 0xC0C0C0U, 0xE0E0E0U, 0xFFFFFFU
	};
	LOCAL CONST INT	hues[CHART_COLS] = { 0, 30, 60, 120, 180, 240, 270, 300 };
	LOCAL CONST INT	sat[CHART_ROWS - 1] = { 64, 128, 255, 255, 255 };
	LOCAL CONST INT	val[CHART_ROWS - 1] = { 255, 255, 255, 192, 128 };
	INT		col = k % CHART_COLS, row = k / CHART_COLS;

	if ( row == 0 ) {
		return greys[col];
	}

	return hsv_rgb(hues[col], sat[row - 1], val[row - 1]);
}

/* A part placed by the pop-up's own corner: it has no margin */
LOCAL T_WMPART *add( T_WMPANEL *def, UW type, INT num, INT l, INT t, INT w, INT h, CONST char *label )
{
	return dt_pn_add(def, type, num, l - DT_PN_IN, t - DT_PN_IN, w, h, label);
}

LOCAL T_WMPANEL *panel_def( UW col )
{
	LOCAL CONST char * CONST forms[3] = { "RGB", "HSL", "HEX" };
	T_WMPANEL	*def = dt_pn_new(POP_W - 2 * DT_PN_IN, POP_H - 2 * DT_PN_IN);
	T_WMPART	*pt;
	INT		k;

	if ( def == NULL ) {
		return NULL;
	}
	def->kind = WM_PNL_SHEET;
	(void)add(def, WM_PT_SVMAP, CP_MAP, 0, 0, POP_W, MAP_H, NULL);
	pt = add(def, WM_PT_DROPPER, CP_DROPPER, 8, ROW_Y + 6, 22, 22, NULL);
	if ( pt != NULL ) {
		pt->answer = ANS_DROPPER;
	}
	pt = add(def, WM_PT_COLDOT, CP_SHOWN, 42, ROW_Y + 1, 32, 32, NULL);
	if ( pt != NULL ) {
		pt->colour = col;
	}
	(void)add(def, WM_PT_HUEBAR, CP_HUE, 80, ROW_Y + 8, 146, 18, NULL);
	for ( k = 0; k < 3; k++ ) {
		pt = add(def, WM_PT_BOX | P_DIGITS, CP_N1 + k, BOX_X(k), BOX_Y, BOX_W, 26, NULL);
		if ( pt != NULL ) {
			pt->max = 3;
		}
		(void)add(def, WM_PT_LABEL, CP_L1 + k, BOX_X(k) + 20, LABEL_Y, 30, 20, NULL);
	}
	pt = add(def, WM_PT_BOX | P_ASCII | P_GONE, CP_HEX, BOX_X(0), BOX_Y, BOX_X(2) + BOX_W - BOX_X(0), 26, NULL);
	if ( pt != NULL ) {
		pt->max = 7;
	}
	(void)add(def, WM_PT_LABEL | P_GONE, CP_LHEX, BOX_X(1) + 10, LABEL_Y, 40, 20, "HEX");
	for ( k = 0; k < 3; k++ ) {
		pt = add(def, WM_PT_CHOICE, CP_FORM + k, 10 + k * 71, FORM_Y, 70, 22, forms[k]);
		if ( pt != NULL ) {
			pt->group = 2;
			pt->value = ( k == FORM_RGB ) ? 1 : 0;
		}
	}
	for ( k = 0; k < CHART_N; k++ ) {
		pt = add(def, WM_PT_SWATCH, CP_CHART + k,
			  8 + ( k % CHART_COLS ) * CHART_DX, CHART_Y + ( k / CHART_COLS ) * CHART_DY,
			  CHART_DX - 1, CHART_DY - 1, NULL);
		if ( pt != NULL ) {
			pt->colour = chart_colour(k);
			pt->group = 1;
		}
	}

	return def;
}

/* A number's words */
LOCAL void num_text( INT n, UB *out )
{
	UB	rev[8];
	INT	i = 0, k = 0;

	do {
		rev[i++] = (UB)( '0' + n % 10 );
		n /= 10;
	} while ( n > 0 && i < 7 );
	while ( i > 0 ) {
		out[k++] = rev[--i];
	}
	out[k] = 0;
}

/* The boxes from the colour, all but the ones it was typed into */
LOCAL void boxes_show( INT pid, INT form, BOOL typed, UW col, INT h )
{
	UB	t[8];
	INT	n[3], s, l, k;

	if ( !typed || form != FORM_HEX ) {
		dt_colour_text(col, t);
		(void)wm_panel_set_text(pid, CP_HEX, t);
	}
	if ( typed && form != FORM_HEX ) {
		return;
	}
	if ( form == FORM_HSL ) {
		rgb_sl(col, &s, &l);
		n[0] = h;  n[1] = s;  n[2] = l;
	} else {
		n[0] = (INT)( ( col >> 16 ) & 0xFF );
		n[1] = (INT)( ( col >> 8 ) & 0xFF );
		n[2] = (INT)( col & 0xFF );
	}
	for ( k = 0; k < 3; k++ ) {
		num_text(n[k], t);
		(void)wm_panel_set_text(pid, CP_N1 + k, t);
	}
}

/* Every part shown again from the colour */
LOCAL void panel_show( INT pid, INT form, BOOL typed, UW col, INT h, INT s, INT v )
{
	INT	k;

	(void)wm_panel_set(pid, CP_MAP, WM_SVMAP_VALUE(h, s, v));
	(void)wm_panel_set(pid, CP_HUE, h);
	(void)wm_panel_set_colour(pid, CP_SHOWN, col);
	for ( k = 0; k < CHART_N; k++ ) {
		(void)wm_panel_set(pid, CP_CHART + k, ( chart_colour(k) == col ) ? 1 : 0);
	}
	boxes_show(pid, form, typed, col, h);
	wm_panel_draw(pid);
}

/* The boxes and the words under them for a form */
LOCAL void form_show( INT pid, INT form )
{
	LOCAL CONST char * CONST names[2][3] = { { "R", "G", "B" }, { "H", "S", "L" } };
	INT	k;

	(void)wm_panel_show(pid, CP_HEX, (BOOL)( form == FORM_HEX ));
	(void)wm_panel_show(pid, CP_LHEX, (BOOL)( form == FORM_HEX ));
	for ( k = 0; k < 3; k++ ) {
		(void)wm_panel_show(pid, CP_N1 + k, (BOOL)( form != FORM_HEX ));
		(void)wm_panel_show(pid, CP_L1 + k, (BOOL)( form != FORM_HEX ));
		if ( form != FORM_HEX ) {
			(void)wm_panel_set_text(pid, CP_L1 + k, (CONST UB *)names[form][k]);
		}
	}
}

/* The boxes of the form read: FALSE while they do not yet say a colour */
LOCAL BOOL boxes_read( INT pid, INT form, UW *p_col, INT *p_h )
{
	UB	t[WM_LABEL_MAX];
	INT	n[3], k, i;

	if ( form == FORM_HEX ) {
		return (BOOL)( wm_panel_text(pid, CP_HEX, t, sizeof(t)) >= E_OK && dt_colour_of(t, p_col) );
	}
	for ( k = 0; k < 3; k++ ) {
		if ( wm_panel_text(pid, CP_N1 + k, t, sizeof(t)) < E_OK || t[0] == 0 ) {
			return FALSE;
		}
		n[k] = 0;
		for ( i = 0; t[i] != 0; i++ ) {
			n[k] = n[k] * 10 + ( t[i] - '0' );
		}
	}
	if ( form == FORM_RGB ) {
		if ( n[0] > 255 || n[1] > 255 || n[2] > 255 ) {
			return FALSE;
		}
		*p_col = ( (UW)n[0] << 16 ) | ( (UW)n[1] << 8 ) | (UW)n[2];
		return TRUE;
	}
	if ( n[0] > 359 || n[1] > 100 || n[2] > 100 ) {
		return FALSE;
	}
	*p_col = hsl_rgb(n[0], n[1], n[2]);
	*p_h = n[0];			/* what was typed, though a grey has no hue */

	return TRUE;
}

/* The dropper: the colour under the next press anywhere on the screen. FALSE on Escape */
LOCAL BOOL drop_take( UW *p_col )
{
	T_WMEV		ev;
	T_DPRECT	r;
	UINT		was = wm_pointer_kind(), btn;
	INT		x = 0, y = 0;
	UW		px = 0;
	BOOL		got = FALSE;

	(void)wm_set_pointer(WM_PT_PICK);
	wm_update();
	while ( !dt_stopping() ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ESC ) {
			break;
		}
		if ( ev.type == HID_EV_BTN_DOWN && ts_hid_pointer(&x, &y, &btn) >= E_OK ) {
			r.left = x;  r.top = y;  r.right = x + 1;  r.bottom = y + 1;
			if ( wm_grab_screen(&px, 1, &r, FALSE) >= E_OK ) {
				*p_col = px & 0x00FFFFFFU;
				got = TRUE;
			}
			break;
		}
	}
	(void)wm_set_pointer(was);
	wm_update();

	return got;
}

/*
 * The pop-up with its corner at (x, y) of the screen, or as near as the
 * screen lets it be, worked until it ends. TRUE with the colour in *io
 * when it was kept. 'owner' is the window whose keys are the pop-up's:
 * it holds them throughout, and they go back to whichever window held
 * them before, the panel that asked or a program's own window.
 */
LOCAL BOOL pick_at( INT owner, UW *io, INT x, INT y )
{
	T_WMPANEL	*def;
	T_WMEV		ev;
	UINT		ans = WM_ANS_NONE;
	UW		col = *io & 0x00FFFFFFU, c;
	INT		pid, wid, was = wm_focused(), form = FORM_RGB;
	INT		h = 0, s = 0, v = 0, k, n, hh;

	def = panel_def(col);
	if ( def == NULL ) {
		return FALSE;
	}
	pid = wm_panel_open_at(owner, def, x, y);
	Kfree(def);
	if ( pid < 0 ) {
		return FALSE;
	}
	wid = wm_panel_wid(pid);
	wm_focus(owner);
	rgb_hsv(col, &h, &s, &v);
	form_show(pid, form);
	panel_show(pid, form, FALSE, col, h, s, v);
	wm_update();

	while ( ans == WM_ANS_NONE ) {
		if ( dt_stopping() ) {
			ans = WM_ANS_CANCEL;
			break;
		}
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ENTER ) {
			ans = WM_ANS_OK;
			break;
		}
		if ( ev.type == HID_EV_BTN_DOWN && ev.wid != wid ) {
			ans = WM_ANS_OK;	/* pressed elsewhere: put away with what it shows */
			break;
		}
		if ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) {
			ev.wid = owner;		/* the letters are the pop-up's */
		}
		if ( wm_panel_event(pid, &ev, &ans) < E_OK ) {
			ans = WM_ANS_CANCEL;
		}
		if ( ans == ANS_DROPPER ) {
			ans = WM_ANS_NONE;
			c = col;
			if ( drop_take(&c) ) {
				col = c;
				rgb_hsv(col, &h, &s, &v);
				panel_show(pid, form, FALSE, col, h, s, v);
			}
			wm_update();
			continue;
		}

		/* What this event moved is what the others follow */
		for ( k = 0; k < 3; k++ ) {
			if ( k != form && wm_panel_get(pid, CP_FORM + k, &n) >= E_OK && n != 0 ) {
				form = k;
				form_show(pid, form);
				panel_show(pid, form, FALSE, col, h, s, v);
				break;
			}
		}
		if ( wm_panel_get(pid, CP_HUE, &n) >= E_OK && n != h ) {
			h = n;
			col = hsv_rgb(h, s, v);
			panel_show(pid, form, FALSE, col, h, s, v);
		}
		if ( wm_panel_get(pid, CP_MAP, &n) >= E_OK && n != WM_SVMAP_VALUE(h, s, v) ) {
			s = WM_SVMAP_S(n);
			v = WM_SVMAP_V(n);
			col = hsv_rgb(h, s, v);
			panel_show(pid, form, FALSE, col, h, s, v);
		}
		for ( k = 0; k < CHART_N; k++ ) {
			if ( wm_panel_get(pid, CP_CHART + k, &n) >= E_OK && n != 0 && chart_colour(k) != col ) {
				col = chart_colour(k);
				rgb_hsv(col, &h, &s, &v);
				panel_show(pid, form, FALSE, col, h, s, v);
				break;
			}
		}
		hh = h;
		if ( ev.type == HID_EV_KEY_DOWN && boxes_read(pid, form, &c, &hh) && ( c != col || hh != h ) ) {
			col = c;
			rgb_hsv(col, &h, &s, &v);
			if ( form == FORM_HSL ) {
				h = hh;
			}
			panel_show(pid, form, TRUE, col, h, s, v);
		}
		wm_update();
	}
	wm_panel_close(pid);
	if ( was > 0 ) {
		wm_focus(was);
	}
	wm_update();
	if ( ans != WM_ANS_OK ) {
		return FALSE;
	}
	*io = col;

	return TRUE;
}

/* Asked from a menu: the pop-up at the pointer */
EXPORT BOOL dt_pick_colour( INT owner, UW *io )
{
	INT	x = 0, y = 0;
	UINT	btn;

	(void)ts_hid_pointer(&x, &y, &btn);

	return pick_at(owner, io, x, y + 8);
}

/* Asked with a place: the pop-up with its corner there (a program's part, DT_F_AT) */
EXPORT BOOL dt_pick_colour_at( INT owner, UW *io, INT x, INT y )
{
	return pick_at(owner, io, x, y);
}

/* Asked from a part that shows the colour: the pop-up under it, set against its left */
EXPORT BOOL dt_pick_colour_part( INT owner, INT pid, INT num, UW *io )
{
	T_DPRECT	r;

	if ( wm_panel_part_screen(pid, num, &r) < E_OK ) {
		return dt_pick_colour(owner, io);
	}

	return pick_at(owner, io, r.left, r.bottom + 2);
}
