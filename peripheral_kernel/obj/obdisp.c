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
#include <ts/conf.h>
#include "obj.h"

#define MODE_TEXT_MAX	1024

#ifdef RPI5
#define DRIVER		"firmware framebuffer (HDMI)"
#else
#define DRIVER		"bochs-display"
#endif

/* The sizes it can be set to */
LOCAL CONST UH	modes[][2] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
	{ 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 1920, 1200 }
};

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

/* Record 1: what the screen is now */
LOCAL INT mode_text( UB *t, INT n, INT max )
{
	T_DISPSPEC	s;
	INT		i;

	if ( ts_disp_ref(&s) < E_OK ) {
		return knl_oj_put(t, n, max, "SCREEN\tnone\n");
	}
	n = knl_oj_put(t, n, max, "SIZE\t");
	n = put_size(t, n, max, s.width, s.height);
	n = knl_oj_put(t, n, max, "\n");
	n = put_line(t, n, max, "WIDTH", s.width);
	n = put_line(t, n, max, "HEIGHT", s.height);
	n = put_line(t, n, max, "DEPTH", s.bpp);
	n = put_line(t, n, max, "BPP", s.fb_bpp);
	n = put_line(t, n, max, "PITCH", s.pitch);
	n = knl_oj_put(t, n, max, "DRIVER\t" DRIVER "\n");
	n = knl_oj_put(t, n, max, "MODES\t");
	for ( i = 0; i < (INT)( sizeof(modes) / sizeof(modes[0]) ); i++ ) {
		if ( i > 0 ) n = knl_oj_put(t, n, max, " ");
		n = put_size(t, n, max, modes[i][0], modes[i][1]);
	}
	return knl_oj_put(t, n, max, "\n");
}

LOCAL INT disp_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	n = knl_oj_put(t, n, max, "<p>Record 1: the mode, as key and value lines (SIZE, WIDTH, HEIGHT, "
			  "DEPTH, BPP, PITCH, DRIVER, MODES); written WIDTHxHEIGHT, the screen is set "
			  "to that size where the hardware can. Record 2: the pixels, rows of PITCH "
			  "bytes, four bytes a pixel (XRGB8888), read with a key that may control the "
			  "device.</p>");
	return knl_obdev_link(t, n, max, &ob_uuid_system, (CONST UB *)"system");
}

LOCAL INT disp_attr( void *ctx, UB *j, INT n, INT max )
{
	T_DISPSPEC	s;
	INT		i;

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
	n = knl_oj_put(j, n, max, ",\"driver\":\"" DRIVER "\",\"modes\":[");
	for ( i = 0; i < (INT)( sizeof(modes) / sizeof(modes[0]) ); i++ ) {
		n = knl_oj_put(j, n, max, ( i > 0 ) ? ",\"" : "\"");
		n = put_size(j, n, max, modes[i][0], modes[i][1]);
		n = knl_oj_put(j, n, max, "\"");
	}
	return knl_oj_put(j, n, max, "]");
}

LOCAL UD pixels_size( void )
{
	T_DISPSPEC	s;

	return ( ts_disp_ref(&s) >= E_OK ) ? (UD)s.pitch * s.height : 0;
}

LOCAL ER disp_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	*t;
	INT	len;
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
	t = (UB *)Kmalloc(MODE_TEXT_MAX);
	if ( t == NULL ) {
		return E_NOMEM;
	}
	len = mode_text(t, 0, MODE_TEXT_MAX);
	if ( len >= 0 && off < len ) {
		n = (SZ)( len - off );
		if ( n > size ) n = size;
		knl_memcpy(buf, t + off, (INT)n);
	}
	Kfree(t);
	if ( p_asize != NULL ) *p_asize = n;
	return ( len < 0 ) ? E_SYS : E_OK;
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
	if ( !parse_size((CONST UB *)buf, size, &w, &h) ) {
		return E_PAR;
	}
	er = ts_disp_setmode(w, h);
	if ( er < E_OK ) {
		return er;
	}
	knl_obdisp_changed();
	if ( p_asize != NULL ) *p_asize = size;
	return E_OK;
}

LOCAL UD disp_size( void *ctx, INT recno )
{
	UB	*t;
	INT	len;

	(void)ctx;
	if ( recno == OB_DSP_PIXELS ) {
		return pixels_size();
	}
	t = (UB *)Kmalloc(MODE_TEXT_MAX);
	if ( t == NULL ) {
		return 0;
	}
	len = mode_text(t, 0, MODE_TEXT_MAX);
	Kfree(t);
	return ( len > 0 ) ? (UD)len : 0;
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
}
