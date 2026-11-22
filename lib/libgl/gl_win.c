/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gl_win.c
 *	The windows and the GPU as the EGL platform tessronos sees them
 *	(tools/mesa/shim/ts_glport.h, docs/tessronos/13-gpu.md 13.6.3): the
 *	surface of a window's work area, the rectangles of it shown, and
 *	what drives the GPU.
 */

#include <config.h>		/* first: the core's types as wide as the kernel's */
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/wm.h>
#include <ts/glapp.h>
#include <ts/gpuapp.h>
#include "ts_glport.h"

int ts_gl_win_surf( const void *native_window, struct ts_gl_wsurf *s )
{
	CONST TS_EGLWINDOW	*w = (CONST TS_EGLWINDOW *)native_window;
	T_WMSURF		k;
	ER			er;

	if ( w == NULL || w->key <= 0 || s == NULL ) {
		return E_PAR;
	}
	er = wm_obj_surf(w->key, &k);
	if ( er < E_OK ) {
		return er;
	}
	s->addr = (uint32_t *)k.addr;
	s->width = k.width;
	s->height = k.height;
	s->pitch = k.pitch;
	s->serial = k.serial;
	return 0;
}

int ts_gl_win_put( const void *native_window, const int *rects, int n )
{
	CONST TS_EGLWINDOW	*w = (CONST TS_EGLWINDOW *)native_window;
	T_DPRECT		r[WM_PUT_MAX];
	INT			i;

	if ( w == NULL || w->key <= 0 || n <= 0 ) {
		return E_PAR;
	}
	if ( n > WM_PUT_MAX ) {
		n = WM_PUT_MAX;
	}
	for ( i = 0; i < n; i++ ) {
		r[i].left = rects[i * 4];
		r[i].top = rects[i * 4 + 1];
		r[i].right = rects[i * 4] + rects[i * 4 + 2];
		r[i].bottom = rects[i * 4 + 1] + rects[i * 4 + 3];
	}
	return wm_obj_put(w->key, r, n);
}

static int	probe_step = TS_GL_PROBE_NONE;
static char	probe_driver[16];
static char	probe_why[160];
static int	last_er;		/* the last refusal of gp_ref_gpu or gp_opn_gpu */

static void copy_str( char *to, int n, const char *from )
{
	int i = 0;

	for ( ; from != NULL && i < n - 1 && from[i] != '\0'; i++ ) {
		to[i] = from[i];
	}
	if ( n > 0 ) {
		to[i] = '\0';
	}
}

void ts_gl_probe_note( int step, const char *driver )
{
	probe_step = step;
	copy_str(probe_driver, sizeof(probe_driver), driver);
	probe_why[0] = '\0';
}

int ts_gl_probe_result( char *driver, int n )
{
	if ( driver != NULL ) {
		copy_str(driver, n, probe_driver);
	}
	return probe_step;
}

void ts_gl_probe_why( const char *why )
{
	copy_str(probe_why, sizeof(probe_why), why);
}

int ts_gl_probe_reason( char *why, int n )
{
	if ( why != NULL ) {
		copy_str(why, n, probe_why);
	}
	return probe_step;
}

void ts_gl_note_error( int er )
{
	last_er = er;
}

int ts_gl_last_error( void )
{
	return last_er;
}

const char *ts_gl_error_name( int er )
{
	switch ( er ) {
	case 0:		return "none";
	case E_NOEXS:	return "E_NOEXS";
	case E_NOMEM:	return "E_NOMEM";
	case E_OBJ:	return "E_OBJ";
	case E_OACV:	return "E_OACV";
	case E_NOSPT:	return "E_NOSPT";
	case E_LIMIT:	return "E_LIMIT";
	case E_PAR:	return "E_PAR";
	case E_ID:	return "E_ID";
	case E_IO:	return "E_IO";
	case E_SYS:	return "E_SYS";
	case E_RSFN:	return "E_RSFN";
	default:	return "another error";
	}
}

int ts_gl_gpu_kind( void )
{
	T_GPUINFO	info;
	ER		er;

	if ( ts_gl_drm_faked() ) {
#ifdef __x86_64__
		if ( ts_gl_drm_faked() == 2 ) {
			return TS_GL_GPU_INTEL;	/* the shim pretends an Intel GPU */
		}
#endif
		return TS_GL_GPU_V3D;		/* the shim pretends one */
	}
	er = gp_ref_gpu(&info);
	if ( er < E_OK ) {
		last_er = er;
		return TS_GL_GPU_NONE;
	}
	return (int)info.kind;
}

void ts_gl_log( const char *s )
{
	(void)tm_putstring((CONST UB *)s);
}
