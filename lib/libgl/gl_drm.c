/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gl_drm.c
 *	The DRM shim: the calls of the DRM user library that Mesa's drivers
 *	make (tools/mesa/shim/xf86drm.h), given to the GPU SVCs of a process
 *	(gp_, include/ts/gpuapp.h; docs/tessronos/13-gpu.md 13.6.2).
 *
 *	A descriptor of the GPU (GPUFD_BASE up) is a context of gp_. The
 *	V3D's requests (DRM_IOCTL_V3D_*) go to gp_ one to one: a BO's number
 *	is the context's number of it, a job's BOs are named by those
 *	numbers, and its fence is kept in the synchronisation object the
 *	request names. The synchronisation objects are the shim's own: a
 *	fence number, or signalled; waiting on one is waiting for its fence.
 *	One is turned into a sync_file descriptor (SYNCFD_BASE up) that
 *	carries its fence as it was, and back (drmSyncobjExportSyncFile,
 *	drmSyncobjImportSyncFile): the driver v3d keeps the fences of its
 *	frames so. The driver is told the GPU takes no several
 *	synchronisation objects at once (DRM_V3D_PARAM_SUPPORTS_MULTISYNC_EXT
 *	is 0).
 *
 *	A BO is mapped by an mmap of the GPU's descriptor at the offset
 *	DRM_IOCTL_V3D_MMAP_BO answered; the mapping is that of gp_map_bo and
 *	stays while the BO does, so munmap of it does nothing. A BO is
 *	handed to another process as a "PRIME" descriptor (PRIMEFD_BASE up)
 *	that carries the name of gp_shr_bo, or by that name itself
 *	(DRM_IOCTL_GEM_FLINK, DRM_IOCTL_GEM_OPEN).
 *
 *	There is no device to enumerate: drmGetDevices2 finds none, so the
 *	EGL platforms of Linux find nothing, and the platform tessronos
 *	opens the GPU itself (ts_gl_drm_open) when the machine has a V3D.
 *	Asked about a descriptor it opened (drmGetDevice2), the shim
 *	answers the V3D as a platform device with a render node.
 *	The libcxxrt calls close, fcntl and mmap come here for the GPU's
 *	descriptors (lib/libcxxrt: ts_gpu_fd_*).
 */

#include <config.h>		/* first: the core's types as wide as the kernel's */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/gpuapp.h>
#include "xf86drm.h"
#include "drm-uapi/drm.h"
#include "drm-uapi/v3d_drm.h"
#include "ts_glport.h"
#ifdef __x86_64__
#include "drm-uapi/i915_drm.h"
#endif

#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC	14		/* newlib's, hidden by a strict standard */
#endif

/*
 * The descriptors of the shim: 0x3400 to 0x357f, between the eventfds
 * (0x3000 up, lib/libcxxrt/ts_evfd.c) and the random sources (0x3800 up,
 * ts_urandom.c). libcxxrt asks for the memory objects (0x3c00 to 0x3fff,
 * ts_mman.c) before it asks the shim, so a descriptor in their range
 * never reaches it: close, fcntl and mmap of one would fail with EBADF.
 */
#define GPUFD_BASE	0x3400
#define GPUFD_MAX	64
#define PRIMEFD_BASE	( GPUFD_BASE + GPUFD_MAX )
#define PRIMEFD_MAX	64
#define SYNCFD_BASE	( PRIMEFD_BASE + PRIMEFD_MAX )
#define SYNCFD_MAX	256
#define GPUFD_END	( SYNCFD_BASE + SYNCFD_MAX )
#if GPUFD_END > 0x3800
#error "the GPU's descriptors reach the random sources"
#endif
#define SYNC_MAX	1024
#define MMAP_SHIFT	12		/* the offset of a BO's mmap: its number in pages */

typedef struct {
	uint32_t	offset;
	uint32_t	size;
	void		*addr;		/* where it is mapped, NULL before */
	void		*mem;		/* a pretended BO's memory (ts_gl_drm_fake) */
#ifdef __x86_64__
	uint64_t	va;		/* Intel: where it is placed in the context's space, 0 nowhere */
	uint32_t	tiling, stride;	/* Intel: what the driver said of its tiling */
#endif
} BOENT;

typedef struct {
	int		ctx;		/* of gp_; 0: free */
	int		refs;		/* descriptors on it */
	int		fake;		/* a pretended V3D: no gp_ behind it */
	uint32_t	nbo;
	BOENT		*bo;		/* by number */
	uint32_t	next_bo;	/* a pretended V3D's last BO number */
	uint32_t	next_va;	/* and where its next BO is in the GPU's space */
#ifdef __x86_64__
	int		intel;		/* Intel's GPU (gl_drm_i915.c): its requests are i915's */
	struct i915_fd	*i9;
#endif
} GCTX;

typedef struct {
	int		used;
	int		signaled;
	D		fence;		/* the job it waits for, 0 none */
} SYNC;

/* A sync_file descriptor: a fence carried as a descriptor */
typedef struct {
	int		used;
	int		signaled;
	D		fence;
	int		ctx;		/* of gp_, to wait with; 0 pretended */
} SYNCFD;

/*
 * A pretended V3D (ts_gl_drm_fake), for the tests of a machine without
 * one: the loader, the driver's choice and its screen are made as on
 * the Raspberry Pi 5, against BOs in the process's own memory, and a
 * job ends at once without running. FAKE_CTX numbers its contexts.
 */
#define FAKE_CTX	0x40000000
#define FAKE_VA		0x00100000

static int		fake_on;
static int		fake_last;

#ifdef __x86_64__
/* Intel's GPU (gl_drm_i915.c, read in at the end of this file) */
#define FAKE_INTEL	2		/* ts_gl_drm_fake(2): a pretended Intel GPU */
struct i915_fd;
static int	i915_open( GCTX *c );
static void	i915_close( GCTX *c );
static int	i915_ioctl( int fd, int ctx, unsigned long request, void *arg );
static int	fd_intel( int fd );
static int	i915_device( int fd, drmDevicePtr *device );
#endif

static const uint32_t	fake_core[3] = { 0x07443356, 0x81001441, 0xc0078101 };
static const uint32_t	fake_hub[4] = { 0x42554856, 0x00081117, 0x00001900, 0x00000700 };

static pthread_mutex_t	mu = PTHREAD_MUTEX_INITIALIZER;
static int		gfd[GPUFD_MAX];		/* the GCTX index + 1; 0: free */
static GCTX		gctx[GPUFD_MAX];
static int		pfd[PRIMEFD_MAX];	/* the name a PRIME descriptor carries; 0: free */
static SYNC		sync_tab[SYNC_MAX];
static SYNCFD		sfd_tab[SYNCFD_MAX];

static SYNCFD *sfd_of( int fd )
{
	int i = fd - SYNCFD_BASE;

	if ( i < 0 || i >= SYNCFD_MAX || !sfd_tab[i].used ) {
		return NULL;
	}
	return &sfd_tab[i];
}

/* A new sync_file descriptor; with the lock held */
static int sfd_new( int signaled, D fence, int ctx )
{
	int i;

	for ( i = 0; i < SYNCFD_MAX; i++ ) {
		if ( !sfd_tab[i].used ) {
			sfd_tab[i].used = 1;
			sfd_tab[i].signaled = signaled;
			sfd_tab[i].fence = fence;
			sfd_tab[i].ctx = ctx;
			return SYNCFD_BASE + i;
		}
	}
	errno = EMFILE;
	return -1;
}

/*
 * E_DISWAI from a call of the GPU: the process is ending (_exit,
 * ts_ter_prc). The system lets none of its threads into the GPU's calls
 * from then on, and cuts the waits they are in. A thread still running
 * the driver then (a raster or WebGL thread while the main thread calls
 * _exit) does not go back to it with an error, which the driver would
 * report and abort on (v3d_bo_wait: "wait failed: -4"): it stays here
 * until the system takes it down with the process.
 */
static void ending_stay( void )
{
	struct timespec	t = { 1, 0 };

	for ( ;; ) {
		(void)nanosleep(&t, NULL);
	}
}

static int set_errno( ER er )
{
	if ( er == E_DISWAI ) {
		ending_stay();
	}
	switch ( er ) {
	case E_NOMEM:	errno = ENOMEM;	break;
	case E_ID:	errno = ENOENT;	break;
	case E_TMOUT:	errno = ETIME;	break;
	case E_IO:	errno = EIO;	break;
	case E_NOEXS:	errno = ENODEV;	break;
	case E_LIMIT:	errno = ENOSPC;	break;
	case E_NOSPT:	errno = ENOSYS;	break;
	default:	errno = EINVAL;	break;
	}
	return -1;
}

/* The context of a GPU descriptor; with the lock held */
static GCTX *ctx_of( int fd )
{
	int i = fd - GPUFD_BASE;

	if ( i < 0 || i >= GPUFD_MAX || gfd[i] == 0 ) {
		return NULL;
	}
	return &gctx[gfd[i] - 1];
}

static int fd_ctx( int fd )
{
	GCTX	*c;
	int	ctx = -1;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL ) {
		ctx = c->ctx;
	}
	pthread_mutex_unlock(&mu);
	return ctx;
}

/* A new descriptor on the context; with the lock held */
static int fd_new( int ci )
{
	int i;

	for ( i = 0; i < GPUFD_MAX; i++ ) {
		if ( gfd[i] == 0 ) {
			gfd[i] = ci + 1;
			gctx[ci].refs++;
			return GPUFD_BASE + i;
		}
	}
	errno = EMFILE;
	return -1;
}

void ts_gl_drm_fake( int on )
{
	fake_on = on;
}

int ts_gl_drm_faked( void )
{
	return fake_on;
}

int ts_gl_drm_open( void )
{
	int	ctx, i, fd = -1;

	if ( fake_on ) {
		ctx = FAKE_CTX + ++fake_last;
	} else {
#ifdef __x86_64__
		if ( ts_gl_gpu_kind() != TS_GL_GPU_V3D && ts_gl_gpu_kind() != TS_GL_GPU_INTEL ) {
#else
		if ( ts_gl_gpu_kind() != TS_GL_GPU_V3D ) {
#endif
			return -1;
		}
		ctx = gp_opn_gpu();
		if ( ctx <= 0 ) {
			return -1;
		}
	}
	pthread_mutex_lock(&mu);
	for ( i = 0; i < GPUFD_MAX; i++ ) {
		if ( gctx[i].ctx == 0 ) {
			gctx[i].ctx = ctx;
			gctx[i].refs = 0;
			gctx[i].fake = fake_on;
			gctx[i].nbo = 0;
			gctx[i].bo = NULL;
			gctx[i].next_bo = 0;
			gctx[i].next_va = FAKE_VA;
#ifdef __x86_64__
			gctx[i].intel = ( fake_on == FAKE_INTEL || ( !fake_on && ts_gl_gpu_kind() == TS_GL_GPU_INTEL ) );
			gctx[i].i9 = NULL;
			if ( gctx[i].intel && i915_open(&gctx[i]) < 0 ) {
				gctx[i].ctx = 0;
				break;
			}
#endif
			fd = fd_new(i);
			if ( fd < 0 ) {
				gctx[i].ctx = 0;
			}
			break;
		}
	}
	pthread_mutex_unlock(&mu);
	if ( fd < 0 && !fake_on ) {
		(void)gp_cls_gpu(ctx);
	}
	return fd;
}

/* Whether a descriptor is of a pretended V3D */
static int fd_fake( int fd )
{
	GCTX	*c;
	int	f = 0;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL ) {
		f = c->fake;
	}
	pthread_mutex_unlock(&mu);
	return f;
}

/* What the shim knows of a BO, kept by number; with the lock held */
static BOENT *bo_ent( GCTX *c, uint32_t handle, int make )
{
	if ( handle >= c->nbo ) {
		uint32_t	n;
		BOENT		*b;

		if ( !make ) {
			return NULL;
		}
		n = ( c->nbo == 0 ) ? 256 : c->nbo;
		while ( n <= handle ) {
			n *= 2;
		}
		b = realloc(c->bo, n * sizeof(BOENT));
		if ( b == NULL ) {
			return NULL;
		}
		memset(b + c->nbo, 0, ( n - c->nbo ) * sizeof(BOENT));
		c->bo = b;
		c->nbo = n;
	}
	return &c->bo[handle];
}

static void bo_note( int fd, const T_GPBO *b )
{
	GCTX	*c;
	BOENT	*e;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL && ( e = bo_ent(c, b->handle, 1) ) != NULL ) {
		e->offset = b->offset;
		e->size = (uint32_t)b->size;
		e->addr = NULL;
	}
	pthread_mutex_unlock(&mu);
}

static void bo_forget( int fd, uint32_t handle )
{
	GCTX	*c;
	BOENT	*e;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL && ( e = bo_ent(c, handle, 0) ) != NULL ) {
		memset(e, 0, sizeof(*e));
	}
	pthread_mutex_unlock(&mu);
}

/* ---------------------------------------------------------------- descriptors */

int ts_gpu_fd_close( int fd )
{
	GCTX	*c;
	int	ctx = 0, i = fd - PRIMEFD_BASE;

	pthread_mutex_lock(&mu);
	if ( i >= 0 && i < PRIMEFD_MAX && pfd[i] != 0 ) {
		pfd[i] = 0;
		pthread_mutex_unlock(&mu);
		return 0;
	}
	if ( sfd_of(fd) != NULL ) {
		sfd_of(fd)->used = 0;
		pthread_mutex_unlock(&mu);
		return 0;
	}
	c = ctx_of(fd);
	if ( c == NULL ) {
		pthread_mutex_unlock(&mu);
		return -1;
	}
	gfd[fd - GPUFD_BASE] = 0;
	if ( --c->refs == 0 ) {
		uint32_t	j;

		ctx = c->fake ? 0 : c->ctx;
		for ( j = 0; j < c->nbo; j++ ) {
			free(c->bo[j].mem);
		}
		free(c->bo);
		c->bo = NULL;
		c->nbo = 0;
		c->ctx = 0;
#ifdef __x86_64__
		i915_close(c);
#endif
	}
	pthread_mutex_unlock(&mu);
	if ( ctx > 0 ) {
		(void)gp_cls_gpu(ctx);
	}
	return 0;
}

int ts_gpu_fd_fcntl( int fd, int cmd, int arg )
{
	GCTX	*c;
	int	r = 0;

	(void)arg;
	pthread_mutex_lock(&mu);
	if ( sfd_of(fd) != NULL ) {
		SYNCFD *s = sfd_of(fd);

		r = ( cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC ) ? sfd_new(s->signaled, s->fence, s->ctx)
		  : ( cmd == F_GETFL ) ? O_RDWR : 0;
		pthread_mutex_unlock(&mu);
		return r;
	}
	c = ctx_of(fd);
	if ( c == NULL ) {
		pthread_mutex_unlock(&mu);
		errno = EBADF;
		return -1;
	}
	switch ( cmd ) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:
		r = fd_new((int)( c - gctx ));
		break;
	case F_GETFL:
		r = O_RDWR;
		break;
	default:
		r = 0;
		break;
	}
	pthread_mutex_unlock(&mu);
	return r;
}

void *ts_gpu_fd_mmap( int fd, size_t len, off_t off )
{
	GCTX		*c;
	BOENT		*e;
	uint32_t	handle = (uint32_t)( (uint64_t)off >> MMAP_SHIFT );
	void		*addr = NULL;
	int		ctx;
	ER		er;

	(void)len;
	ctx = fd_ctx(fd);
	if ( ctx <= 0 ) {
		errno = EBADF;
		return MAP_FAILED;
	}
	if ( fd_fake(fd) ) {
		pthread_mutex_lock(&mu);
		c = ctx_of(fd);
		if ( c != NULL && ( e = bo_ent(c, handle, 0) ) != NULL ) {
			addr = e->addr = e->mem;
		}
		pthread_mutex_unlock(&mu);
		if ( addr == NULL ) {
			errno = EINVAL;
			return MAP_FAILED;
		}
		return addr;
	}
	er = gp_map_bo(ctx, handle, &addr);
	if ( er < E_OK ) {
		(void)set_errno(er);
		return MAP_FAILED;
	}
	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL && ( e = bo_ent(c, handle, 1) ) != NULL ) {
		e->addr = addr;
	}
	pthread_mutex_unlock(&mu);
	return addr;
}

int ts_gpu_fd_munmap( void *addr, size_t len )
{
	uint32_t	i, j;
	int		found = 0;

	(void)len;
	pthread_mutex_lock(&mu);
	for ( i = 0; i < GPUFD_MAX && !found; i++ ) {
		for ( j = 0; gctx[i].ctx != 0 && j < gctx[i].nbo; j++ ) {
			if ( gctx[i].bo[j].addr == addr ) {
				found = 1;
				break;
			}
		}
	}
	pthread_mutex_unlock(&mu);
	return found ? 0 : -1;
}

/* ---------------------------------------------------------------- synchronisation objects */

static SYNC *sync_of( uint32_t handle )
{
	if ( handle == 0 || handle > SYNC_MAX || !sync_tab[handle - 1].used ) {
		return NULL;
	}
	return &sync_tab[handle - 1];
}

int drmSyncobjCreate( int fd, uint32_t flags, uint32_t *handle )
{
	int i;

	(void)fd;
	pthread_mutex_lock(&mu);
	for ( i = 0; i < SYNC_MAX; i++ ) {
		if ( !sync_tab[i].used ) {
			sync_tab[i].used = 1;
			sync_tab[i].fence = 0;
			sync_tab[i].signaled = ( flags & DRM_SYNCOBJ_CREATE_SIGNALED ) != 0;
			*handle = (uint32_t)( i + 1 );
			pthread_mutex_unlock(&mu);
			return 0;
		}
	}
	pthread_mutex_unlock(&mu);
	errno = ENOSPC;
	return -1;
}

int drmSyncobjDestroy( int fd, uint32_t handle )
{
	SYNC	*s;

	(void)fd;
	pthread_mutex_lock(&mu);
	s = sync_of(handle);
	if ( s != NULL ) {
		s->used = 0;
	}
	pthread_mutex_unlock(&mu);
	if ( s == NULL ) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* The fence a synchronisation object is to be signalled by; with the lock let go of */
static int sync_get( uint32_t handle, int *signaled, D *fence )
{
	SYNC	*s;

	pthread_mutex_lock(&mu);
	s = sync_of(handle);
	if ( s != NULL ) {
		*signaled = s->signaled;
		*fence = s->fence;
	}
	pthread_mutex_unlock(&mu);
	return ( s != NULL ) ? 0 : -1;
}

static void sync_set( uint32_t handle, int signaled, D fence )
{
	SYNC	*s;

	pthread_mutex_lock(&mu);
	s = sync_of(handle);
	if ( s != NULL ) {
		s->signaled = signaled;
		s->fence = fence;
	}
	pthread_mutex_unlock(&mu);
}

static int64_t mono_ns( void )
{
	struct timespec	t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/*
 * Every object waited for (the GPU runs its jobs in order, so waiting for
 * any one of them is waiting for the earliest). timeout_nsec is the
 * moment it gives up, on the monotonic clock.
 */
int drmSyncobjWait( int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
		    unsigned flags, uint32_t *first_signaled )
{
	unsigned	i;
	int		signaled, ctx = fd_ctx(fd);
	D		fence;
	ER		er;

	(void)flags;
	for ( i = 0; i < num_handles; i++ ) {
		int64_t left = timeout_nsec - mono_ns();
		TMO_U	tmo;

		if ( sync_get(handles[i], &signaled, &fence) < 0 ) {
			errno = EINVAL;
			return -1;
		}
		if ( signaled ) {
			continue;
		}
		if ( fence <= 0 || ctx <= 0 ) {
			errno = ( left <= 0 ) ? ETIME : EINVAL;
			return -1;
		}
		tmo = ( timeout_nsec == INT64_MAX ) ? TMO_FEVR : ( left <= 0 ) ? 0 : (TMO_U)( left / 1000 );
		er = gp_wai_fen(ctx, fence, tmo);
		if ( er < E_OK ) {
			return set_errno(er);
		}
		sync_set(handles[i], 1, fence);
	}
	if ( first_signaled != NULL && num_handles > 0 ) {
		*first_signaled = 0;
	}
	return 0;
}

int drmSyncobjReset( int fd, const uint32_t *handles, uint32_t handle_count )
{
	uint32_t i;

	(void)fd;
	for ( i = 0; i < handle_count; i++ ) {
		sync_set(handles[i], 0, 0);
	}
	return 0;
}

int drmSyncobjSignal( int fd, const uint32_t *handles, uint32_t handle_count )
{
	uint32_t i;

	(void)fd;
	for ( i = 0; i < handle_count; i++ ) {
		sync_set(handles[i], 1, 0);
	}
	return 0;
}

int drmSyncobjTransfer( int fd, uint32_t dst_handle, uint64_t dst_point,
			uint32_t src_handle, uint64_t src_point, uint32_t flags )
{
	int	signaled;
	D	fence;

	(void)fd;
	(void)flags;
	if ( dst_point != 0 || src_point != 0 || sync_get(src_handle, &signaled, &fence) < 0 ) {
		errno = EINVAL;
		return -1;
	}
	sync_set(dst_handle, signaled, fence);
	return 0;
}

/* No sync_file and no timelines on TessronOS */
int drmSyncobjHandleToFD( int fd, uint32_t handle, int *obj_fd )
{
	(void)fd; (void)handle; (void)obj_fd;
	errno = EINVAL;
	return -1;
}

int drmSyncobjFDToHandle( int fd, int obj_fd, uint32_t *handle )
{
	(void)fd; (void)obj_fd; (void)handle;
	errno = EINVAL;
	return -1;
}

/* The object takes the fence of a sync_file descriptor */
int drmSyncobjImportSyncFile( int fd, uint32_t handle, int sync_file_fd )
{
	SYNCFD	*s;
	int	signaled = 0, was;
	D	fence = 0, before;

	(void)fd;
	pthread_mutex_lock(&mu);
	s = sfd_of(sync_file_fd);
	if ( s != NULL ) {
		signaled = s->signaled;
		fence = s->fence;
	}
	pthread_mutex_unlock(&mu);
	if ( s == NULL || sync_get(handle, &was, &before) < 0 ) {
		errno = EINVAL;
		return -1;
	}
	sync_set(handle, signaled, fence);
	return 0;
}

/* A sync_file descriptor carrying the object's fence as it is now */
int drmSyncobjExportSyncFile( int fd, uint32_t handle, int *sync_file_fd )
{
	int	signaled, ctx = fd_fake(fd) ? 0 : fd_ctx(fd), r;
	D	fence;

	if ( sync_get(handle, &signaled, &fence) < 0 ) {
		errno = EINVAL;
		return -1;
	}
	if ( fence <= 0 && !signaled ) {
		errno = EINVAL;			/* nothing submitted to signal it */
		return -1;
	}
	pthread_mutex_lock(&mu);
	r = sfd_new(signaled, fence, ctx < 0 ? 0 : ctx);
	pthread_mutex_unlock(&mu);
	if ( r < 0 ) {
		return -1;
	}
	*sync_file_fd = r;
	return 0;
}

/* ---------------------------------------------------------------- sync_file (tools/mesa/shim/libsync.h) */

int ts_gl_sync_wait( int fd, int timeout )
{
	SYNCFD	*s;
	int	signaled = 0, ctx = 0;
	D	fence = 0;
	ER	er;

	pthread_mutex_lock(&mu);
	s = sfd_of(fd);
	if ( s != NULL ) {
		signaled = s->signaled;
		fence = s->fence;
		ctx = s->ctx;
	}
	pthread_mutex_unlock(&mu);
	if ( s == NULL ) {
		errno = EINVAL;
		return -1;
	}
	if ( signaled || fence <= 0 || ctx <= 0 ) {
		return 0;
	}
	er = gp_wai_fen(ctx, fence, ( timeout < 0 ) ? TMO_FEVR : (TMO_U)timeout * 1000);
	if ( er == E_TMOUT ) {
		errno = ETIME;
		return -1;
	}
	return ( er < E_OK ) ? set_errno(er) : 0;
}

/* The fences are numbered in the order the GPU runs them: the later one stands for both */
int ts_gl_sync_merge( int fd1, int fd2 )
{
	SYNCFD	*a, *b;
	int	r = -1;

	pthread_mutex_lock(&mu);
	a = sfd_of(fd1);
	b = sfd_of(fd2);
	if ( a != NULL && b != NULL ) {
		SYNCFD	*later = ( b->signaled || ( !a->signaled && a->fence >= b->fence ) ) ? a : b;

		r = sfd_new(a->signaled && b->signaled, later->fence, later->ctx);
	} else {
		errno = EINVAL;
	}
	pthread_mutex_unlock(&mu);
	return r;
}

int ts_gl_sync_accumulate( int *fd1, int fd2 )
{
	int r;

	if ( *fd1 < 0 ) {
		r = ts_gpu_fd_fcntl(fd2, F_DUPFD_CLOEXEC, 3);
	} else {
		r = ts_gl_sync_merge(*fd1, fd2);
		if ( r >= 0 ) {
			(void)ts_gpu_fd_close(*fd1);
		}
	}
	if ( r < 0 ) {
		return -1;
	}
	*fd1 = r;
	return 0;
}

int drmSyncobjTimelineSignal( int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count )
{
	(void)fd; (void)handles; (void)points; (void)handle_count;
	errno = EINVAL;
	return -1;
}

int drmSyncobjTimelineWait( int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
			    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled )
{
	(void)fd; (void)handles; (void)points; (void)num_handles;
	(void)timeout_nsec; (void)flags; (void)first_signaled;
	errno = EINVAL;
	return -1;
}

int drmSyncobjQuery( int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count )
{
	(void)fd; (void)handles; (void)points; (void)handle_count;
	errno = EINVAL;
	return -1;
}

int drmSyncobjQuery2( int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
		      uint32_t flags )
{
	(void)fd; (void)handles; (void)points; (void)handle_count; (void)flags;
	errno = EINVAL;
	return -1;
}

/* ---------------------------------------------------------------- the V3D's requests */

static int v3d_get_param( struct drm_v3d_get_param *p )
{
	T_GPUINFO	i;

	if ( gp_ref_gpu(&i) < E_OK ) {
		errno = ENODEV;
		return -1;
	}
	switch ( p->param ) {
	case DRM_V3D_PARAM_V3D_UIFCFG:		p->value = 0;			break;
	case DRM_V3D_PARAM_V3D_HUB_IDENT1:	p->value = i.hub_ident[1];	break;
	case DRM_V3D_PARAM_V3D_HUB_IDENT2:	p->value = i.hub_ident[2];	break;
	case DRM_V3D_PARAM_V3D_HUB_IDENT3:	p->value = i.hub_ident[3];	break;
	case DRM_V3D_PARAM_V3D_CORE0_IDENT0:	p->value = i.core_ident[0];	break;
	case DRM_V3D_PARAM_V3D_CORE0_IDENT1:	p->value = i.core_ident[1];	break;
	case DRM_V3D_PARAM_V3D_CORE0_IDENT2:	p->value = i.core_ident[2];	break;
	case DRM_V3D_PARAM_SUPPORTS_TFU:	p->value = ( i.feature & GPU_FEAT_TFU ) != 0;	break;
	case DRM_V3D_PARAM_SUPPORTS_CSD:	p->value = ( i.feature & GPU_FEAT_CSD ) != 0;	break;
	case DRM_V3D_PARAM_SUPPORTS_CACHE_FLUSH: p->value = ( i.feature & GPU_FEAT_CACHE ) != 0; break;
	default:
		p->value = 0;			/* perfmon, multisync, the CPU queue, super pages */
		break;
	}
	return 0;
}

/* The fence a job waits for: the latest of the objects named that has one */
static D wait_for( uint32_t a, uint32_t b )
{
	int	signaled;
	D	fence, after = 0;

	if ( a != 0 && sync_get(a, &signaled, &fence) == 0 && !signaled && fence > after ) {
		after = fence;
	}
	if ( b != 0 && sync_get(b, &signaled, &fence) == 0 && !signaled && fence > after ) {
		after = fence;
	}
	return after;
}

static int submit( int ctx, T_GPUJOB *job, const uint32_t *handles, int n, uint32_t out_sync )
{
	D fence = gp_sub_job(ctx, job, (CONST UINT *)handles, n);

	if ( fence <= 0 ) {
		return set_errno((ER)fence);
	}
	if ( out_sync != 0 ) {
		sync_set(out_sync, 0, fence);
	}
	return 0;
}

static int v3d_submit_cl( int ctx, struct drm_v3d_submit_cl *s )
{
	T_GPUJOB	j;

	if ( s->extensions != 0 ) {
		errno = EINVAL;
		return -1;
	}
	memset(&j, 0, sizeof(j));
	j.kind = GPU_JOB_CL;
	j.after = wait_for(s->in_sync_bcl, s->in_sync_rcl);
	j.u.cl.bcl_start = s->bcl_start;
	j.u.cl.bcl_end = s->bcl_end;
	j.u.cl.rcl_start = s->rcl_start;
	j.u.cl.rcl_end = s->rcl_end;
	j.u.cl.qma = s->qma;
	j.u.cl.qms = s->qms;
	j.u.cl.qts = s->qts;
	j.u.cl.flags = ( s->flags & DRM_V3D_SUBMIT_CL_FLUSH_CACHE ) ? GPU_CL_FLUSH_CACHE : 0;
	return submit(ctx, &j, (const uint32_t *)(uintptr_t)s->bo_handles, (int)s->bo_handle_count,
		      s->out_sync);
}

static int v3d_submit_tfu( int ctx, struct drm_v3d_submit_tfu *s )
{
	T_GPUJOB	j;
	uint32_t	h[4];
	int		i, n = 0;

	if ( s->extensions != 0 ) {
		errno = EINVAL;
		return -1;
	}
	memset(&j, 0, sizeof(j));
	j.kind = GPU_JOB_TFU;
	j.after = wait_for(s->in_sync, 0);
	j.u.tfu.icfg = s->icfg;
	j.u.tfu.iia = s->iia;
	j.u.tfu.iis = s->iis;
	j.u.tfu.ica = s->ica;
	j.u.tfu.iua = s->iua;
	j.u.tfu.ioa = s->ioa;
	j.u.tfu.ioc = s->v71.ioc;
	j.u.tfu.ios = s->ios;
	for ( i = 0; i < 4; i++ ) {
		j.u.tfu.coef[i] = s->coef[i];
		if ( s->bo_handles[i] != 0 ) {
			h[n++] = s->bo_handles[i];
		}
	}
	return submit(ctx, &j, h, n, s->out_sync);
}

static int v3d_submit_csd( int ctx, struct drm_v3d_submit_csd *s )
{
	T_GPUJOB	j;
	int		i;

	if ( s->extensions != 0 ) {
		errno = EINVAL;
		return -1;
	}
	memset(&j, 0, sizeof(j));
	j.kind = GPU_JOB_CSD;
	j.after = wait_for(s->in_sync, 0);
	for ( i = 0; i < 7; i++ ) {
		j.u.csd.cfg[i] = s->cfg[i];
	}
	for ( i = 0; i < 4; i++ ) {
		j.u.csd.coef[i] = s->coef[i];
	}
	return submit(ctx, &j, (const uint32_t *)(uintptr_t)s->bo_handles, (int)s->bo_handle_count,
		      s->out_sync);
}

/* GET_PARAM of the pretended V3D: the Raspberry Pi 5's registers */
static int fake_get_param( struct drm_v3d_get_param *p )
{
	switch ( p->param ) {
	case DRM_V3D_PARAM_V3D_HUB_IDENT1:	p->value = fake_hub[1];		break;
	case DRM_V3D_PARAM_V3D_HUB_IDENT2:	p->value = fake_hub[2];		break;
	case DRM_V3D_PARAM_V3D_HUB_IDENT3:	p->value = fake_hub[3];		break;
	case DRM_V3D_PARAM_V3D_CORE0_IDENT0:	p->value = fake_core[0];	break;
	case DRM_V3D_PARAM_V3D_CORE0_IDENT1:	p->value = fake_core[1];	break;
	case DRM_V3D_PARAM_V3D_CORE0_IDENT2:	p->value = fake_core[2];	break;
	case DRM_V3D_PARAM_SUPPORTS_TFU:
	case DRM_V3D_PARAM_SUPPORTS_CSD:
	case DRM_V3D_PARAM_SUPPORTS_CACHE_FLUSH: p->value = 1;			break;
	default:				p->value = 0;			break;
	}
	return 0;
}

/* The requests to a pretended V3D: BOs of malloc, jobs that end at once */
static int fake_ioctl( int fd, unsigned long request, void *arg )
{
	GCTX	*c;
	BOENT	*e;

	switch ( request ) {
	case DRM_IOCTL_V3D_CREATE_BO: {
		struct drm_v3d_create_bo	*cb = arg;
		uint32_t			size = ( cb->size + 0xfff ) & ~0xfffU;
		void				*mem = calloc(1, size);
		int				r = -1;

		if ( mem == NULL ) {
			errno = ENOMEM;
			return -1;
		}
		pthread_mutex_lock(&mu);
		c = ctx_of(fd);
		if ( c != NULL && ( e = bo_ent(c, c->next_bo + 1, 1) ) != NULL ) {
			cb->handle = ++c->next_bo;
			cb->offset = c->next_va;
			c->next_va += size;
			e->offset = cb->offset;
			e->size = size;
			e->addr = NULL;
			e->mem = mem;
			mem = NULL;
			r = 0;
		}
		pthread_mutex_unlock(&mu);
		free(mem);
		if ( r < 0 ) {
			errno = ENOMEM;
		}
		return r;
	}
	case DRM_IOCTL_GEM_CLOSE: {
		struct drm_gem_close	*gc = arg;
		void			*mem = NULL;

		pthread_mutex_lock(&mu);
		c = ctx_of(fd);
		if ( c != NULL && ( e = bo_ent(c, gc->handle, 0) ) != NULL ) {
			mem = e->mem;
			memset(e, 0, sizeof(*e));
		}
		pthread_mutex_unlock(&mu);
		free(mem);
		return 0;
	}
	case DRM_IOCTL_V3D_MMAP_BO: {
		struct drm_v3d_mmap_bo	*m = arg;

		m->offset = (uint64_t)m->handle << MMAP_SHIFT;
		return 0;
	}
	case DRM_IOCTL_V3D_GET_BO_OFFSET: {
		struct drm_v3d_get_bo_offset	*g = arg;
		int				r = -1;

		pthread_mutex_lock(&mu);
		c = ctx_of(fd);
		if ( c != NULL && ( e = bo_ent(c, g->handle, 0) ) != NULL && e->mem != NULL ) {
			g->offset = e->offset;
			r = 0;
		}
		pthread_mutex_unlock(&mu);
		if ( r < 0 ) {
			errno = ENOENT;
		}
		return r;
	}
	case DRM_IOCTL_V3D_WAIT_BO:
		return 0;
	case DRM_IOCTL_V3D_GET_PARAM:
		return fake_get_param(arg);
	case DRM_IOCTL_V3D_SUBMIT_CL:
		sync_set(((struct drm_v3d_submit_cl *)arg)->out_sync, 1, 0);
		return 0;
	case DRM_IOCTL_V3D_SUBMIT_TFU:
		sync_set(((struct drm_v3d_submit_tfu *)arg)->out_sync, 1, 0);
		return 0;
	case DRM_IOCTL_V3D_SUBMIT_CSD:
		sync_set(((struct drm_v3d_submit_csd *)arg)->out_sync, 1, 0);
		return 0;
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap	*g = arg;

		return drmGetCap(fd, g->capability, (uint64_t *)&g->value);
	}
	default:
		errno = ENOSYS;
		return -1;
	}
}

int drmIoctl( int fd, unsigned long request, void *arg )
{
	int	ctx = fd_ctx(fd);
	ER	er;

	if ( ctx <= 0 ) {
		errno = EBADF;
		return -1;
	}
#ifdef __x86_64__
	if ( fd_intel(fd) ) {
		return i915_ioctl(fd, ctx, request, arg);
	}
#endif
	if ( fd_fake(fd) ) {
		return fake_ioctl(fd, request, arg);
	}
	switch ( request ) {
	case DRM_IOCTL_V3D_CREATE_BO: {
		struct drm_v3d_create_bo	*c = arg;
		T_GPBO				b;

		er = gp_cre_bo(ctx, (SZ)c->size, 0, &b);
		if ( er < E_OK ) {
			return set_errno(er);
		}
		c->handle = b.handle;
		c->offset = b.offset;
		bo_note(fd, &b);
		return 0;
	}
	case DRM_IOCTL_GEM_CLOSE: {
		struct drm_gem_close	*c = arg;

		er = gp_del_bo(ctx, c->handle);
		if ( er < E_OK ) {
			return set_errno(er);
		}
		bo_forget(fd, c->handle);
		return 0;
	}
	case DRM_IOCTL_V3D_MMAP_BO: {
		struct drm_v3d_mmap_bo	*m = arg;

		m->offset = (uint64_t)m->handle << MMAP_SHIFT;
		return 0;
	}
	case DRM_IOCTL_V3D_GET_BO_OFFSET: {
		struct drm_v3d_get_bo_offset	*g = arg;
		T_GPBO				b;

		er = gp_ref_bo(ctx, g->handle, &b);
		if ( er < E_OK ) {
			return set_errno(er);
		}
		g->offset = b.offset;
		return 0;
	}
	case DRM_IOCTL_V3D_WAIT_BO: {
		struct drm_v3d_wait_bo	*w = arg;

		er = gp_wai_bo(ctx, w->handle, (TMO_U)( w->timeout_ns / 1000 ));
		return ( er < E_OK ) ? set_errno(er) : 0;
	}
	case DRM_IOCTL_V3D_GET_PARAM:
		return v3d_get_param(arg);
	case DRM_IOCTL_V3D_SUBMIT_CL:
		return v3d_submit_cl(ctx, arg);
	case DRM_IOCTL_V3D_SUBMIT_TFU:
		return v3d_submit_tfu(ctx, arg);
	case DRM_IOCTL_V3D_SUBMIT_CSD:
		return v3d_submit_csd(ctx, arg);
	case DRM_IOCTL_GEM_FLINK: {
		struct drm_gem_flink	*f = arg;
		INT			name = gp_shr_bo(ctx, f->handle);

		if ( name <= 0 ) {
			return set_errno(name);
		}
		f->name = (uint32_t)name;
		return 0;
	}
	case DRM_IOCTL_GEM_OPEN: {
		struct drm_gem_open	*o = arg;
		T_GPBO			b;

		er = gp_imp_bo(ctx, (INT)o->name, &b);
		if ( er < E_OK ) {
			return set_errno(er);
		}
		o->handle = b.handle;
		o->size = (uint64_t)b.size;
		bo_note(fd, &b);
		return 0;
	}
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap	*g = arg;

		return drmGetCap(fd, g->capability, (uint64_t *)&g->value);
	}
	default:
		errno = ENOSYS;		/* the performance counters, the CPU queue */
		return -1;
	}
}

int drmCommandWriteRead( int fd, unsigned long index, void *data, unsigned long size )
{
	return drmIoctl(fd, _IOC(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, size), data);
}

int drmGetCap( int fd, uint64_t capability, uint64_t *value )
{
	if ( fd_ctx(fd) <= 0 ) {
		errno = EBADF;
		return -1;
	}
	switch ( capability ) {
	case DRM_CAP_SYNCOBJ:	*value = 1;					return 0;
	case DRM_CAP_PRIME:	*value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;	return 0;
	default:		*value = 0;					return 0;
	}
}

int drmClose( int fd )
{
	return ( ts_gpu_fd_close(fd) == 0 ) ? 0 : -1;
}

int drmDestroyContext( int fd, uint32_t handle )
{
	(void)fd;
	(void)handle;
	return 0;
}

drmVersionPtr drmGetVersion( int fd )
{
	drmVersionPtr	v;
	const char	*name = "v3d";

	if ( fd_ctx(fd) <= 0 ) {
		errno = EBADF;
		return NULL;
	}
#ifdef __x86_64__
	if ( fd_intel(fd) ) {
		name = "i915";
	}
#endif
	v = calloc(1, sizeof(*v));
	if ( v == NULL ) {
		return NULL;
	}
	v->version_major = 1;
	v->name_len = (int)strlen(name);
	v->name = strdup(name);
	v->date = strdup("");
	v->desc = strdup("TessronOS GPU");
	v->desc_len = (int)strlen(v->desc != NULL ? v->desc : "");
	return v;
}

void drmFreeVersion( drmVersionPtr v )
{
	if ( v != NULL ) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

/* ---------------------------------------------------------------- PRIME */

int drmPrimeHandleToFD( int fd, uint32_t handle, uint32_t flags, int *prime_fd )
{
	int	ctx = fd_ctx(fd), i;
	INT	name;

	(void)flags;
	if ( ctx <= 0 ) {
		errno = EBADF;
		return -1;
	}
	name = gp_shr_bo(ctx, handle);
	if ( name <= 0 ) {
		return set_errno(name);
	}
	pthread_mutex_lock(&mu);
	for ( i = 0; i < PRIMEFD_MAX; i++ ) {
		if ( pfd[i] == 0 ) {
			pfd[i] = name;
			*prime_fd = PRIMEFD_BASE + i;
			pthread_mutex_unlock(&mu);
			return 0;
		}
	}
	pthread_mutex_unlock(&mu);
	errno = EMFILE;
	return -1;
}

int drmPrimeFDToHandle( int fd, int prime_fd, uint32_t *handle )
{
	int	ctx = fd_ctx(fd), i = prime_fd - PRIMEFD_BASE, name = 0;
	T_GPBO	b;
	ER	er;

	pthread_mutex_lock(&mu);
	if ( i >= 0 && i < PRIMEFD_MAX ) {
		name = pfd[i];
	}
	pthread_mutex_unlock(&mu);
	if ( ctx <= 0 || name <= 0 ) {
		errno = EBADF;
		return -1;
	}
	er = gp_imp_bo(ctx, name, &b);
	if ( er < E_OK ) {
		return set_errno(er);
	}
	bo_note(fd, &b);
	*handle = b.handle;
	return 0;
}

/* ---------------------------------------------------------------- devices */

int drmGetDevices2( uint32_t flags, drmDevicePtr devices[], int max_devices )
{
	(void)flags;
	(void)devices;
	(void)max_devices;
	return 0;
}

#define V3D_NODE	DRM_DIR_NAME "/" DRM_RENDER_MINOR_NAME "128"
#define V3D_FULLNAME	"/axi/v3d@1002000000"
#define V3D_COMPAT	"brcm,2712-v3d"

/* The device behind a descriptor of the GPU, all in one allocation that drmFreeDevice frees */
typedef struct {
	drmDevice		dev;
	char			*nodes[DRM_NODE_MAX];
	drmPlatformBusInfo	bus;
	drmPlatformDeviceInfo	info;
	char			*compat[2];
	char			node[sizeof(V3D_NODE)];
	char			name[sizeof(V3D_COMPAT)];
} V3DDEV;

/*
 * The V3D as a platform device with a render node only, as the loader
 * of Linux would find it: the bus, its name in the device tree and
 * what it is compatible with.
 */
int drmGetDevice2( int fd, uint32_t flags, drmDevicePtr *device )
{
	V3DDEV	*d;

	(void)flags;
	*device = NULL;
	if ( fd_ctx(fd) <= 0 ) {
		return -ENODEV;
	}
#ifdef __x86_64__
	if ( fd_intel(fd) ) {
		return i915_device(fd, device);
	}
#endif
	d = calloc(1, sizeof(*d));
	if ( d == NULL ) {
		return -ENOMEM;
	}
	strcpy(d->node, V3D_NODE);
	strcpy(d->name, V3D_COMPAT);
	strcpy(d->bus.fullname, V3D_FULLNAME);
	d->nodes[DRM_NODE_RENDER] = d->node;
	d->compat[0] = d->name;
	d->compat[1] = NULL;
	d->info.compatible = d->compat;
	d->dev.nodes = d->nodes;
	d->dev.available_nodes = 1 << DRM_NODE_RENDER;
	d->dev.bustype = DRM_BUS_PLATFORM;
	d->dev.businfo.platform = &d->bus;
	d->dev.deviceinfo.platform = &d->info;
	*device = &d->dev;
	return 0;
}

int drmGetDeviceFromDevId( dev_t dev_id, uint32_t flags, drmDevicePtr *device )
{
	(void)dev_id;
	(void)flags;
	*device = NULL;
	return -ENODEV;
}

void drmFreeDevice( drmDevicePtr *device )
{
	if ( device != NULL ) {
		free(*device);			/* the first member of V3DDEV */
		*device = NULL;
	}
}

void drmFreeDevices( drmDevicePtr devices[], int count )
{
	(void)devices;
	(void)count;
}

int drmDevicesEqual( drmDevicePtr a, drmDevicePtr b )
{
	if ( a == b ) {
		return 1;
	}
#ifdef __x86_64__
	if ( a != NULL && b != NULL && a->bustype == DRM_BUS_PCI && b->bustype == DRM_BUS_PCI ) {
		return a->businfo.pci->bus == b->businfo.pci->bus && a->businfo.pci->dev == b->businfo.pci->dev
		    && a->businfo.pci->func == b->businfo.pci->func;
	}
#endif
	if ( a == NULL || b == NULL || a->bustype != b->bustype || a->bustype != DRM_BUS_PLATFORM ) {
		return 0;
	}
	return strcmp(a->businfo.platform->fullname, b->businfo.platform->fullname) == 0;
}

int drmGetNodeTypeFromFd( int fd )
{
	return ( fd_ctx(fd) > 0 ) ? DRM_NODE_RENDER : -1;
}

static char *node_name( int fd )
{
	return ( fd_ctx(fd) > 0 ) ? strdup(DRM_DIR_NAME "/" DRM_RENDER_MINOR_NAME "128") : NULL;
}

char *drmGetDeviceNameFromFd2( int fd )
{
	return node_name(fd);
}

char *drmGetPrimaryDeviceNameFromFd( int fd )
{
	(void)fd;
	return NULL;
}

char *drmGetRenderDeviceNameFromFd( int fd )
{
	return node_name(fd);
}

#ifdef __x86_64__
#include "gl_drm_i915.c"
#endif
