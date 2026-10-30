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

void ts_gl_probe_note( int step, const char *driver )
{
	int i = 0;

	probe_step = step;
	if ( driver != NULL ) {
		for ( ; i < (int)sizeof(probe_driver) - 1 && driver[i] != '\0'; i++ ) {
			probe_driver[i] = driver[i];
		}
	}
	probe_driver[i] = '\0';
}

int ts_gl_probe_result( char *driver, int n )
{
	int i;

	for ( i = 0; driver != NULL && i < n - 1 && probe_driver[i] != '\0'; i++ ) {
		driver[i] = probe_driver[i];
	}
	if ( driver != NULL && n > 0 ) {
		driver[i] = '\0';
	}
	return probe_step;
}

int ts_gl_gpu_kind( void )
{
	T_GPUINFO	info;

	if ( ts_gl_drm_faked() ) {
#ifdef __x86_64__
		if ( ts_gl_drm_faked() == 2 ) {
			return TS_GL_GPU_INTEL;	/* the shim pretends an Intel GPU */
		}
#endif
		return TS_GL_GPU_V3D;		/* the shim pretends one */
	}
	if ( gp_ref_gpu(&info) < E_OK ) {
		return TS_GL_GPU_NONE;
	}
	return (int)info.kind;
}

void ts_gl_log( const char *s )
{
	(void)tm_putstring((CONST UB *)s);
}
