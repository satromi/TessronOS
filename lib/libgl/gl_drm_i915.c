/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gl_drm_i915.c
 *	The DRM shim's part for Intel's GPU (x64): the requests of i915 that
 *	Mesa's iris makes, given to the GPU SVCs of a process (gp_,
 *	docs/tessronos/13-gpu.md 13.11.9). It is read in at the end of
 *	gl_drm.c, whose tables (the descriptors, the BOs by number, the
 *	synchronisation objects, the sync_file descriptors) it shares.
 *
 *	A descriptor is a context of gp_, which is one address space of the
 *	GPU's: what i915 calls the VM. Its BOs are the context's numbers, as
 *	for the V3D. iris places every BO itself (softpin): the address in
 *	the list of an execbuffer is where the BO has to be, and a BO not
 *	there yet is placed with gp_bnd_bo before the batch goes.
 *
 *	i915's GEM contexts are kept here. Each entry of a context's map of
 *	engines (or each of the two rings of a context without one) runs in
 *	a hardware context of the space of its own (T_GPUBATCH hwc), handed
 *	out as it is first used and made anew when it is used again after
 *	its GEM context went. iris keeps the state of a hardware context
 *	from one batch to the next, as i915 keeps it.
 *
 *	The device is told as a PCI device 8086:<id> whose kernel driver is
 *	i915, so that the loader takes iris for it and iris reads what it is
 *	(GETPARAM, QUERY) from the answers here, which come from gp_ref_gpu.
 *
 *	ts_gl_drm_fake(2) pretends a Kaby Lake GT2 (8086:5917) with BOs in
 *	the process's own memory and batches that end at once, for the tests
 *	of a machine without the GPU: the loader, iris's screen and context
 *	are made as on the machine.
 *
 *	A wait on synchronisation objects follows DRM's: with WAIT_FOR_SUBMIT
 *	an object nothing has been submitted for yet is waited for until a
 *	batch names it (looked at again every millisecond), without it such
 *	an object is an error; WAIT_ALL waits for all, otherwise the first
 *	signalled is told.
 *
 *	REG_READ answers what i915 answers on this generation: the render
 *	engine's time stamp (RING_TIMESTAMP, 0x2358), 64 bits, asked for with
 *	or without the flag of reading it as two halves. It is read by the
 *	render engine itself: a batch of a space of the descriptor's own (so
 *	that nothing of iris's placing is touched) stores the register to
 *	memory, the upper half before and after the lower, and the halves
 *	that agree are taken.
 *
 *	iris calls the C library's ioctl, not drmIoctl: ioctl here (weak, so
 *	that a program's own goes first) gives the GPU's descriptors to
 *	drmIoctl through ts_gpu_fd_ioctl and says no to any other. A program
 *	with an ioctl of its own calls ts_gpu_fd_ioctl from it. The requests
 *	refused are kept and told by ts_gl_drm_tell.
 */

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#define I9_GEMCTX	32		/* GEM contexts of a descriptor */
#define I9_MAP		8		/* engines in a context's map */
#define VA48(v)		( (uint64_t)(v) & 0x0000ffffffffffffULL )
#define WAIT_SLICE_US	1000		/* an object not yet submitted, looked at again after */
#define TS_REG		0x2358		/* the render engine's time stamp; its upper half at +4 */
#define TS_VA		0x100000ULL	/* the batch reading it, in its own space */

typedef struct {
	uint32_t	id;		/* 0: a free slot */
	int		nmap;		/* entries of its map of engines; 0: none (rings) */
	uint8_t		cls[I9_MAP];	/* the class of each entry */
	int8_t		hwc[I9_MAP];	/* the hardware context of each entry, -1 none yet */
	int8_t		ring_hwc[2];	/* without a map: the render and the copy ring's */
} I9GCTX;

struct i915_fd {
	I9GCTX		g[I9_GEMCTX];
	uint32_t	next_id;
	uint8_t		hwc_used[GPU_HWC_MAX];
	uint8_t		hwc_fresh[GPU_HWC_MAX];
	T_GPUINFO	info;
	int		ts_ctx;		/* the space reading the time stamp; 0 none yet */
	uint32_t	*ts_mem;	/* its BO: the batch, then what it stored */
	uint32_t	ts_bo;
};

static pthread_mutex_t	ts_mu = PTHREAD_MUTEX_INITIALIZER;

/*
 * For ts_gl_drm_tell: the batches submitted on each engine (render,
 * copy), their bytes, and the waits that found a job ended in an error
 * (E_IO: the engine refused a command, or the job was reset)
 */
static uint32_t		nbatch[2], nbatch_bytes, nwait_io;

#define COUNT(v, n)	__atomic_add_fetch(&(v), (n), __ATOMIC_RELAXED)

/* The Kaby Lake GT2 pretended: what gp_ref_gpu answers on one */
static void fake_info( T_GPUINFO *i )
{
	memset(i, 0, sizeof(*i));
	i->kind = GPU_KIND_INTEL;
	i->ver = 90;
	i->rev = 7;
	i->ncores = 1;
	i->nqpu = 24;
	i->feature = GPU_FEAT_BATCH | GPU_FEAT_MMU;
	i->va_size = 1ULL << 48;
	i->hub_ident[0] = 0x5917;
	i->hub_ident[1] = 0x02000000;		/* FUSE2: slice 0 */
	i->hub_ident[2] = 0;			/* no execution unit fused off */
	i->hub_ident[3] = 0x0001 | ( 0x7 << 8 );	/* slices, subslices */
	i->core_ident[0] = 12000000;
	i->core_ident[1] = 4096;
	i->core_ident[2] = 1;
}

static int i915_open( GCTX *c )
{
	struct i915_fd	*s = calloc(1, sizeof(*s));
	int		k;

	if ( s == NULL ) {
		errno = ENOMEM;
		return -1;
	}
	if ( c->fake ) {
		fake_info(&s->info);
	} else if ( gp_ref_gpu(&s->info) < E_OK || s->info.kind != GPU_KIND_INTEL ) {
		free(s);
		errno = ENODEV;
		return -1;
	}
	/* GEM context 0, the default one, without a map */
	s->g[0].id = 0;
	s->g[0].ring_hwc[0] = s->g[0].ring_hwc[1] = -1;
	for ( k = 0; k < I9_MAP; k++ ) s->g[0].hwc[k] = -1;
	s->next_id = 1;
	c->i9 = s;
	return 0;
}

static void i915_close( GCTX *c )
{
	if ( c->i9 != NULL && c->i9->ts_ctx > 0 ) (void)gp_cls_gpu(c->i9->ts_ctx);
	free(c->i9);
	c->i9 = NULL;
	c->intel = 0;
}

static int fd_intel( int fd )
{
	GCTX	*c;
	int	r = 0;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL ) {
		r = c->intel;
	}
	pthread_mutex_unlock(&mu);
	return r;
}

/* ---------------------------------------------------------------- the device */

typedef struct {
	drmDevice		dev;
	char			*nodes[DRM_NODE_MAX];
	drmPciBusInfo		bus;
	drmPciDeviceInfo	info;
	char			node[sizeof(V3D_NODE)];
} I9DEV;

static int i915_device( int fd, drmDevicePtr *device )
{
	GCTX		*c;
	I9DEV		*d = calloc(1, sizeof(*d));
	uint16_t	id = 0x5917;
	uint8_t		rev = 0;

	if ( d == NULL ) {
		return -ENOMEM;
	}
	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL && c->i9 != NULL ) {
		id = (uint16_t)c->i9->info.hub_ident[0];
		rev = (uint8_t)c->i9->info.rev;
	}
	pthread_mutex_unlock(&mu);
	strcpy(d->node, V3D_NODE);
	d->nodes[DRM_NODE_RENDER] = d->node;
	d->bus.domain = 0;
	d->bus.bus = 0;
	d->bus.dev = 2;
	d->bus.func = 0;
	d->info.vendor_id = 0x8086;
	d->info.device_id = id;
	d->info.subvendor_id = 0x8086;
	d->info.subdevice_id = 0;
	d->info.revision_id = rev;
	d->dev.nodes = d->nodes;
	d->dev.available_nodes = 1 << DRM_NODE_RENDER;
	d->dev.bustype = DRM_BUS_PCI;
	d->dev.businfo.pci = &d->bus;
	d->dev.deviceinfo.pci = &d->info;
	*device = &d->dev;
	return 0;
}

/* ---------------------------------------------------------------- what the GPU is */

static int i9_getparam( struct i915_fd *s, struct drm_i915_getparam *p )
{
	const T_GPUINFO	*i = &s->info;
	int		v;

	switch ( p->param ) {
	case I915_PARAM_CHIPSET_ID:		v = (int)i->hub_ident[0];		break;
	case I915_PARAM_REVISION:		v = (int)i->rev;			break;
	case I915_PARAM_CS_TIMESTAMP_FREQUENCY:	v = (int)i->core_ident[0];		break;
	case I915_PARAM_SLICE_MASK:		v = (int)( i->hub_ident[3] & 0xff );	break;
	case I915_PARAM_SUBSLICE_MASK:		v = (int)( ( i->hub_ident[3] >> 8 ) & 0xff );	break;
	case I915_PARAM_EU_TOTAL:		v = (int)i->nqpu;			break;
	case I915_PARAM_MMAP_GTT_VERSION:	v = 4;					break;
	case I915_PARAM_HAS_CONTEXT_ISOLATION:	v = ( 1 << I915_ENGINE_CLASS_RENDER ) | ( 1 << I915_ENGINE_CLASS_COPY ); break;
	case I915_PARAM_HAS_EXEC_NO_RELOC:
	case I915_PARAM_HAS_EXEC_HANDLE_LUT:
	case I915_PARAM_HAS_EXEC_SOFTPIN:
	case I915_PARAM_HAS_EXEC_FENCE:
	case I915_PARAM_HAS_EXEC_BATCH_FIRST:
	case I915_PARAM_HAS_EXEC_FENCE_ARRAY:	v = 1;					break;
	case I915_PARAM_HAS_EXEC_ASYNC:
	case I915_PARAM_HAS_EXEC_CAPTURE:
	case I915_PARAM_HAS_EXEC_SUBMIT_FENCE:
	case I915_PARAM_HAS_EXEC_TIMELINE_FENCES:
	case I915_PARAM_HAS_USERPTR_PROBE:	v = 0;					break;
	case I915_PARAM_PXP_STATUS:
		errno = ENODEV;
		return -1;
	default:
		errno = EINVAL;
		return -1;
	}
	*p->value = v;
	return 0;
}

/* One item of a query: its length asked for (0), or filled */
static int32_t q_topology( struct i915_fd *s, void *data, int32_t len )
{
	struct drm_i915_query_topology_info	*t = data;
	const T_GPUINFO	*i = &s->info;
	uint32_t	slices = i->hub_ident[3] & 0xff, ss = ( i->hub_ident[3] >> 8 ) & 0xf;
	int		maxs = 1, maxss = 4, maxeu = 8, sl, k;
	int32_t		need = (int32_t)sizeof(*t) + 1 + maxs * 1 + maxs * maxss * 1;

	if ( len == 0 ) return need;
	if ( len < need ) return -EINVAL;
	memset(t, 0, (size_t)need);
	t->max_slices = (uint16_t)maxs;
	t->max_subslices = (uint16_t)maxss;
	t->max_eus_per_subslice = (uint16_t)maxeu;
	t->subslice_offset = 1;
	t->subslice_stride = 1;
	t->eu_offset = (uint16_t)( 1 + maxs );
	t->eu_stride = 1;
	t->data[0] = (uint8_t)( slices & 1 );
	for ( sl = 0; sl < maxs; sl++ ) {
		t->data[t->subslice_offset + sl] = (uint8_t)ss;
		for ( k = 0; k < maxss; k++ ) {
			if ( ss & ( 1u << k ) ) {
				t->data[t->eu_offset + sl * maxss + k] = (uint8_t)~( i->hub_ident[2] >> ( k * 8 ) );
			}
		}
	}
	return need;
}

static int32_t q_engines( void *data, int32_t len )
{
	struct drm_i915_query_engine_info	*e = data;
	int32_t	need = (int32_t)( sizeof(*e) + 2 * sizeof(e->engines[0]) );

	if ( len == 0 ) return need;
	if ( len < need ) return -EINVAL;
	memset(e, 0, (size_t)need);
	e->num_engines = 2;
	e->engines[0].engine.engine_class = I915_ENGINE_CLASS_RENDER;
	e->engines[1].engine.engine_class = I915_ENGINE_CLASS_COPY;
	e->engines[1].logical_instance = 0;
	return need;
}

static int32_t q_regions( void *data, int32_t len )
{
	struct drm_i915_query_memory_regions	*r = data;
	int32_t	need = (int32_t)( sizeof(*r) + sizeof(r->regions[0]) );
	long	pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
	uint64_t total = ( pages > 0 && psz > 0 ) ? (uint64_t)pages * (uint64_t)psz : 2ULL << 30;

	if ( len == 0 ) return need;
	if ( len < need ) return -EINVAL;
	memset(r, 0, (size_t)need);
	r->num_regions = 1;
	r->regions[0].region.memory_class = I915_MEMORY_CLASS_SYSTEM;
	r->regions[0].region.memory_instance = 0;
	r->regions[0].probed_size = total;
	r->regions[0].unallocated_size = total;
	return need;
}

static int i9_query( struct i915_fd *s, struct drm_i915_query *q )
{
	struct drm_i915_query_item	*it = (struct drm_i915_query_item *)(uintptr_t)q->items_ptr;
	uint32_t			k;

	if ( q->flags != 0 ) {
		errno = EINVAL;
		return -1;
	}
	for ( k = 0; k < q->num_items; k++ ) {
		void	*data = (void *)(uintptr_t)it[k].data_ptr;

		switch ( it[k].query_id ) {
		case DRM_I915_QUERY_TOPOLOGY_INFO:	it[k].length = q_topology(s, data, it[k].length);	break;
		case DRM_I915_QUERY_ENGINE_INFO:	it[k].length = q_engines(data, it[k].length);		break;
		case DRM_I915_QUERY_MEMORY_REGIONS:	it[k].length = q_regions(data, it[k].length);		break;
		default:				it[k].length = -EINVAL;				break;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------- GEM contexts */

/* The slot of a GEM context; with the lock held */
static I9GCTX *gem_ctx( struct i915_fd *s, uint32_t id )
{
	int	k;

	if ( id == 0 ) return &s->g[0];
	for ( k = 1; k < I9_GEMCTX; k++ ) {
		if ( s->g[k].id == id ) return &s->g[k];
	}
	return NULL;
}

/* A map of engines set from i915's i915_context_param_engines of 'size' bytes */
static int set_engines( I9GCTX *g, uint64_t value, uint32_t size )
{
	const struct i915_context_param_engines	*e = (const void *)(uintptr_t)value;
	int	n, k;

	if ( size < sizeof(e->extensions) ) {
		g->nmap = 0;
		return 0;
	}
	n = (int)( ( size - sizeof(e->extensions) ) / sizeof(e->engines[0]) );
	if ( n > I9_MAP || e->extensions != 0 ) {
		errno = EINVAL;
		return -1;
	}
	for ( k = 0; k < n; k++ ) {
		if ( e->engines[k].engine_class != I915_ENGINE_CLASS_RENDER
		  && e->engines[k].engine_class != I915_ENGINE_CLASS_COPY ) {
			errno = EINVAL;
			return -1;
		}
		g->cls[k] = (uint8_t)e->engines[k].engine_class;
		g->hwc[k] = -1;
	}
	g->nmap = n;
	return 0;
}

static int ctx_param_set( I9GCTX *g, const struct drm_i915_gem_context_param *p )
{
	switch ( p->param ) {
	case I915_CONTEXT_PARAM_ENGINES:
		return set_engines(g, p->value, p->size);
	case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
	case I915_CONTEXT_PARAM_LOW_LATENCY:
		if ( p->value != 0 ) {
			errno = ( p->param == I915_CONTEXT_PARAM_PROTECTED_CONTENT ) ? ENODEV : EINVAL;
			return -1;
		}
		return 0;
	case I915_CONTEXT_PARAM_VM:
	case I915_CONTEXT_PARAM_RECOVERABLE:
	case I915_CONTEXT_PARAM_PRIORITY:
	case I915_CONTEXT_PARAM_BANNABLE:
	case I915_CONTEXT_PARAM_PERSISTENCE:
	case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}

/* The hardware contexts of a GEM context given back */
static void hwc_free( struct i915_fd *s, I9GCTX *g )
{
	int	k;

	for ( k = 0; k < I9_MAP; k++ ) {
		if ( g->hwc[k] >= 0 ) s->hwc_used[g->hwc[k]] = 0;
		g->hwc[k] = -1;
	}
	for ( k = 0; k < 2; k++ ) {
		if ( g->ring_hwc[k] >= 0 ) s->hwc_used[g->ring_hwc[k]] = 0;
		g->ring_hwc[k] = -1;
	}
}

static int ctx_create( struct i915_fd *s, struct drm_i915_gem_context_create_ext *cc, int ext )
{
	I9GCTX	*g = NULL;
	uint64_t next;
	int	k;

	for ( k = 1; k < I9_GEMCTX; k++ ) {
		if ( s->g[k].id == 0 ) {
			g = &s->g[k];
			break;
		}
	}
	if ( g == NULL ) {
		errno = ENOSPC;
		return -1;
	}
	memset(g, 0, sizeof(*g));
	for ( k = 0; k < I9_MAP; k++ ) g->hwc[k] = -1;
	g->ring_hwc[0] = g->ring_hwc[1] = -1;
	if ( ext && ( cc->flags & I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS ) ) {
		for ( next = cc->extensions; next != 0; ) {
			const struct drm_i915_gem_context_create_ext_setparam *sp = (const void *)(uintptr_t)next;

			if ( sp->base.name != I915_CONTEXT_CREATE_EXT_SETPARAM || ctx_param_set(g, &sp->param) < 0 ) {
				if ( sp->base.name != I915_CONTEXT_CREATE_EXT_SETPARAM ) errno = EINVAL;
				return -1;
			}
			next = sp->base.next_extension;
		}
	}
	g->id = s->next_id++;
	if ( s->next_id == 0 ) s->next_id = 1;
	cc->ctx_id = g->id;
	return 0;
}

/*
 * The hardware context the batch of a GEM context runs in on 'ring'
 * (an index into its map, or i915's ring), and its engine; with the
 * lock held. A new one is marked to be made anew by the batch.
 */
static int hwc_of( struct i915_fd *s, I9GCTX *g, unsigned ring, int *p_eng, int *p_fresh )
{
	int8_t	*slot;
	int	eng, k;

	if ( g->nmap > 0 ) {
		if ( ring >= (unsigned)g->nmap ) return -1;
		eng = ( g->cls[ring] == I915_ENGINE_CLASS_COPY ) ? GPU_ENG_COPY : GPU_ENG_RENDER;
		slot = &g->hwc[ring];
	} else {
		if ( ring == I915_EXEC_DEFAULT || ring == I915_EXEC_RENDER ) eng = GPU_ENG_RENDER;
		else if ( ring == I915_EXEC_BLT ) eng = GPU_ENG_COPY;
		else return -1;
		slot = &g->ring_hwc[eng];
	}
	if ( *slot < 0 ) {
		for ( k = 0; k < GPU_HWC_MAX && s->hwc_used[k]; k++ ) ;
		if ( k == GPU_HWC_MAX ) return -1;
		s->hwc_used[k] = 1;
		s->hwc_fresh[k] = 1;
		*slot = (int8_t)k;
	}
	*p_eng = eng;
	*p_fresh = s->hwc_fresh[*slot];
	s->hwc_fresh[*slot] = 0;
	return *slot;
}

/* ---------------------------------------------------------------- BOs */

/* A BO made: real ones by gp_, pretended ones in the process's memory */
static int i9_create( int fd, int ctx, uint64_t size, uint32_t *p_handle )
{
	GCTX	*c;
	BOENT	*e;
	T_GPBO	b;
	ER	er;
	int	r = 0;

	size = ( size + 0xfff ) & ~0xfffULL;
	if ( size == 0 || size > GPU_BO_MAXSIZE ) {
		errno = EINVAL;
		return -1;
	}
	if ( fd_fake(fd) ) {
		void	*mem = NULL;

		if ( posix_memalign(&mem, 4096, (size_t)size) != 0 ) {
			errno = ENOMEM;
			return -1;
		}
		memset(mem, 0, (size_t)size);
		pthread_mutex_lock(&mu);
		c = ctx_of(fd);
		if ( c != NULL && ( e = bo_ent(c, c->next_bo + 1, 1) ) != NULL ) {
			*p_handle = ++c->next_bo;
			memset(e, 0, sizeof(*e));
			e->size = (uint32_t)size;
			e->mem = mem;
			mem = NULL;
		} else {
			r = -1;
		}
		pthread_mutex_unlock(&mu);
		free(mem);
		if ( r < 0 ) errno = ENOMEM;
		return r;
	}
	er = gp_cre_bo(ctx, (SZ)size, 0, &b);
	if ( er < E_OK ) {
		return set_errno(er);
	}
	bo_note(fd, &b);
	*p_handle = b.handle;
	return 0;
}

static int i9_close_bo( int fd, int ctx, uint32_t handle )
{
	GCTX	*c;
	BOENT	*e;
	void	*mem = NULL;
	ER	er;

	if ( fd_fake(fd) ) {
		pthread_mutex_lock(&mu);
		c = ctx_of(fd);
		if ( c != NULL && ( e = bo_ent(c, handle, 0) ) != NULL ) {
			mem = e->mem;
			memset(e, 0, sizeof(*e));
		}
		pthread_mutex_unlock(&mu);
		free(mem);
		return 0;
	}
	er = gp_del_bo(ctx, handle);
	if ( er < E_OK ) {
		return set_errno(er);
	}
	bo_forget(fd, handle);
	return 0;
}

/* What is kept of a BO's tiling, or its size: with the lock let go of */
static int bo_field( int fd, uint32_t handle, BOENT *out )
{
	GCTX	*c;
	BOENT	*e;
	int	r = -1;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL && ( e = bo_ent(c, handle, 0) ) != NULL && e->size != 0 ) {
		*out = *e;
		r = 0;
	}
	pthread_mutex_unlock(&mu);
	if ( r < 0 ) errno = ENOENT;
	return r;
}

static void bo_tiling( int fd, uint32_t handle, uint32_t tiling, uint32_t stride )
{
	GCTX	*c;
	BOENT	*e;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	if ( c != NULL && ( e = bo_ent(c, handle, 0) ) != NULL ) {
		e->tiling = tiling;
		e->stride = stride;
	}
	pthread_mutex_unlock(&mu);
}

/* ---------------------------------------------------------------- what a batch asks for, for the record */

/*
 * The first batches of a process told on the console, a line each: the
 * order of what the render engine is asked to do, as letters (D a draw,
 * C a draw that fast clears, P one that resolves partly, R one that
 * resolves fully, W a compute walker, | a pipe control that flushes the
 * render target, i one that invalidates the caches, s one that only
 * stalls, L a register loaded, > the batch going on in another BO), and
 * the registers loaded. Read from the process's mapping of the batch
 * after it went; a BO not mapped by the process cannot be read.
 */
#define TELL_BATCHES	24
#define TELL_EVENTS	96
#define TELL_REGS	12

static uint32_t		told_batches;

/* The process's mapping of the BO at GPU address 'va' of the list, and the bytes left in it from there */
static const uint32_t *batch_at( GCTX *c, const struct drm_i915_gem_exec_object2 *o, uint32_t n, uint64_t va,
				 uint64_t *left )
{
	uint32_t	k;

	for ( k = 0; k < n; k++ ) {
		BOENT	*e = bo_ent(c, o[k].handle, 0);

		if ( e == NULL || e->addr == NULL || e->size == 0 ) continue;
		if ( va >= e->va && va < e->va + e->size ) {
			*left = e->va + e->size - va;
			return (const uint32_t *)( (const uint8_t *)e->addr + ( va - e->va ) );
		}
	}
	return NULL;
}

/*
 * A draw's surfaces, as its pixel shader's binding table names them
 * (entry 0 the render target, 1 what is sampled): the format, the tiling,
 * the auxiliary surface's mode, the address, and the red of the clear
 * colour the state holds; with the lock held
 */
#define TELL_DRAWS	4

static void draw_surfaces( char *out, size_t n, GCTX *c, const struct drm_i915_gem_exec_object2 *o, uint32_t no,
			   uint32_t draw, char kind, uint64_t ssba, uint32_t bt )
{
	const uint32_t	*t, *s;
	uint64_t	left;
	int		l, k;

	l = snprintf(out, n, "i915:   draw %u (%c):", draw, kind);
	t = ( ssba != 0 ) ? batch_at(c, o, no, ssba + bt, &left) : NULL;
	if ( t == NULL || left < 8 ) {
		snprintf(out + l, n - (size_t)l, " no binding table (%llx + %x)\n", (unsigned long long)ssba, bt);
		return;
	}
	for ( k = 0; k < 2 && l > 0 && l < (int)n - 2; k++ ) {
		s = batch_at(c, o, no, ssba + ( t[k] & ~0x3fU ), &left);
		if ( s == NULL || left < 13 * 4 ) {
			l += snprintf(out + l, n - (size_t)l, " %s at %x not read;", k == 0 ? "target" : "source", t[k]);
			continue;
		}
		l += snprintf(out + l, n - (size_t)l, " %s format %03x tiling %u aux %u at %llx clear %08x;",
			      k == 0 ? "target" : "source", ( s[0] >> 18 ) & 0x1ff, ( s[0] >> 12 ) & 3, s[6] & 7,
			      (unsigned long long)VA48(( (uint64_t)s[9] << 32 ) | s[8]), s[12]);
	}
	if ( l > 0 && l < (int)n - 1 ) {
		out[l++] = '\n';
		out[l] = '\0';
	} else {
		out[n - 2] = '\n';
		out[n - 1] = '\0';
	}
}

static void batch_tell( int fd, const struct drm_i915_gem_exec_object2 *o, uint32_t n, uint64_t start, int eng,
			int hwc, uint32_t len )
{
	char		ev[TELL_EVENTS + 1], line[400];
	uint32_t	regs[TELL_REGS], nregs = 0, dw = 0, pieces = 1, draws = 0, k = 0, nb;
	uint64_t	va = start, left = 0;
	const uint32_t	*p;
	const char	*stop = "the end";
	int		nev = 0, l, ndl = 0, d;
	GCTX		*c;
	uint64_t	ssba = 0;
	uint32_t	bt_ps = 0;
	char		dl[TELL_DRAWS][240];

	nb = __atomic_add_fetch(&told_batches, 1, __ATOMIC_RELAXED);
	if ( nb > TELL_BATCHES ) return;
	ev[TELL_EVENTS] = 0;		/* the draws' kind, as the last 3DSTATE_PS set it */
	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	p = ( c != NULL ) ? batch_at(c, o, n, va, &left) : NULL;
	if ( p == NULL ) {
		pthread_mutex_unlock(&mu);
		snprintf(line, sizeof(line), "i915: batch %u (%s, context %d, %u bytes): not mapped by the process\n", nb,
			 eng == GPU_ENG_COPY ? "copy" : "render", hwc, len);
		ts_gl_log(line);
		return;
	}
#define EV(ch)	do { if ( nev < TELL_EVENTS ) ev[nev++] = (ch); } while ( 0 )
	for ( ;; ) {
		uint32_t	h, size;

		if ( left < 4 ) { stop = "the BO's end"; break; }
		if ( dw++ > 65536 ) { stop = "too long"; break; }
		h = p[0];
		switch ( h >> 29 ) {
		case 0: {					/* MI_ */
			uint32_t op = ( h >> 23 ) & 0x3f;

			if ( op == 0x0a ) goto done;		/* MI_BATCH_BUFFER_END */
			size = ( op < 0x10 ) ? 1 : ( h & 0xff ) + 2;
			if ( op == 0x22 ) {			/* MI_LOAD_REGISTER_IMM */
				for ( k = 1; k + 1 < size && k + 1 < left / 4; k += 2 ) {
					uint32_t r = p[k] & 0x7ffffc, j;

					EV('L');
					for ( j = 0; j < nregs && regs[j] != r; j++ ) ;
					if ( j == nregs && nregs < TELL_REGS ) regs[nregs++] = r;
				}
			}
			if ( op == 0x31 ) {			/* MI_BATCH_BUFFER_START: the batch goes on there */
				uint64_t to = VA48(( (uint64_t)p[2] << 32 ) | ( p[1] & ~3U ));

				EV('>');
				pieces++;
				p = batch_at(c, o, n, to, &left);
				if ( p == NULL ) { stop = "a jump out of the list"; goto done; }
				continue;
			}
			break;
		}
		case 3: {					/* the render engine's */
			uint32_t sub = ( h >> 27 ) & 3, op = ( h >> 24 ) & 7, so = ( h >> 16 ) & 0xff;

			size = ( sub == 1 && op == 1 && so == 4 ) || ( sub == 1 && op == 0 && so == 0x0b ) ? 1 : ( h & 0xff ) + 2;
			if ( sub == 3 && op == 0 && so == 0x20 && left >= 7 * 4 ) {	/* 3DSTATE_PS: how the next draws go */
				uint32_t d6 = p[6];

				ev[TELL_EVENTS] = ( d6 & 0x100 ) ? 'C' : ( ( d6 >> 6 ) & 3 ) == 1 ? 'P' : ( ( d6 >> 6 ) & 3 ) == 3 ? 'R' : 'D';
			}
			if ( sub == 0 && op == 1 && so == 1 && left >= 6 * 4 && ( p[4] & 1 ) ) {	/* STATE_BASE_ADDRESS */
				ssba = VA48(( (uint64_t)p[5] << 32 ) | ( p[4] & ~0xfffU ));
			}
			if ( sub == 3 && op == 0 && so == 0x2a && left >= 8 ) bt_ps = p[1] & 0xffe0;	/* the PS's binding table */
			if ( sub == 3 && op == 3 && so == 0 ) {	/* 3DPRIMITIVE */
				char kind = ev[TELL_EVENTS] != 0 ? ev[TELL_EVENTS] : 'D';

				EV(kind);
				draws++;
				if ( ndl < TELL_DRAWS ) draw_surfaces(dl[ndl++], sizeof(dl[0]), c, o, n, draws, kind, ssba, bt_ps);
			}
			if ( sub == 2 && op == 1 && so == 5 ) EV('W');	/* GPGPU_WALKER */
			if ( sub == 3 && op == 2 && so == 0 && left >= 8 ) {	/* PIPE_CONTROL */
				uint32_t f = p[1];

				EV(( f & ( 1U << 12 ) ) ? '|' : ( f & ( ( 1U << 10 ) | ( 1U << 11 ) | ( 1U << 2 ) | ( 1U << 3 ) | ( 1U << 4 ) ) ) ? 'i' : 's');
			}
			break;
		}
		default:
			stop = "a command not known";
			goto done;
		}
		if ( (uint64_t)size * 4 > left ) { stop = "the BO's end"; break; }
		p += size;
		left -= (uint64_t)size * 4;
		dw += size - 1;
	}
done:
#undef EV
	pthread_mutex_unlock(&mu);
	ev[nev] = '\0';
	l = snprintf(line, sizeof(line), "i915: batch %u (%s, context %d, %u bytes, %u pieces, to %s): %u draws %s; registers",
		     nb, eng == GPU_ENG_COPY ? "copy" : "render", hwc, len, pieces, stop, draws, ev);
	for ( k = 0; k < nregs && l > 0 && l < (int)sizeof(line) - 12; k++ ) l += snprintf(line + l, sizeof(line) - (size_t)l, " %04x", regs[k]);
	if ( l > 0 && l < (int)sizeof(line) - 2 ) {
		line[l++] = '\n';
		line[l] = '\0';
	}
	ts_gl_log(line);
	for ( d = 0; d < ndl; d++ ) ts_gl_log(dl[d]);
}

/* ---------------------------------------------------------------- execbuffer */

static int i9_execbuf( int fd, int ctx, struct drm_i915_gem_execbuffer2 *eb )
{
	struct drm_i915_gem_exec_object2	*o = (void *)(uintptr_t)eb->buffers_ptr;
	struct drm_i915_gem_exec_fence		*fe = NULL;
	struct drm_i915_gem_exec_object2	*bat;
	GCTX		*c;
	I9GCTX		*g;
	T_GPUJOB	j;
	uint32_t	*h, n = eb->buffer_count, k, nf = 0;
	uint64_t	start;
	int		eng = 0, fresh = 0, hwc = -1, fake = fd_fake(fd), out_fd = -1, r = 0;
	D		after = 0, fence;

	if ( n == 0 || n > GP_JOB_BO_MAX || ( eb->flags & I915_EXEC_USE_EXTENSIONS ) != 0 ) {
		errno = EINVAL;
		return -1;
	}
	h = malloc(n * sizeof(uint32_t));
	if ( h == NULL ) {
		errno = ENOMEM;
		return -1;
	}

	/* every BO where the list says it is */
	for ( k = 0; k < n; k++ ) {
		BOENT	*e;
		uint64_t want = VA48(o[k].offset), was = 0;
		int	known = 0;

		h[k] = o[k].handle;
		if ( ( o[k].flags & EXEC_OBJECT_PINNED ) == 0 || o[k].relocation_count != 0 ) {
			free(h);
			errno = EINVAL;			/* placed by the kernel: not done here */
			return -1;
		}
		pthread_mutex_lock(&mu);
		c = ctx_of(fd);
		if ( c != NULL && ( e = bo_ent(c, o[k].handle, 0) ) != NULL && e->size != 0 ) {
			known = 1;
			was = e->va;
		}
		pthread_mutex_unlock(&mu);
		if ( !known ) {
			free(h);
			errno = ENOENT;
			return -1;
		}
		if ( was != want ) {
			if ( !fake ) {
				ER	er = gp_bnd_bo(ctx, o[k].handle, want);

				if ( er < E_OK ) {
					free(h);
					return set_errno(er);
				}
			}
			pthread_mutex_lock(&mu);
			c = ctx_of(fd);
			if ( c != NULL && ( e = bo_ent(c, o[k].handle, 0) ) != NULL ) e->va = want;
			pthread_mutex_unlock(&mu);
		}
	}

	bat = ( eb->flags & I915_EXEC_BATCH_FIRST ) ? &o[0] : &o[n - 1];
	start = VA48(bat->offset) + eb->batch_start_offset;

	/* the hardware context of the GEM context's ring */
	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	g = ( c != NULL && c->i9 != NULL ) ? gem_ctx(c->i9, (uint32_t)( eb->rsvd1 & I915_EXEC_CONTEXT_ID_MASK )) : NULL;
	if ( g != NULL ) hwc = hwc_of(c->i9, g, (unsigned)( eb->flags & I915_EXEC_RING_MASK ), &eng, &fresh);
	pthread_mutex_unlock(&mu);
	if ( g == NULL || hwc < 0 ) {
		free(h);
		errno = ( g == NULL ) ? ENOENT : EINVAL;
		return -1;
	}

	/* what it waits for: the latest fence of those named (the GPU runs its jobs in order) */
	if ( eb->flags & I915_EXEC_FENCE_ARRAY ) {
		fe = (struct drm_i915_gem_exec_fence *)(uintptr_t)eb->cliprects_ptr;
		nf = eb->num_cliprects;
		for ( k = 0; k < nf; k++ ) {
			int	sig;
			D	f;

			if ( ( fe[k].flags & I915_EXEC_FENCE_WAIT ) && sync_get(fe[k].handle, &sig, &f) == 0
			  && !sig && f > after ) {
				after = f;
			}
		}
	}
	if ( eb->flags & I915_EXEC_FENCE_IN ) {
		SYNCFD	*sf;

		pthread_mutex_lock(&mu);
		sf = sfd_of((int)( eb->rsvd2 & 0xffffffff ));
		if ( sf != NULL && !sf->signaled && sf->fence > after ) after = sf->fence;
		pthread_mutex_unlock(&mu);
	}

	if ( fake ) {
		fence = 0;
	} else {
		memset(&j, 0, sizeof(j));
		j.kind = GPU_JOB_BATCH;
		j.after = after;
		j.u.batch.start_lo = (UW)start;
		j.u.batch.start_hi = (UW)( start >> 32 );
		j.u.batch.len = eb->batch_len;
		j.u.batch.engine = (UW)eng;
		j.u.batch.hwc = (UW)hwc;
		j.u.batch.flags = fresh ? GPU_BATCH_FRESH : 0;
		fence = gp_sub_job(ctx, &j, (CONST UINT *)h, (INT)n);
		if ( fence <= 0 ) {
			free(h);
			return set_errno((ER)fence);
		}
	}
	free(h);
	COUNT(nbatch[eng == GPU_ENG_COPY ? 1 : 0], 1);
	COUNT(nbatch_bytes, eb->batch_len);
	batch_tell(fd, o, n, start, eng, hwc, eb->batch_len);

	/* what it signals: the job's fence (a pretended one is over at once) */
	for ( k = 0; fe != NULL && k < nf; k++ ) {
		if ( fe[k].flags & I915_EXEC_FENCE_SIGNAL ) sync_set(fe[k].handle, fake, fence);
	}
	if ( eb->flags & I915_EXEC_FENCE_OUT ) {
		pthread_mutex_lock(&mu);
		out_fd = sfd_new(fake, fence, fake ? 0 : ctx);
		pthread_mutex_unlock(&mu);
		if ( out_fd < 0 ) {
			r = -1;
		} else {
			eb->rsvd2 = ( eb->rsvd2 & 0xffffffffULL ) | ( (uint64_t)(uint32_t)out_fd << 32 );
		}
	}
	return r;
}

/* ---------------------------------------------------------------- waiting on synchronisation objects */

/*
 * The objects waited for until 'abs_ns' on the monotonic clock. The GPU
 * runs one job at a time, in the order they came: of the fences
 * submitted, the earliest is the first to be over.
 */
static int i9_sync_wait( int fd, int ctx, const uint32_t *handles, uint32_t n, int64_t abs_ns, uint32_t flags,
			 uint32_t *first )
{
	int	all = ( flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL ) != 0;
	int	submit = ( flags & ( DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE ) ) != 0;
	int	fake = fd_fake(fd);
	uint32_t i;

	if ( n == 0 ) {
		errno = EINVAL;
		return -1;
	}
	for ( ;; ) {
		uint32_t	pending = 0, unsubmitted = 0;
		int64_t		first_done = -1, left;
		D		earliest = 0;
		TMO_U		tmo;
		ER		er;

		for ( i = 0; i < n; i++ ) {
			int	sig;
			D	f;

			if ( sync_get(handles[i], &sig, &f) < 0 ) {
				errno = ENOENT;
				return -1;
			}
			if ( !sig && f > 0 ) {
				er = fake ? E_OK : gp_wai_fen(ctx, f, 0);
				if ( er == E_IO ) COUNT(nwait_io, 1);
				if ( er == E_OK || er == E_IO ) {
					sync_set(handles[i], 1, f);
					sig = 1;
				} else if ( er != E_TMOUT ) {
					return set_errno(er);
				}
			}
			if ( sig ) {
				if ( first_done < 0 ) first_done = i;
			} else if ( f <= 0 ) {
				if ( !submit ) {
					errno = EINVAL;		/* nothing submitted to signal it */
					return -1;
				}
				unsubmitted++;
			} else {
				pending++;
				if ( earliest == 0 || f < earliest ) earliest = f;
			}
		}
		if ( !all && first_done >= 0 ) {
			if ( first != NULL ) *first = (uint32_t)first_done;
			return 0;
		}
		if ( all && pending == 0 && unsubmitted == 0 ) {
			if ( first != NULL ) *first = 0;
			return 0;
		}

		left = abs_ns - mono_ns();
		if ( left <= 0 ) {
			errno = ETIME;
			return -1;
		}
		tmo = ( abs_ns == INT64_MAX ) ? TMO_FEVR : (TMO_U)( left / 1000 ) + 1;
		if ( unsubmitted != 0 && ( tmo == TMO_FEVR || tmo > WAIT_SLICE_US ) ) tmo = WAIT_SLICE_US;
		if ( pending != 0 ) {
			er = gp_wai_fen(ctx, earliest, tmo);
			if ( er < E_OK && er != E_TMOUT && er != E_IO ) return set_errno(er);
		} else {
			usleep((useconds_t)tmo);
		}
	}
}

/* ---------------------------------------------------------------- reading the time stamp */

/* The space of the descriptor's own and its batch, made the first time; under ts_mu */
static int ts_setup( struct i915_fd *s )
{
	T_GPBO		b;
	void		*a;
	uint32_t	*m, k = 0, i;
	ER		er;
	INT		ctx;
	/* where each store goes, in bytes into the BO: upper, lower, upper, lower */
	static const uint32_t	at[4] = { 0x800, 0x808, 0x810, 0x818 };
	static const uint32_t	reg[4] = { TS_REG + 4, TS_REG, TS_REG + 4, TS_REG };

	if ( s->ts_ctx > 0 ) return 0;
	ctx = gp_opn_gpu();
	if ( ctx <= 0 ) return set_errno((ER)ctx);
	er = gp_cre_bo(ctx, 4096, GPU_BO_NOGPU, &b);
	if ( er >= E_OK ) er = gp_bnd_bo(ctx, b.handle, TS_VA);
	if ( er >= E_OK ) er = gp_map_bo(ctx, b.handle, &a);
	if ( er < E_OK ) {
		(void)gp_cls_gpu(ctx);
		return set_errno(er);
	}
	m = a;
	for ( i = 0; i < 4; i++ ) {
		m[k++] = ( 0x24U << 23 ) | 2;		/* MI_STORE_REGISTER_MEM, 64 bit address */
		m[k++] = reg[i];
		m[k++] = (uint32_t)( TS_VA + at[i] );
		m[k++] = (uint32_t)( ( TS_VA + at[i] ) >> 32 );
	}
	m[k++] = 0x0aU << 23;				/* MI_BATCH_BUFFER_END */
	m[k++] = 0;
	s->ts_ctx = ctx;
	s->ts_mem = m;
	s->ts_bo = b.handle;
	return 0;
}

/* The render engine's time stamp, 64 bits */
static int ts_read( int fd, struct i915_fd *s, uint64_t *val )
{
	T_GPUJOB	j;
	UINT		h;
	D		fence;
	ER		er;
	int		r = 0;
	uint32_t	hi0, lo0, hi1, lo1;

	if ( fd_fake(fd) ) {
		/* nothing to read: the monotonic clock in its ticks (12 MHz) */
		*val = (uint64_t)mono_ns() * 12 / 1000;
		return 0;
	}
	pthread_mutex_lock(&ts_mu);
	if ( ts_setup(s) < 0 ) {
		pthread_mutex_unlock(&ts_mu);
		return -1;
	}
	h = s->ts_bo;
	memset(&j, 0, sizeof(j));
	j.kind = GPU_JOB_BATCH;
	j.u.batch.start_lo = (UW)TS_VA;
	j.u.batch.start_hi = (UW)( TS_VA >> 32 );
	j.u.batch.len = 18 * 4;
	j.u.batch.engine = GPU_ENG_RENDER;
	j.u.batch.hwc = 0;
	fence = gp_sub_job(s->ts_ctx, &j, &h, 1);
	er = ( fence > 0 ) ? gp_wai_fen(s->ts_ctx, fence, 1000000) : (ER)fence;
	if ( er < E_OK ) {
		r = set_errno(er);
	} else {
		volatile uint32_t	*o = s->ts_mem;

		hi0 = o[0x800 / 4];
		lo0 = o[0x808 / 4];
		hi1 = o[0x810 / 4];
		lo1 = o[0x818 / 4];
		/* the lower half wrapped between the reads: the second pair agrees */
		*val = ( hi0 == hi1 ) ? ( (uint64_t)hi0 << 32 ) | lo0 : ( (uint64_t)hi1 << 32 ) | lo1;
	}
	pthread_mutex_unlock(&ts_mu);
	return r;
}

/* What i915 lets a process read: the time stamp alone, as one register of 8 bytes */
static int i9_reg_read( int fd, struct i915_fd *s, struct drm_i915_reg_read *rr )
{
	uint64_t	flags = rr->offset & 7;

	if ( ( rr->offset & ~7ULL ) != TS_REG || ( flags != 0 && flags != I915_REG_READ_8B_WA ) ) {
		errno = EINVAL;
		return -1;
	}
	return ts_read(fd, s, &rr->val);
}

/* ---------------------------------------------------------------- the requests */

static int i915_request( int fd, int ctx, unsigned long request, void *arg )
{
	GCTX		*c;
	struct i915_fd	*s;
	int		fake = fd_fake(fd), r;
	ER		er;

	pthread_mutex_lock(&mu);
	c = ctx_of(fd);
	s = ( c != NULL ) ? c->i9 : NULL;
	pthread_mutex_unlock(&mu);
	if ( s == NULL ) {
		errno = EBADF;
		return -1;
	}

	switch ( request ) {
	case DRM_IOCTL_I915_GETPARAM:
		return i9_getparam(s, arg);
	case DRM_IOCTL_I915_QUERY:
		return i9_query(s, arg);

	case DRM_IOCTL_I915_GEM_CREATE: {
		struct drm_i915_gem_create	*cr = arg;

		return i9_create(fd, ctx, cr->size, &cr->handle);
	}
	case DRM_IOCTL_I915_GEM_CREATE_EXT: {
		struct drm_i915_gem_create_ext	*cr = arg;

		return i9_create(fd, ctx, cr->size, &cr->handle);
	}
	case DRM_IOCTL_GEM_CLOSE:
		return i9_close_bo(fd, ctx, ((struct drm_gem_close *)arg)->handle);
	case DRM_IOCTL_I915_GEM_MMAP_OFFSET: {
		struct drm_i915_gem_mmap_offset	*m = arg;

		m->offset = (uint64_t)m->handle << MMAP_SHIFT;
		return 0;
	}
	case DRM_IOCTL_I915_GEM_MMAP: {
		struct drm_i915_gem_mmap	*m = arg;
		void				*a = ts_gpu_fd_mmap(fd, (size_t)m->size, (off_t)( (uint64_t)m->handle << MMAP_SHIFT ));

		if ( a == MAP_FAILED ) return -1;
		m->addr_ptr = (uint64_t)(uintptr_t)a + m->offset;
		return 0;
	}
	case DRM_IOCTL_I915_GEM_SET_DOMAIN:
	case DRM_IOCTL_I915_GEM_SET_CACHING:
		return 0;		/* the GPU and the processor share the last level cache */
	case DRM_IOCTL_I915_GEM_MADVISE:
		((struct drm_i915_gem_madvise *)arg)->retained = 1;
		return 0;
	case DRM_IOCTL_I915_GEM_SET_TILING: {
		struct drm_i915_gem_set_tiling	*t = arg;
		BOENT				e;

		if ( bo_field(fd, t->handle, &e) < 0 ) return -1;
		bo_tiling(fd, t->handle, t->tiling_mode, t->stride);
		t->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		return 0;
	}
	case DRM_IOCTL_I915_GEM_GET_TILING: {
		struct drm_i915_gem_get_tiling	*t = arg;
		BOENT				e;

		if ( bo_field(fd, t->handle, &e) < 0 ) return -1;
		t->tiling_mode = e.tiling;
		t->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		t->phys_swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		return 0;
	}
	case DRM_IOCTL_I915_GEM_GET_APERTURE: {
		struct drm_i915_gem_get_aperture	*a = arg;

		a->aper_size = 256ULL << 20;
		a->aper_available_size = a->aper_size;
		return 0;
	}
	case DRM_IOCTL_I915_GEM_BUSY: {
		struct drm_i915_gem_busy	*b = arg;

		b->busy = 0;
		if ( fake ) return 0;
		er = gp_wai_bo(ctx, b->handle, 0);
		if ( er == E_TMOUT ) {
			b->busy = 1;
			return 0;
		}
		return ( er < E_OK ) ? set_errno(er) : 0;
	}
	case DRM_IOCTL_I915_GEM_WAIT: {
		struct drm_i915_gem_wait	*w = arg;

		if ( fake ) return 0;
		er = gp_wai_bo(ctx, w->bo_handle, ( w->timeout_ns < 0 ) ? TMO_FEVR : (TMO_U)( w->timeout_ns / 1000 ));
		if ( er == E_TMOUT ) {
			errno = ETIME;
			return -1;
		}
		return ( er < E_OK ) ? set_errno(er) : 0;
	}
	case DRM_IOCTL_I915_GEM_USERPTR:
		errno = ENODEV;			/* the process's own memory is not given to the GPU */
		return -1;

	case DRM_IOCTL_I915_GEM_CONTEXT_CREATE:
	case DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT: {
		struct drm_i915_gem_context_create_ext	cc;

		memset(&cc, 0, sizeof(cc));
		if ( request == DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT ) {
			cc = *(struct drm_i915_gem_context_create_ext *)arg;
		}
		pthread_mutex_lock(&mu);
		r = ctx_create(s, &cc, request == DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT);
		pthread_mutex_unlock(&mu);
		if ( r < 0 ) return -1;
		if ( request == DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT ) {
			((struct drm_i915_gem_context_create_ext *)arg)->ctx_id = cc.ctx_id;
		} else {
			((struct drm_i915_gem_context_create *)arg)->ctx_id = cc.ctx_id;
		}
		return 0;
	}
	case DRM_IOCTL_I915_GEM_CONTEXT_DESTROY: {
		struct drm_i915_gem_context_destroy	*d = arg;
		I9GCTX					*g;

		pthread_mutex_lock(&mu);
		g = ( d->ctx_id != 0 ) ? gem_ctx(s, d->ctx_id) : NULL;
		if ( g != NULL ) {
			hwc_free(s, g);
			g->id = 0;
		}
		pthread_mutex_unlock(&mu);
		if ( g == NULL ) {
			errno = ENOENT;
			return -1;
		}
		return 0;
	}
	case DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM: {
		struct drm_i915_gem_context_param	*p = arg;
		I9GCTX					*g;

		pthread_mutex_lock(&mu);
		g = gem_ctx(s, p->ctx_id);
		r = ( g != NULL ) ? ctx_param_set(g, p) : -1;
		pthread_mutex_unlock(&mu);
		if ( g == NULL ) errno = ENOENT;
		return r;
	}
	case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM: {
		struct drm_i915_gem_context_param	*p = arg;

		switch ( p->param ) {
		case I915_CONTEXT_PARAM_GTT_SIZE:	p->value = s->info.va_size;	return 0;
		case I915_CONTEXT_PARAM_VM:		p->value = 1;			return 0;
		case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
		case I915_CONTEXT_PARAM_RECOVERABLE:
		case I915_CONTEXT_PARAM_PRIORITY:	p->value = 0;			return 0;
		default:
			errno = EINVAL;
			return -1;
		}
	}
	case DRM_IOCTL_I915_REG_READ:
		return i9_reg_read(fd, s, arg);
	case DRM_IOCTL_I915_GET_RESET_STATS: {
		struct drm_i915_reset_stats	*rs = arg;

		rs->reset_count = 0;
		rs->batch_active = 0;
		rs->batch_pending = 0;
		return 0;
	}
	case DRM_IOCTL_I915_GEM_EXECBUFFER2:
	case DRM_IOCTL_I915_GEM_EXECBUFFER2_WR:
		return i9_execbuf(fd, ctx, arg);

	/* what DRM has besides i915's: the synchronisation objects, PRIME, names */
	case DRM_IOCTL_SYNCOBJ_CREATE: {
		struct drm_syncobj_create	*a = arg;

		return drmSyncobjCreate(fd, a->flags, &a->handle);
	}
	case DRM_IOCTL_SYNCOBJ_DESTROY:
		return drmSyncobjDestroy(fd, ((struct drm_syncobj_destroy *)arg)->handle);
	case DRM_IOCTL_SYNCOBJ_WAIT: {
		struct drm_syncobj_wait	*w = arg;

		return i9_sync_wait(fd, ctx, (const uint32_t *)(uintptr_t)w->handles, w->count_handles,
				    w->timeout_nsec, w->flags, &w->first_signaled);
	}
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: {
		struct drm_syncobj_timeline_wait	*w = arg;

		/* no timelines: every point is the object's one fence */
		return i9_sync_wait(fd, ctx, (const uint32_t *)(uintptr_t)w->handles, w->count_handles,
				    w->timeout_nsec, w->flags, &w->first_signaled);
	}
	case DRM_IOCTL_SYNCOBJ_RESET: {
		struct drm_syncobj_array	*a = arg;

		return drmSyncobjReset(fd, (const uint32_t *)(uintptr_t)a->handles, a->count_handles);
	}
	case DRM_IOCTL_SYNCOBJ_SIGNAL: {
		struct drm_syncobj_array	*a = arg;

		return drmSyncobjSignal(fd, (const uint32_t *)(uintptr_t)a->handles, a->count_handles);
	}
	case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: {
		struct drm_syncobj_handle	*a = arg;

		if ( ( a->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE ) == 0 ) {
			errno = EINVAL;
			return -1;
		}
		return drmSyncobjExportSyncFile(fd, a->handle, &a->fd);
	}
	case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: {
		struct drm_syncobj_handle	*a = arg;

		if ( ( a->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE ) == 0 ) {
			errno = EINVAL;
			return -1;
		}
		return drmSyncobjImportSyncFile(fd, a->handle, a->fd);
	}
	case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
		struct drm_prime_handle	*a = arg;

		return drmPrimeHandleToFD(fd, a->handle, a->flags, &a->fd);
	}
	case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
		struct drm_prime_handle	*a = arg;

		return drmPrimeFDToHandle(fd, a->fd, &a->handle);
	}
	case DRM_IOCTL_GEM_FLINK: {
		struct drm_gem_flink	*f = arg;
		INT			name = gp_shr_bo(ctx, f->handle);

		if ( name <= 0 ) return set_errno(name);
		f->name = (uint32_t)name;
		return 0;
	}
	case DRM_IOCTL_GEM_OPEN: {
		struct drm_gem_open	*op = arg;
		T_GPBO			b;

		er = gp_imp_bo(ctx, (INT)op->name, &b);
		if ( er < E_OK ) return set_errno(er);
		op->handle = b.handle;
		op->size = (uint64_t)b.size;
		bo_note(fd, &b);
		return 0;
	}
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap	*g = arg;

		return drmGetCap(fd, g->capability, (uint64_t *)&g->value);
	}
	default:
		errno = EINVAL;		/* registers, performance, protected content */
		return -1;
	}
}

/*
 * The requests that were refused, kept for ts_gl_drm_tell: each kind
 * (the request, and for GETPARAM the parameter, for QUERY the item) once,
 * with its errno, in the order they came. An item of a query answered
 * with a negative length is kept too, with that length as its errno:
 * the driver takes it as a query i915 does not know.
 */
#define I9_FAILS	12

typedef struct {
	uint32_t	req;		/* the request's number (its low byte) */
	int32_t		what;		/* GETPARAM's parameter, QUERY's item, -1 */
	int32_t		err;
	uint32_t	times;
} I9FAIL;

static pthread_mutex_t	fail_mu = PTHREAD_MUTEX_INITIALIZER;
static I9FAIL		fails[I9_FAILS];
static uint32_t		nfails, nreq, nrefused;

static void fail_note( unsigned long request, int what, int err )
{
	uint32_t	k, req = (uint32_t)( request & 0xff );

	pthread_mutex_lock(&fail_mu);
	nrefused++;
	for ( k = 0; k < nfails; k++ ) {
		if ( fails[k].req == req && fails[k].what == what && fails[k].err == err ) {
			fails[k].times++;
			break;
		}
	}
	if ( k == nfails && nfails < I9_FAILS ) {
		fails[k].req = req;
		fails[k].what = what;
		fails[k].err = err;
		fails[k].times = 1;
		nfails++;
	}
	pthread_mutex_unlock(&fail_mu);
}

/* The name of a request of i915, by the low byte of its number */
static const char *req_name( uint32_t req )
{
	switch ( req ) {
	case DRM_COMMAND_BASE + DRM_I915_GETPARAM:		return "GETPARAM";
	case DRM_COMMAND_BASE + DRM_I915_QUERY:			return "QUERY";
	case DRM_COMMAND_BASE + DRM_I915_GEM_CREATE:		return "GEM_CREATE";
	case DRM_COMMAND_BASE + DRM_I915_GEM_CREATE_EXT:	return "GEM_CREATE_EXT";
	case DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_CREATE:	return "GEM_CONTEXT_CREATE";
	case DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_SETPARAM:	return "GEM_CONTEXT_SETPARAM";
	case DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_GETPARAM:	return "GEM_CONTEXT_GETPARAM";
	case DRM_COMMAND_BASE + DRM_I915_GEM_EXECBUFFER2:	return "GEM_EXECBUFFER2";
	case DRM_COMMAND_BASE + DRM_I915_GEM_USERPTR:		return "GEM_USERPTR";
	case DRM_COMMAND_BASE + DRM_I915_GEM_MMAP_GTT:		return "GEM_MMAP_OFFSET";
	case DRM_COMMAND_BASE + DRM_I915_GEM_WAIT:		return "GEM_WAIT";
	case DRM_COMMAND_BASE + DRM_I915_REG_READ:		return "REG_READ";
	case DRM_COMMAND_BASE + DRM_I915_GEM_SET_TILING:	return "GEM_SET_TILING";
	case DRM_COMMAND_BASE + DRM_I915_GEM_GET_TILING:	return "GEM_GET_TILING";
	case 0x09:						return "GEM_CLOSE";
	case 0x0c:						return "GET_CAP";
	default:						return NULL;
	}
}

/*
 * The requests, those that fail kept for ts_gl_drm_tell and, for the
 * pretended GPU, told on the console at once (a test: which request a
 * driver stopped at is what is looked for)
 */
static int i915_ioctl( int fd, int ctx, unsigned long request, void *arg )
{
	int	r = i915_request(fd, ctx, request, arg), e = errno;

	pthread_mutex_lock(&fail_mu);
	nreq++;
	pthread_mutex_unlock(&fail_mu);
	if ( r == 0 && request == DRM_IOCTL_I915_QUERY ) {
		const struct drm_i915_query	*q = arg;
		const struct drm_i915_query_item *it = (const void *)(uintptr_t)q->items_ptr;
		uint32_t			k;

		for ( k = 0; k < q->num_items; k++ ) {
			if ( it[k].length < 0 ) fail_note(request, (int)it[k].query_id, -it[k].length);
		}
	}
	if ( r < 0 ) {
		int	what = -1;

		if ( request == DRM_IOCTL_I915_GETPARAM ) what = ((struct drm_i915_getparam *)arg)->param;
		else if ( request == DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM || request == DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM ) {
			what = (int)((struct drm_i915_gem_context_param *)arg)->param;
		}
		fail_note(request, what, e);
	}
	if ( r < 0 && fd_fake(fd) ) {
		char	line[96];

		snprintf(line, sizeof(line), "i915 (pretended): request %02lx of %lu bytes: errno %d\n",
			 request & 0xff, ( request >> 16 ) & 0x3fff, e);
		ts_gl_log(line);
	}
	errno = e;
	return r;
}

/*
 * What the driver asked of i915 so far and what was refused, on the
 * console in one or two lines (the EGL platform, when the driver did not
 * make its screen or its frame was wrong): which request the driver
 * stopped at, with its errno, is what tells why.
 */
void ts_gl_drm_tell( const char *why )
{
	char		line[640];
	int		n;
	uint32_t	k;

	pthread_mutex_lock(&fail_mu);
	n = snprintf(line, sizeof(line), "egl: i915 (%s): %u requests, batches %u render %u copy (%u bytes), "
		     "%u waits found a job ended in an error; %u refused%s", why, nreq, nbatch[0], nbatch[1],
		     nbatch_bytes, nwait_io, nrefused, nfails > 0 ? ":" : "");
	for ( k = 0; k < nfails && n > 0 && n < (int)sizeof(line) - 1; k++ ) {
		const char	*nm = req_name(fails[k].req);
		char		rq[16];

		if ( nm == NULL ) {
			snprintf(rq, sizeof(rq), "%02x", fails[k].req);
			nm = rq;
		}
		if ( fails[k].what >= 0 ) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %d errno %d (%u)", nm, fails[k].what,
				      fails[k].err, fails[k].times);
		} else {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s errno %d (%u)", nm, fails[k].err,
				      fails[k].times);
		}
	}
	pthread_mutex_unlock(&fail_mu);
	if ( n >= (int)sizeof(line) - 1 ) n = (int)sizeof(line) - 2;
	line[n] = '\n';
	line[n + 1] = '\0';
	ts_gl_log(line);
}

/*
 * A request on a descriptor of the GPU, for any ioctl of the program
 * (the C library's below, or a program's own that comes before it):
 * -1 with ENOTTY for a descriptor that is not the GPU's
 */
int ts_gpu_fd_ioctl( int fd, unsigned long request, void *arg )
{
	if ( fd >= GPUFD_BASE && fd < GPUFD_BASE + GPUFD_MAX && fd_ctx(fd) > 0 ) {
		return drmIoctl(fd, request, arg);
	}
	errno = ENOTTY;
	return -1;
}

/*
 * The C library's ioctl for the GPU's descriptors. A program with an
 * ioctl of its own (the browser's) calls ts_gpu_fd_ioctl from it:
 * otherwise every request of iris (intel_ioctl) is refused with ENOTTY.
 */
__attribute__((weak)) int ioctl( int fd, unsigned long request, ... )
{
	va_list	ap;
	void	*arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	return ts_gpu_fd_ioctl(fd, request, arg);
}

/*
 * fstat of a descriptor of the GPU (lib/libcxxrt's _fstat asks here
 * first): a character device, the render node of DRM, the same for every
 * descriptor of the one GPU. iris keeps one buffer manager for the
 * descriptors of one device, as on Linux.
 */
int ts_gpu_fd_fstat( int fd, struct stat *st )
{
	if ( fd < GPUFD_BASE || fd >= GPUFD_BASE + GPUFD_MAX || fd_ctx(fd) <= 0 ) {
		errno = EBADF;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0666;
	st->st_rdev = ( 226 << 8 ) | 128;
	return 0;
}

/*
 * Intel's measuring of batches (INTEL_MEASURE, a tool of the developers
 * of the drivers) makes a named pipe when it is asked for; there are
 * none here
 */
__attribute__((weak)) int mkfifoat( int dirfd, const char *path, mode_t mode )
{
	(void)dirfd;
	(void)path;
	(void)mode;
	errno = ENOSYS;
	return -1;
}

/* ---------------------------------------------------------------- a check for the tests */

static void *sig_later( void *arg )
{
	int		*a = arg;
	struct timespec	t = { 0, 30 * 1000 * 1000 };
	uint32_t	h = (uint32_t)a[1];

	(void)nanosleep(&t, NULL);
	(void)drmSyncobjSignal(a[0], &h, 1);
	return NULL;
}

/*
 * The waits on synchronisation objects and the reading of the time
 * stamp, against the pretended Kaby Lake (ts_gl_drm_fake(2) on): 0, or
 * the step that went wrong (tests/uprog/glprog.c "i915sync")
 */
int ts_gl_i915_check( void )
{
	struct drm_syncobj_create	cr;
	struct drm_syncobj_wait		w;
	struct drm_i915_reg_read	rr;
	uint32_t	h[2] = { 0, 0 };
	uint64_t	t1;
	int64_t		t0;
	pthread_t	th;
	int		fd, a[2], step = 0, k;

	fd = ts_gl_drm_open();
	if ( fd < 0 || !fd_intel(fd) ) return 1;
	for ( k = 0; k < 2; k++ ) {
		memset(&cr, 0, sizeof(cr));
		if ( drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &cr) != 0 ) {
			step = 2;
			goto out;
		}
		h[k] = cr.handle;
	}

	/* nothing submitted: an error without WAIT_FOR_SUBMIT, a time out with it */
	memset(&w, 0, sizeof(w));
	w.handles = (uint64_t)(uintptr_t)h;
	w.count_handles = 1;
	w.timeout_nsec = 0;
	if ( drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) == 0 || errno != EINVAL ) { step = 3; goto out; }
	w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	if ( drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) == 0 || errno != ETIME ) { step = 4; goto out; }

	/* signalled later by another thread: waited for until then */
	a[0] = fd;
	a[1] = (int)h[0];
	if ( pthread_create(&th, NULL, sig_later, a) != 0 ) { step = 5; goto out; }
	t0 = mono_ns();
	w.timeout_nsec = t0 + 2000000000LL;
	k = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w);
	t0 = mono_ns() - t0;
	(void)pthread_join(th, NULL);
	if ( k != 0 ) { step = 6; goto out; }
	if ( t0 < 20000000LL || t0 > 1500000000LL ) { step = 7; goto out; }

	/* any of two: the signalled one told; all of two: not yet */
	w.count_handles = 2;
	w.timeout_nsec = mono_ns() + 5000000LL;
	w.first_signaled = 9;
	if ( drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) != 0 || w.first_signaled != 0 ) { step = 8; goto out; }
	w.flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	w.timeout_nsec = mono_ns() + 5000000LL;
	if ( drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) == 0 || errno != ETIME ) { step = 9; goto out; }

	/* the time stamp: read as two halves or as one, going up; nothing else */
	memset(&rr, 0, sizeof(rr));
	rr.offset = TS_REG | I915_REG_READ_8B_WA;
	if ( drmIoctl(fd, DRM_IOCTL_I915_REG_READ, &rr) != 0 || rr.val == 0 ) { step = 10; goto out; }
	t1 = rr.val;
	rr.offset = TS_REG;
	if ( drmIoctl(fd, DRM_IOCTL_I915_REG_READ, &rr) != 0 || rr.val < t1 ) { step = 11; goto out; }
	rr.offset = TS_REG + 4;
	if ( drmIoctl(fd, DRM_IOCTL_I915_REG_READ, &rr) == 0 || errno != EINVAL ) { step = 12; goto out; }
	rr.offset = 0x2000;
	if ( drmIoctl(fd, DRM_IOCTL_I915_REG_READ, &rr) == 0 || errno != EINVAL ) { step = 13; goto out; }
out:
	(void)drmSyncobjDestroy(fd, h[0]);
	(void)drmSyncobjDestroy(fd, h[1]);
	(void)ts_gpu_fd_close(fd);
	return step;
}
