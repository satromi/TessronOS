/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	glprog.c
 *	A program the kernel tests start as a process (tests/ktest/ktest_gl.c)
 *	to draw with OpenGL ES (make GL=1: Mesa with softpipe when there is
 *	no GPU; docs/tessronos/13-gpu.md 13.6), on /boot as GLPROG.ELF.
 *
 *	The start-up argument names what to draw; every drawing is read back
 *	with glReadPixels (or out of the window's pixels) and compared with
 *	what it should be. The exit code is 0 when it was, else the number
 *	of the first step that was not.
 *
 *	info	the display, its configs and the context: EGL 1.5, OpenGL ES
 *		3, the extensions a program relies on
 *	tri	a triangle of one colour over a cleared pbuffer, drawn by
 *		GLSL shaders with a uniform
 *	depth	two rectangles at different depths drawn far one last: the
 *		depth test keeps the near one
 *	tex	a texture of four texels drawn over the pbuffer, nearest
 *		sampling, and a texture of mipmaps made by the library
 *	glsl	a GLSL ES 3.00 shader with a loop, a function, an integer
 *		uniform and gl_FragCoord, whose colours are computed here
 *		too; and a vertex shader that moves the vertices
 *	fbo	a context with no surface (EGL_KHR_surfaceless_context)
 *		drawing into a framebuffer object
 *	win	a window of its own as an EGL window surface: two frames
 *		swapped, read back from the window's pixels (wm_obj_surf)
 *	speed	frames of 300 x 300 pixels with lighting and a depth
 *		buffer, timed
 *	v3dpath	the platform's choice of the V3D's driver and its screen,
 *		against the DRM shim's pretended V3D
 */

#include <config.h>		/* first: the core's types as wide as the kernel's */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/wm.h>
#include <ts/glapp.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

SZ	ts_get_arg( void *buf, SZ size );

#define W	64
#define H	64

static EGLDisplay	dpy;
static EGLContext	ctx;
static EGLSurface	surf = EGL_NO_SURFACE;
static EGLConfig	cfg;

static long now_us( void )
{
	struct timespec	t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

/* The display, a config with depth that has the surface type asked, and an ES 3 context */
static int egl_up( EGLint surface_type )
{
	EGLint	maj, min, n;
	EGLint	ca[] = {
		EGL_SURFACE_TYPE, surface_type,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 16,
		EGL_NONE
	};
	EGLint	xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };

	dpy = eglGetPlatformDisplay(EGL_PLATFORM_TESSRONOS, NULL, NULL);
	if ( dpy == EGL_NO_DISPLAY ) return 1;
	if ( !eglInitialize(dpy, &maj, &min) ) return 2;
	if ( !eglBindAPI(EGL_OPENGL_ES_API) ) return 3;
	if ( !eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1 ) return 4;
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
	if ( ctx == EGL_NO_CONTEXT ) return 5;
	return 0;
}

static int pbuffer_up( int w, int h )
{
	EGLint	pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
	int	r = egl_up(EGL_PBUFFER_BIT);

	if ( r != 0 ) return r;
	surf = eglCreatePbufferSurface(dpy, cfg, pa);
	if ( surf == EGL_NO_SURFACE ) return 6;
	if ( !eglMakeCurrent(dpy, surf, surf, ctx) ) return 7;
	glViewport(0, 0, w, h);
	return 0;
}

static void egl_down( void )
{
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if ( surf != EGL_NO_SURFACE ) eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
}

static GLuint shader( GLenum type, const char *src )
{
	GLuint	s = glCreateShader(type);
	GLint	ok = 0;
	char	log[512];

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if ( !ok ) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("glprog: shader: %s\n", log);
		return 0;
	}
	return s;
}

static GLuint program( const char *vs, const char *fs )
{
	GLuint	v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs), p;
	GLint	ok = 0;
	char	log[512];

	if ( v == 0 || f == 0 ) return 0;
	p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "pos");
	glBindAttribLocation(p, 1, "uv");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if ( !ok ) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		printf("glprog: link: %s\n", log);
		return 0;
	}
	glDeleteShader(v);
	glDeleteShader(f);
	return p;
}

/* A pixel read back, against what it should be (a difference of tol allowed in each colour) */
static int pixel_is( int x, int y, int r, int g, int b, int tol )
{
	GLubyte	p[4];

	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	if ( abs(p[0] - r) > tol || abs(p[1] - g) > tol || abs(p[2] - b) > tol ) {
		printf("glprog: pixel %d,%d is %d,%d,%d, not %d,%d,%d\n", x, y, p[0], p[1], p[2], r, g, b);
		return 0;
	}
	return 1;
}

static const char *vs_flat =
	"attribute vec2 pos;\n"
	"void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_flat =
	"precision mediump float;\n"
	"uniform vec4 colour;\n"
	"void main() { gl_FragColor = colour; }\n";

static int test_info( void )
{
	const char	*s;
	int		r = egl_up(EGL_PBUFFER_BIT);

	if ( r != 0 ) return 100 + r;
	printf("glprog: EGL %s, %s\n", eglQueryString(dpy, EGL_VERSION), eglQueryString(dpy, EGL_VENDOR));
	s = eglQueryString(dpy, EGL_EXTENSIONS);
	printf("glprog: EGL extensions: %s\n", s);
	if ( strstr(s, "EGL_KHR_surfaceless_context") == NULL ) return 110;
	if ( strstr(s, "EGL_KHR_create_context") == NULL ) return 111;
	if ( !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) ) return 112;
	printf("glprog: GL %s | %s | %s | GLSL %s\n", glGetString(GL_VERSION), glGetString(GL_VENDOR),
	       glGetString(GL_RENDERER), glGetString(GL_SHADING_LANGUAGE_VERSION));
	if ( strncmp((const char *)glGetString(GL_VERSION), "OpenGL ES 3.", 12) != 0 ) return 113;
	egl_down();
	return 0;
}

static int test_tri( void )
{
	static const GLfloat	v[] = { -0.75f, -0.75f, 0.75f, -0.75f, 0.0f, 0.75f };
	GLuint			p;
	int			r = pbuffer_up(W, H);

	if ( r != 0 ) return 200 + r;
	p = program(vs_flat, fs_flat);
	if ( p == 0 ) return 210;
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(p);
	glUniform4f(glGetUniformLocation(p, "colour"), 1.0f, 0.0f, 0.0f, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	if ( glGetError() != GL_NO_ERROR ) return 211;
	if ( !pixel_is(W / 2, H / 2, 255, 0, 0, 0) ) return 212;	/* inside */
	if ( !pixel_is(2, H - 3, 0, 0, 255, 0) ) return 213;		/* above the left edge */
	if ( !pixel_is(W - 3, H - 3, 0, 0, 255, 0) ) return 214;
	if ( !pixel_is(W / 2, 4, 0, 0, 255, 0) ) return 215;		/* below the base */
	if ( !pixel_is(W / 2, 10, 255, 0, 0, 0) ) return 216;		/* just above it */
	egl_down();
	return 0;
}

static int test_depth( void )
{
	static const char	*vs =
		"attribute vec2 pos;\n"
		"uniform float z;\n"
		"void main() { gl_Position = vec4(pos, z, 1.0); }\n";
	static const GLfloat	near[] = { -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
	static const GLfloat	far[] = { -1.0f, -1.0f, 0.25f, -1.0f, -1.0f, 0.25f, 0.25f, 0.25f };
	GLuint			p;
	int			r = pbuffer_up(W, H);

	if ( r != 0 ) return 300 + r;
	p = program(vs, fs_flat);
	if ( p == 0 ) return 310;
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearDepthf(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glUseProgram(p);
	glEnableVertexAttribArray(0);
	/* the near one (green) first, then the far one (red) over it */
	glUniform1f(glGetUniformLocation(p, "z"), -0.5f);
	glUniform4f(glGetUniformLocation(p, "colour"), 0.0f, 1.0f, 0.0f, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, near);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUniform1f(glGetUniformLocation(p, "z"), 0.5f);
	glUniform4f(glGetUniformLocation(p, "colour"), 1.0f, 0.0f, 0.0f, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, far);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if ( glGetError() != GL_NO_ERROR ) return 311;
	if ( !pixel_is(W / 2 - 2, H / 2 - 2, 0, 255, 0, 0) ) return 312;	/* both: the near one */
	if ( !pixel_is(4, 4, 255, 0, 0, 0) ) return 313;			/* the far one alone */
	if ( !pixel_is(W - 4, H - 4, 0, 0, 0, 0) ) return 314;			/* neither */
	if ( !pixel_is(W - 20, W - 20, 0, 255, 0, 0) ) return 315;		/* the near one alone */
	egl_down();
	return 0;
}

static const char *vs_tex =
	"attribute vec2 pos;\n"
	"attribute vec2 uv;\n"
	"varying vec2 t;\n"
	"void main() { t = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_tex =
	"precision mediump float;\n"
	"uniform sampler2D s;\n"
	"varying vec2 t;\n"
	"void main() { gl_FragColor = texture2D(s, t); }\n";
static const GLfloat quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
static const GLfloat quad_uv[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };

static int test_tex( void )
{
	/* rows from the bottom: red, green; blue, white */
	static const GLubyte	texel[] = {
		255, 0, 0, 255,   0, 255, 0, 255,
		0, 0, 255, 255,   255, 255, 255, 255
	};
	GLubyte			*big;
	GLuint			p, t[2];
	int			r = pbuffer_up(W, H), i;

	if ( r != 0 ) return 400 + r;
	p = program(vs_tex, fs_tex);
	if ( p == 0 ) return 410;
	glGenTextures(2, t);
	glBindTexture(GL_TEXTURE_2D, t[0]);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "s"), 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, quad_uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if ( glGetError() != GL_NO_ERROR ) return 411;
	if ( !pixel_is(W / 4, H / 4, 255, 0, 0, 0) ) return 412;
	if ( !pixel_is(3 * W / 4, H / 4, 0, 255, 0, 0) ) return 413;
	if ( !pixel_is(W / 4, 3 * H / 4, 0, 0, 255, 0) ) return 414;
	if ( !pixel_is(3 * W / 4, 3 * H / 4, 255, 255, 255, 0) ) return 415;

	/* 256 x 256 of half red, half blue in columns, drawn at a quarter of its size */
	big = malloc(256 * 256 * 4);
	if ( big == NULL ) return 416;
	for ( i = 0; i < 256 * 256; i++ ) {
		int odd = ( i & 1 ) != 0;
		big[i * 4] = odd ? 0 : 255;
		big[i * 4 + 1] = 0;
		big[i * 4 + 2] = odd ? 255 : 0;
		big[i * 4 + 3] = 255;
	}
	glBindTexture(GL_TEXTURE_2D, t[1]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	free(big);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if ( glGetError() != GL_NO_ERROR ) return 417;
	/* the columns averaged by the mipmaps: purple everywhere */
	if ( !pixel_is(W / 3, H / 2, 128, 0, 128, 3) ) return 418;
	if ( !pixel_is(2 * W / 3, H / 5, 128, 0, 128, 3) ) return 419;
	egl_down();
	return 0;
}

static int test_glsl( void )
{
	static const char	*vs =
		"#version 300 es\n"
		"layout(location = 0) in vec2 pos;\n"
		"uniform vec2 shift;\n"
		"void main() { gl_Position = vec4(pos * 0.5 + shift, 0.0, 1.0); }\n";
	static const char	*fs =
		"#version 300 es\n"
		"precision highp float;\n"
		"precision highp int;\n"
		"uniform int n;\n"
		"out vec4 o;\n"
		"float tri(int k) { float s = 0.0; for (int i = 1; i <= k; i++) s += float(i); return s; }\n"
		"void main() {\n"
		"  vec2 f = floor(gl_FragCoord.xy);\n"
		"  o = vec4(f.x / 255.0, f.y / 255.0, tri(n) / 255.0, 1.0);\n"
		"  if (int(f.x) % 2 == 1) o.g = 1.0;\n"
		"}\n";
	GLuint			p;
	int			r = pbuffer_up(W, H);

	if ( r != 0 ) return 500 + r;
	p = program(vs, fs);
	if ( p == 0 ) return 510;
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "n"), 10);			/* 1 + ... + 10 = 55 */
	glUniform2f(glGetUniformLocation(p, "shift"), 0.5f, 0.5f);	/* the upper right quarter */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if ( glGetError() != GL_NO_ERROR ) return 511;
	if ( !pixel_is(40, 50, 40, 50, 55, 1) ) return 512;
	if ( !pixel_is(41, 50, 41, 255, 55, 1) ) return 513;
	if ( !pixel_is(60, 33, 60, 33, 55, 1) ) return 514;
	if ( !pixel_is(20, 20, 0, 0, 0, 0) ) return 515;		/* outside the quarter */
	if ( !pixel_is(40, 20, 0, 0, 0, 0) ) return 516;
	egl_down();
	return 0;
}

static int test_fbo( void )
{
	static const GLfloat	v[] = { -1.0f, -1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f };
	GLuint			fb, rb, p;
	int			r = egl_up(EGL_PBUFFER_BIT);

	if ( r != 0 ) return 600 + r;
	if ( !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) ) return 610;
	glGenFramebuffers(1, &fb);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	if ( glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ) return 611;
	glViewport(0, 0, 32, 32);
	glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	p = program(vs_flat, fs_flat);
	if ( p == 0 ) return 612;
	glUseProgram(p);
	glUniform4f(glGetUniformLocation(p, "colour"), 1.0f, 1.0f, 0.0f, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);			/* the lower left quarter */
	if ( glGetError() != GL_NO_ERROR ) return 613;
	if ( !pixel_is(4, 4, 255, 255, 0, 0) ) return 614;
	if ( !pixel_is(28, 28, 0, 255, 0, 0) ) return 615;
	glDeleteFramebuffers(1, &fb);
	glDeleteRenderbuffers(1, &rb);
	egl_down();
	return 0;
}

/* A pixel of the window's own surface, x and y from the top left */
static int win_is( const T_WMSURF *s, int x, int y, uint32_t want )
{
	uint32_t px = ((const uint32_t *)( (const UB *)s->addr + (SZ)y * s->pitch ))[x];

	if ( px != want ) {
		printf("glprog: window pixel %d,%d is %06x, not %06x\n", x, y, (unsigned)px, (unsigned)want);
		return 0;
	}
	return 1;
}

static int test_win( void )
{
	static const GLfloat	v[] = { -1.0f, -1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f };
	CONST char		*j = "{\"rect\":[200,150,420,330],\"attr\":7}";
	TS_EGLWINDOW		ew;
	T_OBCRE			c;
	TS_UUID			win;
	T_WMSURF		s;
	EGLint			w, h;
	GLuint			p;
	int			r;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"glprog";
	c.json = (CONST UB *)j;
	c.jsonsz = (SZ)strlen(j);
	if ( ob_cre_obj(&c, &win) != E_OK ) return 700;
	ew.key = ob_opn_obj(&win, OB_OP_R | OB_OP_W);
	if ( ew.key <= 0 ) return 701;
	r = egl_up(EGL_WINDOW_BIT);
	if ( r != 0 ) return 702 + r;
	surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)&ew, NULL);
	if ( surf == EGL_NO_SURFACE ) {
		printf("glprog: window surface: error %x\n", eglGetError());
		return 710;
	}
	if ( !eglMakeCurrent(dpy, surf, surf, ctx) ) return 711;
	eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
	if ( wm_obj_surf(ew.key, &s) != E_OK ) return 712;
	printf("glprog: window surface %dx%d, work area %dx%d\n", w, h, s.width, s.height);
	if ( w != s.width || h != s.height ) return 713;

	/* the first frame: blue */
	glViewport(0, 0, w, h);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	if ( !pixel_is(1, 1, 0, 0, 255, 0) ) return 721;		/* read back before the swap */
	if ( !eglSwapBuffers(dpy, surf) ) return 714;
	if ( !win_is(&s, 0, 0, 0x0000FF) || !win_is(&s, w - 1, h - 1, 0x0000FF) ) return 715;

	/* the second: the lower left quarter yellow, the rest as it was (the buffer kept) */
	p = program(vs_flat, fs_flat);
	if ( p == 0 ) return 716;
	glUseProgram(p);
	glUniform4f(glGetUniformLocation(p, "colour"), 1.0f, 1.0f, 0.0f, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if ( !eglSwapBuffers(dpy, surf) ) return 717;
	if ( !win_is(&s, 2, h - 3, 0xFFFF00) ) return 718;		/* lower left */
	if ( !win_is(&s, 2, 2, 0x0000FF) ) return 719;			/* upper left */
	if ( !win_is(&s, w - 3, h - 3, 0x0000FF) ) return 720;		/* lower right */
	egl_down();
	(void)ob_del_obj(&win);
	return 0;
}

/* Frames of rings of quads with a light, timed: the speed of softpipe */
static int test_speed( void )
{
	static const char	*vs =
		"attribute vec3 pos;\n"
		"attribute vec3 nrm;\n"
		"uniform mat4 m;\n"
		"varying float l;\n"
		"void main() {\n"
		"  vec3 n = normalize((m * vec4(nrm, 0.0)).xyz);\n"
		"  l = 0.2 + 0.8 * max(dot(n, normalize(vec3(0.3, 0.5, 1.0))), 0.0);\n"
		"  gl_Position = m * vec4(pos, 1.0);\n"
		"}\n";
	static const char	*fs =
		"precision mediump float;\n"
		"varying float l;\n"
		"void main() { gl_FragColor = vec4(0.8 * l, 0.3 * l, 0.1 * l, 1.0); }\n";
	enum { SEG = 64, RINGS = 8, FRAMES = 20 };
	GLfloat		*vtx, m[16];
	GLuint		p, b;
	int		r = pbuffer_up(300, 300), i, k, n = 0, f;
	long		t0, t1;

	if ( r != 0 ) return 800 + r;
	p = program(vs, fs);
	if ( p == 0 ) return 810;
	glBindAttribLocation(p, 1, "nrm");
	glLinkProgram(p);
	vtx = malloc(sizeof(GLfloat) * 6 * 6 * SEG * RINGS);
	if ( vtx == NULL ) return 811;
	for ( k = 0; k < RINGS; k++ ) {
		float	r0 = 0.1f + 0.1f * k, r1 = r0 + 0.08f, z = -0.2f + 0.05f * k;

		for ( i = 0; i < SEG; i++ ) {
			float	a0 = 6.2831853f * i / SEG, a1 = 6.2831853f * ( i + 1 ) / SEG;
			float	q[4][3] = {
				{ r0 * __builtin_cosf(a0), r0 * __builtin_sinf(a0), z },
				{ r1 * __builtin_cosf(a0), r1 * __builtin_sinf(a0), z },
				{ r1 * __builtin_cosf(a1), r1 * __builtin_sinf(a1), z + 0.03f },
				{ r0 * __builtin_cosf(a1), r0 * __builtin_sinf(a1), z + 0.03f },
			};
			int	o[6] = { 0, 1, 2, 0, 2, 3 }, e;

			for ( e = 0; e < 6; e++ ) {
				GLfloat *d = vtx + n * 6;

				d[0] = q[o[e]][0];
				d[1] = q[o[e]][1];
				d[2] = q[o[e]][2];
				d[3] = 0.2f * __builtin_cosf(a0);
				d[4] = 0.2f * __builtin_sinf(a0);
				d[5] = 1.0f;
				n++;
			}
		}
	}
	glGenBuffers(1, &b);
	glBindBuffer(GL_ARRAY_BUFFER, b);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 6 * n, vtx, GL_STATIC_DRAW);
	free(vtx);
	glUseProgram(p);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
	t0 = now_us();
	for ( f = 0; f < FRAMES; f++ ) {
		float a = 0.1f * f, c = __builtin_cosf(a), s = __builtin_sinf(a);

		memset(m, 0, sizeof(m));
		m[0] = c;	m[1] = s * 0.5f;	m[2] = s * 0.8f;
		m[4] = -s;	m[5] = c * 0.5f;	m[6] = c * 0.8f;
		m[9] = -0.8f;	m[10] = 0.5f;
		m[15] = 1.0f;
		glUniformMatrix4fv(glGetUniformLocation(p, "m"), 1, GL_FALSE, m);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, n);
		eglSwapBuffers(dpy, surf);
	}
	glFinish();
	t1 = now_us();
	if ( glGetError() != GL_NO_ERROR ) return 812;
	printf("glprog: speed: %d frames of 300x300, %d triangles each, %ld ms a frame (%ld.%01ld fps)\n",
	       FRAMES, n / 3, ( t1 - t0 ) / FRAMES / 1000, FRAMES * 1000000L / ( t1 - t0 ),
	       ( FRAMES * 10000000L / ( t1 - t0 ) ) % 10);
	egl_down();
	return 0;
}

/* The DRM shim's pretended V3D and how far the platform got (tools/mesa/shim/ts_glport.h) */
void	ts_gl_drm_fake( int on );
int	ts_gl_probe_result( char *driver, int n );

#define PROBE_NODRAW	6		/* TS_GL_PROBE_NODRAW */

/*
 * The loader's path to the V3D's driver on a machine without one: the
 * DRM shim pretends the Raspberry Pi 5's V3D, and the platform has to
 * choose the driver v3d and make its screen. The jobs do not run, so
 * the frame the platform checks is wrong and it goes on with softpipe.
 */
static int test_v3dpath( void )
{
	EGLint	maj, min;
	char	drv[16];
	int	step;

	ts_gl_drm_fake(1);
	dpy = eglGetPlatformDisplay(EGL_PLATFORM_TESSRONOS, NULL, NULL);
	if ( dpy == EGL_NO_DISPLAY ) return 901;
	if ( !eglInitialize(dpy, &maj, &min) ) return 902;
	step = ts_gl_probe_result(drv, sizeof(drv));
	printf("glprog: v3dpath: driver '%s', step %d\n", drv, step);
	eglTerminate(dpy);
	ts_gl_drm_fake(0);
	if ( strcmp(drv, "v3d") != 0 ) return 903;
	if ( step != PROBE_NODRAW ) return 910 + step;
	return 0;
}

#ifdef __x86_64__
/*
 * The platform's choice of Intel's driver (iris), its screen and the
 * frame of the check, against the DRM shim's pretended Kaby Lake: the
 * frame is not drawn there, so the platform goes back to softpipe at
 * that step
 */
static int test_irispath( void )
{
	EGLint	maj, min;
	char	drv[16];
	int	step;

	ts_gl_drm_fake(2);
	dpy = eglGetPlatformDisplay(EGL_PLATFORM_TESSRONOS, NULL, NULL);
	if ( dpy == EGL_NO_DISPLAY ) return 901;
	if ( !eglInitialize(dpy, &maj, &min) ) return 902;
	step = ts_gl_probe_result(drv, sizeof(drv));
	printf("glprog: irispath: driver '%s', step %d\n", drv, step);
	eglTerminate(dpy);
	ts_gl_drm_fake(0);
	if ( strcmp(drv, "iris") != 0 ) return 903;
	if ( step != PROBE_NODRAW ) return 910 + step;
	return 0;
}

/*
 * The DRM shim's waits on synchronisation objects (WAIT_FOR_SUBMIT,
 * WAIT_ALL) and its reading of the time stamp (REG_READ), against the
 * pretended Kaby Lake
 */
int	ts_gl_i915_check( void );

static int test_i915sync( void )
{
	int	r;

	ts_gl_drm_fake(2);
	r = ts_gl_i915_check();
	ts_gl_drm_fake(0);
	printf("glprog: i915sync: step %d\n", r);
	return ( r == 0 ) ? 0 : 920 + r;
}
#endif

int main( void )
{
	char	mode[32];
	SZ	n = ts_get_arg(mode, sizeof(mode) - 1);
	long	t0 = now_us();
	int	r;

	if ( n < 0 ) n = 0;
	mode[n] = '\0';
	if ( strcmp(mode, "info") == 0 )	r = test_info();
	else if ( strcmp(mode, "tri") == 0 )	r = test_tri();
	else if ( strcmp(mode, "depth") == 0 )	r = test_depth();
	else if ( strcmp(mode, "tex") == 0 )	r = test_tex();
	else if ( strcmp(mode, "glsl") == 0 )	r = test_glsl();
	else if ( strcmp(mode, "fbo") == 0 )	r = test_fbo();
	else if ( strcmp(mode, "win") == 0 )	r = test_win();
	else if ( strcmp(mode, "speed") == 0 )	r = test_speed();
	else if ( strcmp(mode, "v3dpath") == 0 ) r = test_v3dpath();
#ifdef __x86_64__
	else if ( strcmp(mode, "irispath") == 0 ) r = test_irispath();
	else if ( strcmp(mode, "i915sync") == 0 ) r = test_i915sync();
#endif
	else					r = 99;
	printf("glprog: %s: %s %d (%ld ms)\n", mode, r == 0 ? "ok" : "failed at", r, ( now_us() - t0 ) / 1000);
	return r;
}
