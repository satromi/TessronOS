/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_glport.h
 *	What the EGL platform tessronos (platform_tessronos.c, built inside
 *	Mesa) asks of the system, in plain C types: Mesa's sources and the
 *	system's headers do not go into one file, so lib/libgl/gl_win.c
 *	and gl_drm.c answer these with the system's calls
 *	(docs/tessronos/13-gpu.md 13.6.3).
 */

#ifndef TS_GLPORT_H
#define TS_GLPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The pixels of a window's work area as the process has them (wm_obj_surf) */
struct ts_gl_wsurf {
	uint32_t	*addr;		/* 0x00rrggbb, rows 'pitch' bytes apart */
	int		width, height;
	int		pitch;
	unsigned	serial;		/* changes when the window's surface is made anew */
};

/*
 * The surface of the window an EGL native window names (include/ts/
 * glapp.h TS_EGLWINDOW): 0, or a negative error when it names none.
 * Asked again after the window changed size, a new surface of the new
 * size.
 */
int	ts_gl_win_surf( const void *native_window, struct ts_gl_wsurf *s );

/* Rectangles of the surface (x, y, w, h each) shown in the window */
int	ts_gl_win_put( const void *native_window, const int *rects, int n );

/* What drives the GPU (include/ts/gpu.h GPU_KIND_*), GPU_KIND_NONE when nothing */
#define TS_GL_GPU_NONE	0
#define TS_GL_GPU_V3D	1
#define TS_GL_GPU_VIRTIO 2
#define TS_GL_GPU_INTEL	3	/* Intel's integrated graphics (x64) */
int	ts_gl_gpu_kind( void );

/* A descriptor of the GPU for the DRM shim (a context of gp_), or -1 */
int	ts_gl_drm_open( void );

/*
 * For the tests of a machine without a V3D: with 'on', the DRM shim
 * pretends the Raspberry Pi 5's V3D (its registers, BOs in the
 * process's memory, jobs that end without running), so that the
 * loader and the driver's screen are made as on the board.
 */
void	ts_gl_drm_fake( int on );
int	ts_gl_drm_faked( void );

/*
 * How far the platform got with the GPU (TS_GL_PROBE_*), and the name
 * of the driver it chose, kept for the tests (ts_gl_probe_result).
 */
#define TS_GL_PROBE_DRAWS	0	/* the GPU draws */
#define TS_GL_PROBE_NONE	1	/* not tried: no GPU, or softpipe asked */
#define TS_GL_PROBE_NOOPEN	2	/* the GPU was not opened */
#define TS_GL_PROBE_NODRIVER	3	/* no driver named for it */
#define TS_GL_PROBE_NOPROBE	4	/* the driver's loader did not take the descriptor */
#define TS_GL_PROBE_NOSCREEN	5	/* the driver's screen was not made */
#define TS_GL_PROBE_NODRAW	6	/* the screen was made, the frame was wrong */
void	ts_gl_probe_note( int step, const char *driver );
int	ts_gl_probe_result( char *driver, int n );

/* Text on the console, for the platform's own messages */
void	ts_gl_log( const char *s );

#ifdef __x86_64__
/*
 * What Intel's driver asked of i915 (the DRM shim's requests) and what
 * was refused, each kind once with its errno, on the console
 */
void	ts_gl_drm_tell( const char *why );
#endif

#ifdef __cplusplus
}
#endif

#endif /* TS_GLPORT_H */
