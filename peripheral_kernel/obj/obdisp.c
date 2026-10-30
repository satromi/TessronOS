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

#ifdef X64
LOCAL UH	pc_modes[24][2];

/* The sizes offered, into pc_modes: how many */
LOCAL INT modes_now( void )
{
	return knl_disp_hw_modes(pc_modes, 24);
}
#define NMODES		modes_now()
#define MODE_W(i)	pc_modes[i][0]
#define MODE_H(i)	pc_modes[i][1]
#else
#define NMODES		(INT)( sizeof(modes) / sizeof(modes[0]) )
#define MODE_W(i)	modes[i][0]
#define MODE_H(i)	modes[i][1]
#endif

/* Record 1: what the screen is now */
LOCAL INT mode_text( UB *t, INT n, INT max )
{
	T_DISPSPEC	s;
	INT		i, nm;

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
	n = knl_oj_put(t, n, max, "DRIVER\t");
	n = knl_oj_put(t, n, max, DRIVER);
	n = knl_oj_put(t, n, max, "\n");
	n = knl_oj_put(t, n, max, "MODES\t");
	nm = NMODES;
	for ( i = 0; i < nm; i++ ) {
		if ( i > 0 ) n = knl_oj_put(t, n, max, " ");
		n = put_size(t, n, max, MODE_W(i), MODE_H(i));
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
	return knl_oj_put(t, n, max, "\n");
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
			  "device.</p>");
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
	nm = NMODES;
	for ( i = 0; i < nm; i++ ) {
		n = knl_oj_put(j, n, max, ( i > 0 ) ? ",\"" : "\"");
		n = put_size(j, n, max, MODE_W(i), MODE_H(i));
		n = knl_oj_put(j, n, max, "\"");
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
		BOOL		now = (BOOL)( ts_disp_ref(&cur) >= E_OK && cur.width == w && cur.height == h );
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
}
