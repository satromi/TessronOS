/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	platform_tessronos.c
 *	The EGL platform of TessronOS (docs/tessronos/13-gpu.md 13.6.3),
 *	put into Mesa's tree by tools/mesa/build.sh and built with libEGL.
 *
 *	A native window is a window of the window system, named by the key of
 *	its window object (include/ts/glapp.h TS_EGLWINDOW). Its pixels are
 *	the surface of the window's work area the process has mapped
 *	(wm_obj_surf); the frame is drawn by the driver into a back buffer of
 *	its own, and eglSwapBuffers copies it there (the swrast loader's
 *	putImage), then shows the rectangles that changed (wm_obj_put): those
 *	named to eglSwapBuffersWithDamageKHR, or else the bands of rows that
 *	differ from what the surface held. When the window changes size the
 *	surface is asked for again and the back buffer follows.
 *
 *	A pbuffer has one buffer, an image the driver makes, which it draws
 *	in as the front buffer; a context needs no surface at all
 *	(EGL_KHR_surfaceless_context).
 *
 *	The driver is the V3D's (v3d) when the machine has one and the
 *	kernel gives the process an address space on it (13-gpu.md 13.5),
 *	and softpipe otherwise, or when TS_GL_DRIVER=softpipe asks for it.
 *	Before the v3d driver is taken, a frame is cleared on the GPU and
 *	read back; when that does not come out right the display falls back
 *	on softpipe. What was chosen, and why, goes to the console (lines
 *	"egl:"). On the V3D a window's frame is drawn in a back buffer of
 *	the driver's (a linear image, in a BO), which eglSwapBuffers maps
 *	and copies into the window's pixels as softpipe's frame is copied.
 *
 *	The system's own calls are reached through tools/mesa/shim/
 *	ts_glport.h, so that this file includes Mesa's headers only.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "egl_dri2.h"
#include "eglglobals.h"
#include "egldevice.h"
#include "loader.h"
#include "dri_util.h"
#include "dri_screen.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/u_inlines.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe-loader/pipe_loader.h"
#ifdef __x86_64__
#include "intel/dev/intel_debug.h"
#endif
#include "ts_glport.h"

#define TS_DAMAGE_MAX	16	/* rectangles of a frame shown at once (WM_PUT_MAX) */

struct ts_surface {
   struct dri2_egl_surface base;
   const void *window;            /* TS_EGLWINDOW; NULL for a pbuffer */
   struct ts_gl_wsurf ws;         /* the window's pixels, as last asked */
   int damage[TS_DAMAGE_MAX * 4]; /* x, y, w, h from the top */
   int ndamage;                   /* 0: find what changed */
   struct dri_image *back;        /* the V3D's: a window's back buffer */
   int sent, put_er;              /* the rectangles the last frame sent, and what that answered */
   unsigned frames, same;         /* GPU frames so far, and how many in a row read back as the one before */
   uint32_t last_sum;
};

static struct ts_surface *
ts_surface(void *loaderPrivate)
{
   return (struct ts_surface *)loaderPrivate;
}

/* The window's surface asked for again; false when the window is gone */
static bool
ts_refresh(struct ts_surface *ts)
{
   if (ts_gl_win_surf(ts->window, &ts->ws) < 0) {
      memset(&ts->ws, 0, sizeof(ts->ws));
      return false;
   }
   ts->base.base.Width = ts->ws.width;
   ts->base.base.Height = ts->ws.height;
   return true;
}

static void
ts_get_drawable_info(struct dri_drawable *draw, int *x, int *y, int *w, int *h,
                     void *loaderPrivate)
{
   struct ts_surface *ts = ts_surface(loaderPrivate);

   *x = *y = 0;
   if (ts->window != NULL)
      (void)ts_refresh(ts);
   *w = ts->base.base.Width;
   *h = ts->base.base.Height;
}

/*
 * Row y of the frame into the window's pixels, as far as both reach
 * (the byte the window does not use cleared); whether anything there
 * changed.
 */
static bool
ts_copy_row(uint32_t *d, const uint32_t *s, int n)
{
   bool changed = false;

   for (int i = 0; i < n; i++) {
      uint32_t p = s[i] & 0x00ffffff;
      if (d[i] != p) {
         d[i] = p;
         changed = true;
      }
   }
   return changed;
}

/* A band of changed rows added to the rectangles shown, the nearest two joined when full */
static void
ts_add_band(int *r, int *n, int x, int w, int top, int bottom)
{
   if (*n == TS_DAMAGE_MAX) {
      int best = 0, gap = INT32_MAX;
      for (int i = 0; i + 1 < *n; i++) {
         int g = r[(i + 1) * 4 + 1] - (r[i * 4 + 1] + r[i * 4 + 3]);
         if (g < gap) {
            gap = g;
            best = i;
         }
      }
      r[best * 4 + 3] = r[(best + 1) * 4 + 1] + r[(best + 1) * 4 + 3] - r[best * 4 + 1];
      memmove(&r[(best + 1) * 4], &r[(best + 2) * 4], (*n - best - 2) * 4 * sizeof(int));
      (*n)--;
   }
   r[*n * 4] = x;
   r[*n * 4 + 1] = top;
   r[*n * 4 + 2] = w;
   r[*n * 4 + 3] = bottom - top;
   (*n)++;
}

static void
ts_put_image2(struct dri_drawable *draw, int op, int x, int y, int w, int h,
              int stride, char *data, void *loaderPrivate)
{
   struct ts_surface *ts = ts_surface(loaderPrivate);
   int rects[TS_DAMAGE_MAX * 4], n = 0;

   if (ts->window == NULL)
      return;                   /* a pbuffer's buffer is an image of the driver's */
   if (op != __DRI_SWRAST_IMAGE_OP_DRAW && op != __DRI_SWRAST_IMAGE_OP_SWAP)
      return;
   if (ts->ws.addr == NULL && !ts_refresh(ts))
      return;

   /* as much of it as lies in the window's surface */
   int x0 = MAX2(x, 0), y0 = MAX2(y, 0);
   int x1 = MIN2(x + w, ts->ws.width), y1 = MIN2(y + h, ts->ws.height);
   if (x0 >= x1 || y0 >= y1)
      return;

   if (ts->ndamage > 0) {
      /* only what the program said it changed */
      for (int i = 0; i < ts->ndamage; i++) {
         int rx0 = MAX2(ts->damage[i * 4], x0), ry0 = MAX2(ts->damage[i * 4 + 1], y0);
         int rx1 = MIN2(ts->damage[i * 4] + ts->damage[i * 4 + 2], x1);
         int ry1 = MIN2(ts->damage[i * 4 + 1] + ts->damage[i * 4 + 3], y1);
         if (rx0 >= rx1 || ry0 >= ry1)
            continue;
         for (int r = ry0; r < ry1; r++) {
            (void)ts_copy_row((uint32_t *)((char *)ts->ws.addr + (size_t)r * ts->ws.pitch) + rx0,
                              (const uint32_t *)(data + (size_t)(r - y) * stride) + (rx0 - x),
                              rx1 - rx0);
         }
         rects[n * 4] = rx0;
         rects[n * 4 + 1] = ry0;
         rects[n * 4 + 2] = rx1 - rx0;
         rects[n * 4 + 3] = ry1 - ry0;
         n++;
      }
   } else {
      /* the bands of rows that differ from what the window has */
      int top = -1;
      for (int r = y0; r < y1; r++) {
         bool c = ts_copy_row((uint32_t *)((char *)ts->ws.addr + (size_t)r * ts->ws.pitch) + x0,
                              (const uint32_t *)(data + (size_t)(r - y) * stride) + (x0 - x),
                              x1 - x0);
         if (c && top < 0) {
            top = r;
         } else if (!c && top >= 0) {
            ts_add_band(rects, &n, x0, x1 - x0, top, r);
            top = -1;
         }
      }
      if (top >= 0)
         ts_add_band(rects, &n, x0, x1 - x0, top, y1);
   }
   ts->sent = n;
   ts->put_er = 0;
   if (n > 0 && (ts->put_er = ts_gl_win_put(ts->window, rects, n)) < 0) {
      /* a new surface since (the window changed size): taken at the next frame */
      memset(&ts->ws, 0, sizeof(ts->ws));
   }
}

static void
ts_put_image(struct dri_drawable *draw, int op, int x, int y, int w, int h,
             char *data, void *loaderPrivate)
{
   ts_put_image2(draw, op, x, y, w, h, w * 4, data, loaderPrivate);
}

static void
ts_get_image2(struct dri_drawable *read, int x, int y, int w, int h, int stride,
              char *data, void *loaderPrivate)
{
   struct ts_surface *ts = ts_surface(loaderPrivate);

   for (int r = 0; r < h; r++) {
      uint32_t *d = (uint32_t *)(data + (size_t)r * stride);
      if (ts->window == NULL || ts->ws.addr == NULL || y + r < 0 || y + r >= ts->ws.height) {
         memset(d, 0, (size_t)w * 4);
         continue;
      }
      const uint32_t *s = (const uint32_t *)((char *)ts->ws.addr + (size_t)(y + r) * ts->ws.pitch);
      for (int i = 0; i < w; i++)
         d[i] = (x + i >= 0 && x + i < ts->ws.width) ? (s[x + i] | 0xff000000) : 0;
   }
}

static void
ts_get_image(struct dri_drawable *read, int x, int y, int w, int h, char *data,
             void *loaderPrivate)
{
   ts_get_image2(read, x, y, w, h, w * 4, data, loaderPrivate);
}

static const __DRIswrastLoaderExtension ts_swrast_loader_extension = {
   .base = {__DRI_SWRAST_LOADER, 3},
   .getDrawableInfo = ts_get_drawable_info,
   .putImage = ts_put_image,
   .getImage = ts_get_image,
   .putImage2 = ts_put_image2,
   .getImage2 = ts_get_image2,
};

/* ---------------------------------------------------------------- the images of the GPU's driver */

static int
ts_image_get_buffers(struct dri_drawable *driDrawable, unsigned int format,
                     uint32_t *stamp, void *loaderPrivate, uint32_t buffer_mask,
                     struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   struct ts_surface *ts = loaderPrivate;

   buffers->image_mask = 0;
   buffers->front = NULL;
   buffers->back = NULL;
   if (ts->window != NULL) {
      /* softpipe: buffers of the driver's own, put by the swrast loader */
      if (dri2_dpy->swrast)
         return 0;
      /* the V3D: a linear back buffer as large as the window, copied at the swap */
      if (ts->back) {
         int w = 0, h = 0;
         dri2_query_image(ts->back, __DRI_IMAGE_ATTRIB_WIDTH, &w);
         dri2_query_image(ts->back, __DRI_IMAGE_ATTRIB_HEIGHT, &h);
         if (w != dri2_surf->base.Width || h != dri2_surf->base.Height) {
            dri2_destroy_image(ts->back);
            ts->back = NULL;
         }
      }
      if (!ts->back) {
         int fd = -1;

         /*
          * The buffer the window system is handed: made shareable, and
          * exported once (a descriptor asked for and closed again), so
          * that the driver takes it as a buffer of another's from now
          * on. A buffer of the driver's own would be moved to new memory
          * at the first swap's flush, to be made fit for sharing, while
          * the state the frame's draws were set up with still names the
          * old one: they would draw there from the second frame on.
          */
         ts->back = dri_create_image(dri2_dpy->dri_screen_render_gpu,
                                     dri2_surf->base.Width, dri2_surf->base.Height,
                                     dri2_surf->visual, NULL, 0,
                                     __DRI_IMAGE_USE_SHARE | __DRI_IMAGE_USE_LINEAR |
                                     __DRI_IMAGE_USE_BACKBUFFER, NULL);
         if (!ts->back)
            return 0;
         if (dri2_query_image(ts->back, __DRI_IMAGE_ATTRIB_FD, &fd) && fd >= 0)
            close(fd);
         else
            ts_gl_log("egl: the window's back buffer could not be exported; "
                      "the driver may move it at the first swap\n");
      }
      if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
         buffers->image_mask |= __DRI_IMAGE_BUFFER_BACK;
         buffers->back = ts->back;
      }
      return 1;
   }
   /* a pbuffer is single buffered: its one buffer is the front */
   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      if (!dri2_surf->front) {
         dri2_surf->front = dri_create_image(dri2_dpy->dri_screen_render_gpu,
                                             dri2_surf->base.Width, dri2_surf->base.Height,
                                             dri2_surf->visual, NULL, 0, 0, NULL);
         if (!dri2_surf->front)
            return 0;
      }
      buffers->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
      buffers->front = dri2_surf->front;
   }
   return 1;
}

static void
ts_flush_front_buffer(struct dri_drawable *driDrawable, void *loaderPrivate)
{
}

static unsigned
ts_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   switch (cap) {
   case DRI_LOADER_CAP_FP16:
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static const __DRIimageLoaderExtension ts_image_loader_extension = {
   .base = {__DRI_IMAGE_LOADER, 2},
   .getBuffers = ts_image_get_buffers,
   .flushFrontBuffer = ts_flush_front_buffer,
   .getCapability = ts_get_capability,
};

static const __DRIextension *ts_image_loader_extensions[] = {
   &ts_image_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

/*
 * softpipe: a window's frame through the swrast loader, a pbuffer's one
 * buffer an image the driver makes (the image loader answers nothing for
 * a window, which then has buffers of the driver's own)
 */
static const __DRIextension *ts_swrast_loader_extensions[] = {
   &ts_swrast_loader_extension.base,
   &ts_image_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

/* ---------------------------------------------------------------- surfaces */

static _EGLSurface *
ts_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                  void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct ts_surface *ts;
   const struct dri_config *config;

   ts = calloc(1, sizeof *ts);
   if (!ts) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSurface");
      return NULL;
   }
   if (!dri2_init_surface(&ts->base.base, disp, type, conf, attrib_list, false,
                          native_window))
      goto cleanup;

   if (type == EGL_WINDOW_BIT) {
      ts->window = native_window;
      if (!ts_refresh(ts)) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "eglCreateWindowSurface");
         goto cleanup_init;
      }
   }

   config = dri2_get_dri_config(dri2_conf, type, ts->base.base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH, "Unsupported surfacetype/colorspace configuration");
      goto cleanup_init;
   }
   ts->base.visual = dri2_image_format_for_pbuffer_config(dri2_dpy, config);
   if (ts->base.visual == PIPE_FORMAT_NONE)
      goto cleanup_init;

   if (!dri2_create_drawable(dri2_dpy, config, &ts->base, ts))
      goto cleanup_init;
   /* the first validation asks for the size (ts_get_drawable_info) before it makes the buffers */
   dri_invalidate_drawable(ts->base.dri_drawable);

   if (type == EGL_WINDOW_BIT)
      ts->base.base.SwapInterval = 0;
   return &ts->base.base;

cleanup_init:
   dri2_fini_surface(&ts->base.base);
cleanup:
   free(ts);
   return NULL;
}

static _EGLSurface *
ts_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf, void *native_window,
                         const EGLint *attrib_list)
{
   return ts_create_surface(disp, EGL_WINDOW_BIT, conf, native_window, attrib_list);
}

static _EGLSurface *
ts_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf, const EGLint *attrib_list)
{
   return ts_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL, attrib_list);
}

static EGLBoolean
ts_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct ts_surface *ts = (struct ts_surface *)surf;

   if (ts->base.front) {
      dri2_destroy_image(ts->base.front);
      ts->base.front = NULL;
   }
   if (ts->back) {
      dri2_destroy_image(ts->back);
      ts->back = NULL;
   }
   driDestroyDrawable(ts->base.dri_drawable);
   dri2_fini_surface(surf);
   free(ts);
   return EGL_TRUE;
}

static EGLBoolean
ts_swap_buffers_with_damage(_EGLDisplay *disp, _EGLSurface *draw, const EGLint *rects,
                            EGLint n_rects)
{
   struct ts_surface *ts = (struct ts_surface *)draw;

   /* EGL's rectangles have their origin at the bottom left */
   ts->ndamage = 0;
   if (n_rects > 0 && n_rects <= TS_DAMAGE_MAX) {
      for (int i = 0; i < n_rects; i++) {
         ts->damage[i * 4] = rects[i * 4];
         ts->damage[i * 4 + 1] = draw->Height - rects[i * 4 + 1] - rects[i * 4 + 3];
         ts->damage[i * 4 + 2] = rects[i * 4 + 2];
         ts->damage[i * 4 + 3] = rects[i * 4 + 3];
      }
      ts->ndamage = n_rects;
   }
   driSwapBuffers(ts->base.dri_drawable);
   ts->ndamage = 0;
   return EGL_TRUE;
}

static EGLBoolean
ts_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   return ts_swap_buffers_with_damage(disp, draw, NULL, 0);
}

/*
 * The V3D's frame: the drawing flushed, the back buffer mapped (which
 * waits for the GPU) and copied into the window's pixels as softpipe's
 * frame is; the back buffer stays as it was, so the next frame starts
 * from this one. When the window changed size, the buffer follows at
 * the next frame.
 */
/*
 * What the GPU's frame held, for the record: a sum of its pixels, how
 * many are not black, the rectangles sent to the window. Told for the
 * first frames, every 500th, and once the frame has read back the same
 * 30 times in a row, so that a frame the GPU stopped drawing is told
 * apart from one drawn but not shown.
 */
static void
ts_frame_note(struct ts_surface *ts, const char *px, int w, int h, int stride)
{
   uint32_t sum = 2166136261u, first = 0;
   int lit = 0;
   char line[200];

   for (int r = 0; r < h; r++) {
      const uint32_t *s = (const uint32_t *)(px + (size_t)r * stride);
      for (int i = 0; i < w; i++) {
         uint32_t p = s[i] & 0x00ffffff;
         sum = (sum ^ p) * 16777619u;
         if (p != 0)
            lit++;
      }
   }
   if (w > 0 && h > 0)
      first = *(const uint32_t *)(px + (size_t)(h / 2) * stride + (size_t)(w / 2) * 4);
   ts->frames++;
   ts->same = (ts->frames > 1 && sum == ts->last_sum) ? ts->same + 1 : 0;
   ts->last_sum = sum;
   if (ts->frames <= 8 || ts->frames % 500 == 0 || ts->same == 30) {
      snprintf(line, sizeof(line), "egl: frame %u (%dx%d): sum %08x, %d of %d pixels not black, centre %08x, "
               "%d rects sent (%d), the same as the one before %u times in a row\n",
               ts->frames, w, h, sum, lit, w * h, first, ts->sent, ts->put_er, ts->same);
      ts_gl_log(line);
   }
}

static EGLBoolean
ts_gpu_swap_with_damage(_EGLDisplay *disp, _EGLSurface *draw, const EGLint *rects,
                        EGLint n_rects)
{
   struct ts_surface *ts = (struct ts_surface *)draw;
   _EGLContext *ctx = _eglGetCurrentContext();
   struct dri2_egl_context *dctx = ctx ? dri2_egl_context(ctx) : NULL;
   void *map_data = NULL;
   int stride = 0, w, h;
   char *px;

   if (ts->window == NULL || ts->back == NULL || dctx == NULL)
      return EGL_TRUE;
   dri_flush(dctx->dri_context, ts->base.dri_drawable,
             __DRI2_FLUSH_DRAWABLE | __DRI2_FLUSH_CONTEXT, __DRI2_THROTTLE_SWAPBUFFER);
   dri2_query_image(ts->back, __DRI_IMAGE_ATTRIB_WIDTH, &w);
   dri2_query_image(ts->back, __DRI_IMAGE_ATTRIB_HEIGHT, &h);
   px = dri2_map_image(dctx->dri_context, ts->back, 0, 0, w, h,
                       __DRI_IMAGE_TRANSFER_READ, &stride, &map_data);
   if (px) {
      ts->ndamage = 0;
      if (n_rects > 0 && n_rects <= TS_DAMAGE_MAX) {
         for (int i = 0; i < n_rects; i++) {
            ts->damage[i * 4] = rects[i * 4];
            ts->damage[i * 4 + 1] = h - rects[i * 4 + 1] - rects[i * 4 + 3];
            ts->damage[i * 4 + 2] = rects[i * 4 + 2];
            ts->damage[i * 4 + 3] = rects[i * 4 + 3];
         }
         ts->ndamage = n_rects;
      }
      ts_put_image2(NULL, __DRI_SWRAST_IMAGE_OP_SWAP, 0, 0, w, h, stride, px, ts);
      ts_frame_note(ts, px, w, h, stride);
      ts->ndamage = 0;
      dri2_unmap_image(dctx->dri_context, ts->back, map_data);
   }
   if (ts_refresh(ts) && (ts->ws.width != w || ts->ws.height != h))
      dri_invalidate_drawable(ts->base.dri_drawable);
   return EGL_TRUE;
}

static EGLBoolean
ts_gpu_swap(_EGLDisplay *disp, _EGLSurface *draw)
{
   return ts_gpu_swap_with_damage(disp, draw, NULL, 0);
}

static EGLint
ts_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surf)
{
   return driSWRastQueryBufferAge(((struct dri2_egl_surface *)surf)->dri_drawable);
}

static EGLBoolean
ts_query_surface(_EGLDisplay *disp, _EGLSurface *surf, EGLint attribute, EGLint *value)
{
   struct ts_surface *ts = (struct ts_surface *)surf;

   if ((attribute == EGL_WIDTH || attribute == EGL_HEIGHT) && ts->window != NULL) {
      int w = surf->Width, h = surf->Height;
      if (ts_refresh(ts) && (w != surf->Width || h != surf->Height))
         dri_invalidate_drawable(ts->base.dri_drawable);
   }
   return _eglQuerySurface(disp, surf, attribute, value);
}

static const struct dri2_egl_display_vtbl ts_swrast_display_vtbl = {
   .create_window_surface = ts_create_window_surface,
   .create_pbuffer_surface = ts_create_pbuffer_surface,
   .destroy_surface = ts_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_buffers = ts_swap_buffers,
   .swap_buffers_with_damage = ts_swap_buffers_with_damage,
   .query_buffer_age = ts_query_buffer_age,
   .query_surface = ts_query_surface,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static const struct dri2_egl_display_vtbl ts_gpu_display_vtbl = {
   .create_window_surface = ts_create_window_surface,
   .create_pbuffer_surface = ts_create_pbuffer_surface,
   .destroy_surface = ts_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_buffers = ts_gpu_swap,
   .swap_buffers_with_damage = ts_gpu_swap_with_damage,
   .query_surface = ts_query_surface,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

/* ---------------------------------------------------------------- the display */

/*
 * The configs: eight bits of red, green and blue at 16, 8 and 0 of a 32
 * bit pixel (the window's 0x00rrggbb), with or without alpha above them,
 * for windows and pbuffers alike. The driver's other formats (ten bits a
 * colour, half floats) are not offered: a program asking for eight bits
 * a colour would be given the larger ones first, which the window cannot
 * show and the software winsys cannot make buffers of.
 */
static void
ts_add_configs(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   EGLint surface_type = EGL_PBUFFER_BIT | EGL_WINDOW_BIT | EGL_SWAP_BEHAVIOR_PRESERVED_BIT;

   for (unsigned i = 0; dri2_dpy->driver_configs[i]; i++) {
      const struct dri_config *config = dri2_dpy->driver_configs[i];
      int shifts[4];
      unsigned int sizes[4];

      dri2_get_shifts_and_sizes(config, shifts, sizes);
      if (shifts[0] != 16 || shifts[1] != 8 || shifts[2] != 0 ||
          sizes[0] != 8 || sizes[1] != 8 || sizes[2] != 8 ||
          (sizes[3] != 0 && (sizes[3] != 8 || shifts[3] != 24)))
         continue;
      dri2_add_config(disp, config, surface_type, NULL);
   }
}

/*
 * A frame of 64 x 64 cleared on the GPU and read back: what the driver,
 * the address space, the jobs and their fences have to do for anything
 * to be drawn. True when every pixel came out as it was cleared to.
 */
#define TS_CHECK_SIZE   64

/*
 * The GPU as the console names it: the V3D of the Raspberry Pi, or on
 * a PC Intel's (iris)
 */
#ifdef __x86_64__
#define TS_GPU_NAME     "the GPU"
#else
#define TS_GPU_NAME     "the V3D"
#endif

/*
 * The frame of the check read back: the pixels that are not as cleared
 * (B, G, R, A: 0.75, 0.5, 0.25 and 1 of 255), -1 when it cannot be read.
 * 'what' gets the first pixels and the last, and how many different
 * words the frame holds, for the console.
 */
static int
ts_check_read(struct pipe_context *pipe, struct pipe_resource *res, char *what, size_t n, uint8_t first[4])
{
   struct pipe_transfer *tr = NULL;
   struct pipe_box box;
   const uint8_t *map;
   uint32_t seen[4];
   int bad = 0, nseen = 0, more = 0;

   u_box_2d(0, 0, TS_CHECK_SIZE, TS_CHECK_SIZE, &box);
   map = pipe->texture_map(pipe, res, 0, PIPE_MAP_READ, &box, &tr);
   if (!map) {
      snprintf(what, n, "not read");
      return -1;
   }
   for (int y = 0; y < TS_CHECK_SIZE; y++) {
      const uint8_t *p = map + (size_t)y * tr->stride;
      for (int x = 0; x < TS_CHECK_SIZE; x++, p += 4) {
         uint32_t w;
         int k;

         if (abs(p[0] - 191) > 1 || abs(p[1] - 128) > 1 || abs(p[2] - 64) > 1 || p[3] != 255)
            bad++;
         memcpy(&w, p, 4);
         for (k = 0; k < nseen && seen[k] != w; k++)
            ;
         if (k == nseen) {
            if (nseen < 4)
               seen[nseen++] = w;
            else
               more = 1;
         }
      }
   }
   {
      const uint32_t *w0 = (const uint32_t *)map;
      const uint32_t *wl = (const uint32_t *)(map + (size_t)(TS_CHECK_SIZE - 1) * tr->stride);

      memcpy(first, map, 4);

      snprintf(what, n, "%d wrong, stride %u, words %08x %08x %08x %08x .. %08x, %s%d kinds (%08x %08x %08x %08x)",
               bad, (unsigned)tr->stride, w0[0], w0[1], w0[2], w0[3], wl[TS_CHECK_SIZE - 1],
               more ? "over " : "", nseen, seen[0], nseen > 1 ? seen[1] : 0, nseen > 2 ? seen[2] : 0,
               nseen > 3 ? seen[3] : 0);
   }
   pipe->texture_unmap(pipe, tr);
   return bad;
}

/* A frame of the check, cleared and waited for: 0, or why not on the console's terms */
static int
ts_check_clear(struct pipe_screen *ps, struct pipe_context *pipe, bool linear, struct pipe_resource **out)
{
   struct pipe_resource templ, *res;
   struct pipe_surface surf;
   struct pipe_framebuffer_state fb;
   struct pipe_fence_handle *fence = NULL;
   union pipe_color_union c;
   int r = 0;

   *out = NULL;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   templ.width0 = TS_CHECK_SIZE;
   templ.height0 = TS_CHECK_SIZE;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   if (linear) {
      /* no tiling, no compression and no fast clear: the clear is drawn into the pixels */
      templ.bind |= PIPE_BIND_LINEAR;
      templ.usage = PIPE_USAGE_STAGING;
   }
   res = ps->resource_create(ps, &templ);
   if (!res)
      return 1;
   memset(&surf, 0, sizeof(surf));
   pipe_surface_init(pipe, &surf, res, 0, 0);
   c.f[0] = 0.25f;
   c.f[1] = 0.5f;
   c.f[2] = 0.75f;
   c.f[3] = 1.0f;
   /*
    * Cleared as glClear clears: the frame bound as the render target and
    * pipe->clear. Not pipe->clear_render_target, which iris takes as its
    * own surface (struct iris_surface, whose view the driver makes in
    * set_framebuffer_state) and so reads the view's format from the
    * pipe_surface's layers, 0: R32G32B32A32_FLOAT, the clear colour's
    * floats written as they are, 16 bytes a pixel, on past the frame.
    */
   memset(&fb, 0, sizeof(fb));
   fb.width = TS_CHECK_SIZE;
   fb.height = TS_CHECK_SIZE;
   fb.layers = 1;
   fb.nr_cbufs = 1;
   fb.cbufs[0] = surf;
   pipe->set_framebuffer_state(pipe, &fb);
   pipe->clear(pipe, PIPE_CLEAR_COLOR0, 0xf, 0, NULL, &c, 0.0, 0);
   memset(&fb, 0, sizeof(fb));
   pipe->set_framebuffer_state(pipe, &fb);
   pipe->flush(pipe, &fence, 0);
   if (!fence || !ps->fence_finish(ps, pipe, fence, 2000000000ull))
      r = 2;
   ps->fence_reference(ps, &fence, NULL);
   pipe_resource_reference(&surf.texture, NULL);
   *out = res;
   return r;
}

static bool
ts_gpu_check(struct pipe_screen *ps, const char *driver, char *why, size_t n)
{
   struct pipe_context *pipe = ps->context_create(ps, NULL, 0);
   struct pipe_resource *res = NULL;
   char what[200];
   uint8_t first[4];
   int bad, r;

   if (!pipe) {
      snprintf(why, n, "no context");
      return false;
   }
   r = ts_check_clear(ps, pipe, false, &res);
   if (r == 1) {
      pipe->destroy(pipe);
      snprintf(why, n, "no buffer");
      return false;
   }
   if (r == 2) {
      snprintf(why, n, "the frame did not end");
      bad = -1;
   } else {
      bad = ts_check_read(pipe, res, what, sizeof(what), first);
      if (bad < 0)
         snprintf(why, n, "the frame cannot be read");
      else if (bad > 0)
         snprintf(why, n, "%d pixels wrong (first row %02x %02x %02x %02x)", bad, first[0], first[1],
                  first[2], first[3]);
   }
#ifdef __x86_64__
   if (bad > 0) {
      /*
       * Where it goes wrong, on one line: the same frame read again after
       * 100 ms (a job told over before it was), and a frame without
       * tiling, compression or a fast clear, cleared and read the same
       * way (only the clear of a compressed frame and its resolve wrong)
       */
      char line[720], again[200], lin[200];
      uint8_t f2[4];
      struct pipe_resource *lres = NULL;
      int r2, lin_bad = -1;

      usleep(100000);
      (void)ts_check_read(pipe, res, again, sizeof(again), f2);
      r2 = ts_check_clear(ps, pipe, true, &lres);
      if (r2 == 1)
         snprintf(lin, sizeof(lin), "not made");
      else if (r2 == 2)
         snprintf(lin, sizeof(lin), "did not end");
      else
         lin_bad = ts_check_read(pipe, lres, lin, sizeof(lin), f2);
      pipe_resource_reference(&lres, NULL);
      snprintf(line, sizeof(line), "egl: the check: %s; again after 100 ms: %s; linear: %s\n", what, again, lin);
      ts_gl_log(line);

      /*
       * Intel's, for the record only: the colour compression (CCS) and the
       * fast clears turned off for a while (what INTEL_DEBUG noccs,nofc
       * does, read once by the driver at its first screen), and a tiled
       * frame made and checked again. Right then, it is the compressed
       * frame's clear, resolve or copy that goes wrong. The GPU is not
       * used so: the system's part has to be put right instead.
       */
      if (driver != NULL && strcmp(driver, "iris") == 0) {
         struct pipe_resource *res2 = NULL;
         int bad2 = -1;

         BITSET_SET(intel_debug, DEBUG_NO_CCS);
         BITSET_SET(intel_debug, DEBUG_NO_FAST_CLEAR);
         r2 = ts_check_clear(ps, pipe, false, &res2);
         if (r2 == 0)
            bad2 = ts_check_read(pipe, res2, again, sizeof(again), f2);
         else
            snprintf(again, sizeof(again), r2 == 1 ? "not made" : "did not end");
         pipe_resource_reference(&res2, NULL);
         snprintf(line, sizeof(line), "egl: the check with colour compression and fast clears off: %s (%s)\n",
                  again, bad2 == 0 ? "right" : "wrong too");
         ts_gl_log(line);
         BITSET_CLEAR(intel_debug, DEBUG_NO_CCS);
         BITSET_CLEAR(intel_debug, DEBUG_NO_FAST_CLEAR);
      }
   }
#endif
   pipe_resource_reference(&res, NULL);
   pipe->destroy(pipe);
   return bad == 0;
}

static bool
ts_probe_gpu(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   const char *want = os_get_option("TS_GL_DRIVER");
   char why[96], line[160];
   int64_t t0;

   struct pipe_loader_device *dev = NULL;

   ts_gl_probe_note(TS_GL_PROBE_NONE, NULL);
   if (disp->Options.ForceSoftware || (want != NULL && strcmp(want, "softpipe") == 0)) {
      ts_gl_log("egl: softpipe, as asked\n");
      return false;
   }
#ifdef __x86_64__
   if (ts_gl_gpu_kind() != TS_GL_GPU_V3D && ts_gl_gpu_kind() != TS_GL_GPU_INTEL)
#else
   if (ts_gl_gpu_kind() != TS_GL_GPU_V3D)
#endif
      return false;                  /* no GPU: softpipe, nothing to tell */
   dri2_dpy->fd_render_gpu = ts_gl_drm_open();
   if (dri2_dpy->fd_render_gpu < 0) {
      ts_gl_probe_note(TS_GL_PROBE_NOOPEN, NULL);
      ts_gl_log("egl: " TS_GPU_NAME " is not open to this process (no address space), softpipe\n");
      return false;
   }
   dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;
   dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd_render_gpu);
   ts_gl_probe_note(TS_GL_PROBE_NODRIVER, NULL);
   snprintf(why, sizeof(why), "no driver named for the descriptor");
   if (dri2_dpy->driver_name) {
      /*
       * The step the driver's screen is made by, tried alone first, so
       * that a failure says whether the loader did not take the
       * descriptor or the driver did not make its screen.
       */
      ts_gl_probe_note(TS_GL_PROBE_NOPROBE, dri2_dpy->driver_name);
      errno = 0;
      if (pipe_loader_drm_probe_fd(&dev, dri2_dpy->fd_render_gpu, false)) {
         pipe_loader_release(&dev, 1);
         ts_gl_probe_note(TS_GL_PROBE_NOSCREEN, dri2_dpy->driver_name);
         snprintf(why, sizeof(why), "the driver %s did not make its screen", dri2_dpy->driver_name);
      } else {
         snprintf(why, sizeof(why), "the loader of the driver %s did not take the descriptor (errno %d)",
                  dri2_dpy->driver_name, errno);
         goto fail;
      }
      dri2_detect_swrast_kopper(disp);
      dri2_dpy->loader_extensions = ts_image_loader_extensions;
      if (dri2_create_screen(disp)) {
         ts_gl_probe_note(TS_GL_PROBE_NODRAW, dri2_dpy->driver_name);
         t0 = os_time_get_nano();
         if (ts_gpu_check(dri2_dpy->dri_screen_render_gpu->base.screen, dri2_dpy->driver_name, why,
                          sizeof(why))) {
            snprintf(line, sizeof(line), "egl: " TS_GPU_NAME " (%s), a frame cleared and read back in %d ms\n",
                     dri2_dpy->driver_name, (int)((os_time_get_nano() - t0) / 1000000));
            ts_gl_log(line);
            ts_gl_probe_note(TS_GL_PROBE_DRAWS, dri2_dpy->driver_name);
            disp->Device = _eglFindDevice(-1, true);
            return true;
         }
         driDestroyScreen(dri2_dpy->dri_screen_render_gpu);
         dri2_dpy->dri_screen_render_gpu = NULL;
         dri2_dpy->dri_screen_display_gpu = NULL;
         dri2_dpy->own_dri_screen = false;
         if (dri2_dpy->driver_configs) {
            for (unsigned i = 0; dri2_dpy->driver_configs[i]; i++)
               free((struct dri_config *)dri2_dpy->driver_configs[i]);
            free(dri2_dpy->driver_configs);
            dri2_dpy->driver_configs = NULL;
         }
      }
   }
fail:
   snprintf(line, sizeof(line), "egl: " TS_GPU_NAME " did not draw (%s), softpipe\n", why);
   ts_gl_log(line);
#ifdef __x86_64__
   /* the requests of i915 the driver made and those refused: where it stopped */
   if (dri2_dpy->driver_name != NULL && strcmp(dri2_dpy->driver_name, "iris") == 0)
      ts_gl_drm_tell(dri2_dpy->driver_name);
#endif
   free(dri2_dpy->driver_name);
   dri2_dpy->driver_name = NULL;
   close(dri2_dpy->fd_render_gpu);
   dri2_dpy->fd_render_gpu = -1;
   dri2_dpy->fd_display_gpu = -1;
   return false;
}

static bool
ts_probe_sw(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   dri2_dpy->fd_render_gpu = -1;
   dri2_dpy->fd_display_gpu = -1;
   disp->Device = _eglFindDevice(-1, true);
   dri2_dpy->driver_name = strdup("swrast");
   if (!dri2_dpy->driver_name)
      return false;
   dri2_detect_swrast_kopper(disp);
   dri2_dpy->loader_extensions = ts_swrast_loader_extensions;
   if (!dri2_create_screen(disp)) {
      free(dri2_dpy->driver_name);
      dri2_dpy->driver_name = NULL;
      return false;
   }
   return true;
}

EGLBoolean
dri2_initialize_tessronos(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   bool gpu;

   gpu = ts_probe_gpu(disp);
   if (!gpu && !ts_probe_sw(disp))
      return _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to load driver");

   dri2_setup_screen(disp);
   if (dri2_dpy->swrast) {
      /* a frame is shown as soon as it is put: no interval to wait for */
      dri2_dpy->min_swap_interval = 0;
      dri2_dpy->max_swap_interval = 1;
      dri2_dpy->default_swap_interval = 0;
      disp->Extensions.EXT_buffer_age = EGL_TRUE;
      disp->Extensions.EXT_swap_buffers_with_damage = EGL_TRUE;
   }
   ts_add_configs(disp);

   /* filled last, so that nothing calls through it while it is set up */
   dri2_dpy->vtbl = gpu ? &ts_gpu_display_vtbl : &ts_swrast_display_vtbl;
   return EGL_TRUE;
}
