/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fbcon.c (x64 PC)
 *	The screen console: what goes to the console, drawn on the screen
 *	the UEFI loader set up (Graphics Output Protocol), in white on
 *	black with an 8 x 16 bitmap font (confont.h), drawn as many times
 *	larger as the loader drew its own (twice on a screen 1600 pixels
 *	wide or more), so that a photograph of the screen can be read. It
 *	works from the first line the kernel says, before there is any other
 *	way to draw, and is the console of a machine without a serial port
 *	(the Surface Go 2, docs/private/x64_surface_go2_study.md 2.13).
 *
 *	The top row is a band that says the step of the start-up under way
 *	(knl_fbcon_stage); the console's lines go below it, starting under
 *	the loader's last one. It never scrolls: reading the screen back is
 *	slow, so when the last row is written the next line goes to the top
 *	again, and the row after the one being written is kept clear so that
 *	the newest line can be found. The screen's driver takes the screen
 *	over when it first draws on it or changes its size (knl_fbcon_stop),
 *	not when it is set up: until the desktop draws, the band and the
 *	lines go on telling the steps that follow. The bottom row is kept
 *	for the start-up's watch (knl_fbcon_watch): a step that has stood
 *	five seconds is named there with how long it has stood and how many
 *	processors still take their timer's interrupt, written again every
 *	second from the processors' timers. What stops the kernel brings
 *	the console back (knl_fbcon_resume) on a cleared screen, the band
 *	red and naming the last step reached, and the summary of an
 *	exception kept in yellow under it (knl_fbcon_pin), which the lines
 *	that follow do not write over.
 *
 *	For trying a machine out, the settings (cmdline.txt) may stop the
 *	start-up at a step, its console left on the screen to be read
 *	("stopat=USB": the first step whose name begins so), or make it fault
 *	there ("faultat=USB": an invalid instruction), to see what a stop
 *	looks like.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <sys/sysdef.h>
#include <tm/tmonitor.h>
#include "../cpu/core/x86_64/bootinfo.h"
#include "confont.h"

IMPORT void	(*tm_mirror)( const UB *buf, INT size );
IMPORT void	knl_dev_set_wc( UD pa, UD size );	/* mmu.c */
IMPORT BOOL	knl_dev_mapped( UD pa );
IMPORT void	knl_bootlog_put( const UB *buf, INT size );	/* bootlog.c */
IMPORT UD	knl_bootlog_head( void );
IMPORT void	knl_bootlog_tail( UD until, INT lines, void (*out)( CONST UB *s, INT n ) );
IMPORT UINT	tm_out_lock( void );		/* libtm.c: the console's lock */
IMPORT void	tm_out_unlock( UINT intsts );
IMPORT UD	knl_lockup_now( void );		/* lockup.c: the counter its checks read */

LOCAL _UW	*fb = NULL;		/* the pixels (device window) */
LOCAL UW	fb_w, fb_h, fb_pitch;	/* pixels; pixels from one row to the next */
LOCAL UW	scale = 1, cw = CONFONT_W, ch = CONFONT_H;
LOCAL UW	cols, rows, cx, cy;
LOCAL UW	lines_end;		/* the console's lines go above this row; the watch's is under them */
LOCAL UW	fg = 0x00e0e0e0, bg = 0x00000000, band = 0x0060c0ff, red = 0x00ff5050;
LOCAL BOOL	stopped = FALSE;
LOCAL BOOL	handed = FALSE;		/* the screen's driver has drawn: the start-up's watch is over */
LOCAL UB	stage_now[96] = "start";
LOCAL UD	said_before = 0;	/* where the console was when the machine stopped */

/* The start-up's watch (knl_fbcon_watch) */
#define WATCH_S		5		/* a step standing this many seconds is named on the bottom row */
LOCAL volatile UD	stage_at = 0;	/* the counter (knl_lockup_now) when the step began */
LOCAL volatile UW	watch_busy = 0;	/* the processor writing the watch's row (PRC ID + 1), 0 none */
LOCAL UD		watch_last = 0;	/* the seconds last written */
LOCAL BOOL		watch_shown = FALSE;

/*
 * Lines kept under the band (knl_fbcon_pin): the summary of what stopped
 * the machine. The console's lines go below them and, coming round
 * again, start under them, so that a long report after it (a processor's
 * lockup telling of the others) does not write over them.
 */
#define PIN_MAX		3
#define PIN_LEN		160
LOCAL UB	pin_text[PIN_MAX][PIN_LEN];
LOCAL UW	npin = 0;
LOCAL UW	pin_fg = 0x00ffff60;

LOCAL UW rgb( CONST T_BOOTINFO *bi, UW r, UW g, UW b )
{
	UW	v = 0, s;

	if ( bi->fb.format == BI_FB_BGRX ) return ( r << 16 ) | ( g << 8 ) | b;
	if ( bi->fb.format == BI_FB_RGBX ) return ( b << 16 ) | ( g << 8 ) | r;
	for ( s = 0; s < 32 && ( ( bi->fb.mask_r >> s ) & 1 ) == 0; s++ ) ;
	v |= ( r << s ) & bi->fb.mask_r;
	for ( s = 0; s < 32 && ( ( bi->fb.mask_g >> s ) & 1 ) == 0; s++ ) ;
	v |= ( g << s ) & bi->fb.mask_g;
	for ( s = 0; s < 32 && ( ( bi->fb.mask_b >> s ) & 1 ) == 0; s++ ) ;
	v |= ( b << s ) & bi->fb.mask_b;
	return v;
}

LOCAL void cell( UW col, UW row, UB c, UW f, UW b )
{
	CONST UB	*g;
	UW		y, x;

	if ( c < CONFONT_FIRST || c > CONFONT_LAST ) {
		c = '?';
	}
	g = confont[c - CONFONT_FIRST];
	for ( y = 0; y < ch; y++ ) {
		_UW	*p = fb + (UD)( row * ch + y ) * fb_pitch + col * cw;
		UB	bits = g[y / scale];

		for ( x = 0; x < cw; x++ ) {
			p[x] = ( bits & ( 0x80 >> ( x / scale ) ) ) ? f : b;
		}
	}
}

LOCAL void clear_row( UW r )
{
	UW	y, x;

	for ( y = r * ch; y < ( r + 1 ) * ch && y < fb_h; y++ ) {
		_UW	*p = fb + (UD)y * fb_pitch;

		for ( x = 0; x < cols * cw; x++ ) {
			p[x] = bg;
		}
	}
}

/* The first row of the console's lines: under the band and the lines kept */
LOCAL UW first_row( void )
{
	return ( 1 + npin < lines_end ) ? 1 + npin : 1;
}

LOCAL UW next_row( UW r )
{
	return ( r + 1 < lines_end ) ? r + 1 : first_row();
}

LOCAL void newline( void )
{
	cx = 0;
	cy = next_row(cy);
	clear_row(cy);
	clear_row(next_row(cy));
}

LOCAL void draw( UB c )
{
	cell(cx, cy, c, fg, bg);
	if ( ++cx >= cols ) {
		newline();
	}
}

/* The band at the top: TessronOS, the step, in the colour given */
LOCAL void draw_band( CONST UB *head, UW colour )
{
	UW	i = 0, k;

	for ( k = 0; head[k] != '\0' && i < cols; k++ ) cell(i++, 0, head[k], bg, colour);
	for ( k = 0; stage_now[k] != '\0' && i < cols; k++ ) cell(i++, 0, stage_now[k], bg, colour);
	while ( i < cols ) cell(i++, 0, ' ', bg, colour);
}

/* The lines kept, each on a row of its own under the band */
LOCAL void draw_pins( void )
{
	UW	k, i;

	for ( k = 0; k < npin && 1 + k < lines_end; k++ ) {
		clear_row(1 + k);
		for ( i = 0; pin_text[k][i] != '\0' && i < cols; i++ ) cell(i, 1 + k, pin_text[k][i], pin_fg, bg);
	}
}

LOCAL void fbcon_out( const UB *buf, INT size )
{
	knl_bootlog_put(buf, size);
	if ( stopped || fb == NULL ) {
		return;
	}
	while ( size-- > 0 ) {
		UB	c = *buf++;

		if ( c == '\n' ) {
			newline();
		} else if ( c == '\r' ) {
			cx = 0;
		} else if ( c == '\t' ) {
			do {
				draw(' ');
			} while ( ( cx & 7 ) != 0 && cx != 0 );
		} else if ( c == '\b' ) {
			if ( cx > 0 ) cx--;
		} else {
			draw(c);
		}
	}
}

/*
 * Set up on the screen of the T_BOOTINFO; a screen of another kind than
 * 32 bits a pixel is left alone. The rows of the screen, which is a
 * device's memory, are made write-combining in the device window first.
 * The loader's lines stay; the kernel's start under them.
 */
/* ------------------------------------------------------------------ the marks of the start */

/*
 * Drawn straight on the screen the loader left, through the device
 * window the loader's tables give it (DEV_BASE), before the console is
 * there: the squares of bootinfo.h's marks, and the numbers of an
 * exception. Each also goes to the page of marks for the next loader.
 */
LOCAL BOOL mark_box( CONST T_BOOTINFO *bi, UW x0, UW y0, UW w, UW h, UW c )
{
	_UW	*p;
	UW	x, y, pitch;

	if ( bi == NULL || bi->fb.pa == 0 || bi->fb.format == BI_FB_NONE
	  || x0 + w > bi->fb.width || y0 + h > bi->fb.height ) {
		return FALSE;
	}
	p = (_UW *)DEV_BASE(bi->fb.pa);
	pitch = bi->fb.stride / 4;
	for ( y = 0; y < h; y++ ) {
		for ( x = 0; x < w; x++ ) {
			p[(UD)( y0 + y ) * pitch + x0 + x] = c;
		}
	}
	return TRUE;
}

EXPORT void knl_fbcon_mark( CONST T_BOOTINFO *bi, UW k, UW r, UW g, UW b )
{
	if ( bi == NULL ) {
		return;
	}
	if ( bi->mark_pa != 0 ) {
		((T_BIMARK *)PA2VA(bi->mark_pa))->last = k;
	}
	if ( bi->fb.height >= 40 ) {
		(void)mark_box(bi, 8 + ( k - 1 ) * 30, bi->fb.height - 32, 24, 24, rgb(bi, r, g, b));
	}
}

/* The hexadecimal digits, 3 x 5 dots each, a row in 3 bits from the top */
LOCAL CONST UB	hex_dots[16][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
	{ 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 },
	{ 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 }, { 7, 5, 7, 5, 5 }, { 6, 5, 6, 5, 6 },
	{ 7, 4, 4, 4, 7 }, { 6, 5, 5, 5, 6 }, { 7, 4, 7, 4, 7 }, { 7, 4, 7, 4, 4 }
};

/* v in n hexadecimal digits at x, y, a dot 4 pixels; the x after them */
LOCAL UW mark_hex( CONST T_BOOTINFO *bi, UW x, UW y, UD v, INT n, UW c, UW bg )
{
	INT	i, r, d;

	for ( i = n - 1; i >= 0; i-- ) {
		CONST UB	*g = hex_dots[( v >> ( i * 4 ) ) & 15];

		(void)mark_box(bi, x - 2, y - 2, 16, 24, bg);
		for ( r = 0; r < 5; r++ ) {
			for ( d = 0; d < 3; d++ ) {
				if ( ( g[r] >> ( 2 - d ) ) & 1 ) (void)mark_box(bi, x + d * 4, y + r * 4, 4, 4, c);
			}
		}
		x += 16;
	}
	return x + 12;
}

/*
 * An exception: in the page of marks always; on the screen only while
 * the console is not yet there (after it, the console says it): a red
 * square after the marks, then the vector, RIP, CR2 and the error code
 */
EXPORT void knl_fbcon_early_exc( UW vec, UD rip, UD cr2, UD err )
{
	CONST T_BOOTINFO	*bi = knl_bootinfo;
	UW			x, y, white, black;

	if ( bi == NULL ) {
		return;
	}
	if ( bi->mark_pa != 0 ) {
		T_BIMARK	*m = (T_BIMARK *)PA2VA(bi->mark_pa);

		m->vec = vec;
		m->rip = rip;
		m->cr2 = cr2;
		m->err = err;
	}
	if ( fb != NULL || bi->fb.height < 80 ) {
		return;
	}
	white = rgb(bi, 0xff, 0xff, 0xff);
	black = rgb(bi, 0, 0, 0);
	(void)mark_box(bi, 8 + ( BI_MK_EXC - 1 ) * 30, bi->fb.height - 32, 24, 24, rgb(bi, 0xff, 0, 0));
	x = 8 + BI_MK_EXC * 30;
	y = bi->fb.height - 30;
	x = mark_hex(bi, x, y, vec, 2, white, black);
	x = mark_hex(bi, x, y, rip, 16, white, black);
	x = mark_hex(bi, x, y, cr2, 16, white, black);
	(void)mark_hex(bi, x, y, err, 4, white, black);
}

EXPORT void knl_fbcon_init( CONST T_BOOTINFO *bi )
{
	tm_mirror = fbcon_out;			/* the console kept for the files, screen or not */
	if ( bi->fb.pa == 0 || bi->fb.format == BI_FB_NONE || bi->fb.width < CONFONT_W * 20 ) {
		return;
	}
	knl_dev_set_wc(bi->fb.pa, bi->fb.size);
	/*
	 * Written only where the device window shows it, first and last
	 * byte: a write that faults here would fault again in the handler
	 * that reports it on this same screen. Without it the console goes
	 * to the files alone (bootlog.c).
	 */
	if ( !knl_dev_mapped(bi->fb.pa) || !knl_dev_mapped(bi->fb.pa + bi->fb.size - 1) ) {
		tm_printf((UB *)"TessronOS x64: the screen at %x%08x is not mapped; the console goes to the files only\n",
			  (UW)(bi->fb.pa >> 32), (UW)bi->fb.pa);
		return;
	}
	fb = (_UW *)DEV_BASE(bi->fb.pa);
	fb_w = bi->fb.width;
	fb_h = bi->fb.height;
	fb_pitch = bi->fb.stride / 4;
	scale = ( bi->con_scale >= 1 && bi->con_scale <= 4 ) ? bi->con_scale
	      : ( fb_w >= 1600 && fb_h >= 900 ) ? 2 : 1;
	cw = CONFONT_W * scale;
	ch = CONFONT_H * scale;
	cols = fb_w / cw;
	rows = fb_h / ch;
	lines_end = ( rows > 4 ) ? rows - 1 : rows;
	fg = rgb(bi, 0xe0, 0xe0, 0xe0);
	bg = rgb(bi, 0, 0, 0);
	band = rgb(bi, 0x60, 0xc0, 0xff);
	red = rgb(bi, 0xff, 0x50, 0x50);
	pin_fg = rgb(bi, 0xff, 0xff, 0x60);
	cx = 0;
	cy = bi->con_y / ch;
	if ( cy < 1 || cy >= lines_end ) cy = 1;
	clear_row(cy);
	clear_row(next_row(cy));
	draw_band((CONST UB *)" TessronOS x64: ", band);
}

/*
 * The step of the start-up under way, in the band at the top and in the
 * console's log: where the machine stopped can be read off a photograph
 */
IMPORT CONST T_BOOTINFO	*knl_bootinfo;

/* Whether the settings name this step after "key=" (the step's name begins with the word given) */
LOCAL BOOL asked( CONST char *key, CONST char *s )
{
	CONST char	*c = knl_bootinfo->cmdline;
	INT		k, i;

	for ( ; *c != '\0'; c++ ) {
		for ( k = 0; key[k] != '\0' && c[k] == key[k]; k++ ) ;
		if ( key[k] != '\0' || ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) ) continue;
		c += k;
		for ( i = 0; c[i] != '\0' && c[i] != ' ' && s[i] == c[i]; i++ ) ;
		return (BOOL)( i > 0 && ( c[i] == '\0' || c[i] == ' ' ) );
	}
	return FALSE;
}

EXPORT void knl_fbcon_stage( CONST char *s )
{
	INT	i;
	UINT	lk;

	lk = tm_out_lock();			/* the band and the watch's row drawn whole, not under the driver */
	for ( i = 0; i < (INT)sizeof(stage_now) - 1 && s[i] != '\0'; i++ ) stage_now[i] = (UB)s[i];
	stage_now[i] = '\0';
	stage_at = knl_lockup_now();
	watch_last = 0;
	tm_printf((UB *)"[stage] %s\n", s);
	if ( fb != NULL && !stopped ) {
		draw_band((CONST UB *)" TessronOS x64: ", band);
	}
	tm_out_unlock(lk);
	if ( knl_bootinfo == NULL ) {
		return;
	}
	if ( asked("faultat=", s) ) {
		tm_printf((UB *)"[stage] a fault made here, as the settings ask (faultat=)\n");
		Asm("ud2");
	}
	if ( asked("stopat=", s) ) {
		tm_printf((UB *)"[stage] stopped here, as the settings ask (stopat=)\n");
		for ( ;; ) {
			if ( !in_indp() ) {
				tk_slp_tsk(TMO_FEVR);
			} else {
				Asm("hlt");
			}
		}
	}
}

EXPORT CONST UB *knl_fbcon_last_stage( void )
{
	return stage_now;
}

/*
 * The screen's driver has the screen now: it is about to draw on it or
 * change its size. Taken under the console's lock, so that no line is
 * half drawn when the driver begins, and after the watch's row being
 * written on another processor is done.
 */
EXPORT void knl_fbcon_stop( void )
{
	UINT	lk;
	INT	n;

	lk = tm_out_lock();
	stopped = TRUE;
	handed = TRUE;
	tm_out_unlock(lk);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);	/* stopped seen before watch_busy is read */
	for ( n = 0; n < 1000000 && __atomic_load_n(&watch_busy, __ATOMIC_ACQUIRE) != 0; n++ ) {
		Asm("pause");
	}
}

/*
 * The start-up's watch, from each processor's timer (lockup.c, every
 * half second, interrupt context). Once the step under way has stood
 * WATCH_S seconds, the bottom row names it with the seconds it has
 * stood and how many of the processors took their timer's interrupt in
 * the last second, written again each second by whichever processor
 * comes first. A count that does not go up between two looks at the
 * screen means no processor takes interrupts any more; fewer processors
 * than there are means one is held with its interrupts masked or was
 * stopped by an exception; the first of them is named (quiet, from
 * lockup.c) with where its last timer's interrupt found it. It ends
 * when the screen's driver first draws (knl_fbcon_stop).
 */
EXPORT void knl_fbcon_watch( UD now, UD frq, INT me, INT ticking, INT online, CONST UB *quiet )
{
	UB	t[256];
	UD	at = stage_at, s;
	UW	exp = 0;
	INT	n, i, x0;

	if ( fb == NULL || stopped || handed || lines_end == rows || at == 0 || frq == 0 || now <= at ) {
		return;
	}
	s = ( now - at ) / frq;
	if ( s < WATCH_S && !watch_shown ) {
		return;
	}
	if ( s == watch_last ) {
		return;
	}
	if ( !__atomic_compare_exchange_n(&watch_busy, &exp, (UW)( me + 1 ), FALSE,
					  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ) {
		return;				/* another processor is writing it */
	}
	if ( !stopped && s != watch_last ) {
		watch_last = s;
		clear_row(rows - 1);
		if ( s < WATCH_S ) {
			watch_shown = FALSE;	/* a new step: the row cleared until it stands */
		} else {
			/* the counts first: a long step's name is cut at the right, and the band has it */
			n = tm_sprintf(t, (CONST UB *)" %ld s at this step, %d/%d CPUs ticking%s%s (seen from CPU%d): %s ",
				       (D)s, ticking, online, ( quiet != NULL && quiet[0] != '\0' ) ? "; " : "",
				       ( quiet != NULL ) ? quiet : (CONST UB *)"", me, stage_now);
			x0 = ( n < (INT)cols ) ? (INT)cols - n : 0;
			for ( i = 0; i < n && x0 + i < (INT)cols; i++ ) {
				cell((UW)( x0 + i ), rows - 1, t[i], bg, red);
			}
			watch_shown = TRUE;
		}
	}
	__atomic_store_n(&watch_busy, 0, __ATOMIC_RELEASE);
}

/*
 * What stops the machine is said on the screen again, on a cleared
 * screen under a red band naming the last step reached
 */
EXPORT void knl_fbcon_resume( void )
{
	UW	r;

	if ( fb == NULL ) {
		return;
	}
	if ( stopped ) {
		stopped = FALSE;
		for ( r = 1; r < rows; r++ ) clear_row(r);
		cy = first_row();
	} else {
		newline();
	}
	cx = 0;
	said_before = knl_bootlog_head();
	draw_band((CONST UB *)" TessronOS x64 STOPPED; the last step: ", red);
	draw_pins();
}

/*
 * The machine stops for good (exc_hdl.c, after the records): the screen
 * taken back from whoever drew on it since, cleared, the red band and
 * the lines kept, and the console's last lines under them
 */
LOCAL void tail_line( CONST UB *s, INT n );

EXPORT void knl_fbcon_repaint( void )
{
	UW	r;

	if ( fb == NULL ) {
		return;
	}
	stopped = FALSE;
	for ( r = 1; r < rows; r++ ) clear_row(r);
	draw_band((CONST UB *)" TessronOS x64 STOPPED; the last step: ", red);
	draw_pins();
	cx = 0;
	cy = first_row();
	knl_bootlog_tail(knl_bootlog_head(), (INT)( lines_end - cy ) - 3, tail_line);
}

/*
 * A line kept under the band from now on (the summary of an exception:
 * its kind, RIP, CR2, the task): drawn at once, and again when the screen
 * is cleared for a report. Any context; one more than PIN_MAX takes the
 * last place.
 */
EXPORT void knl_fbcon_pin( CONST UB *line )
{
	UW	k = ( npin < PIN_MAX ) ? npin : PIN_MAX - 1, i;

	for ( i = 0; i < PIN_LEN - 1 && line[i] != '\0' && line[i] != '\n'; i++ ) pin_text[k][i] = line[i];
	pin_text[k][i] = '\0';
	if ( npin < PIN_MAX ) npin++;
	if ( fb == NULL || stopped ) {
		return;
	}
	draw_pins();
	if ( cy < first_row() ) {
		cy = first_row();
		cx = 0;
		clear_row(cy);
		clear_row(next_row(cy));
	}
}

LOCAL void tail_line( CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n && i < (INT)cols - 1; i++ ) {
		draw(( s[i] >= 0x20 && s[i] < 0x7f ) ? s[i] : (UB)' ');
	}
	newline();
}

/*
 * Under the report of what stopped the machine: the console's last lines
 * before it, as many as there is room for down to the foot of the screen
 */
EXPORT void knl_fbcon_tail( void )
{
	INT	room;

	if ( fb == NULL || stopped || cy + 4 >= lines_end ) {
		return;
	}
	room = (INT)( lines_end - cy ) - 3;
	if ( room > 24 ) room = 24;
	newline();
	for ( cx = 0; cx < cols; cx++ ) cell(cx, cy, '-', band, bg);
	newline();
	knl_bootlog_tail(said_before, room, tail_line);
}

#endif /* X64 */
