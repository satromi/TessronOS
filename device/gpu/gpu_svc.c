/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpu_svc.c
 *	The GPU for processes: SVC class 10, gp_ (docs/tessronos/13-gpu.md
 *	13.5, include/ts/gpuapp.h).
 *
 *	A context belongs to the process that opened it and has a table of
 *	the BOs it holds. A process names a BO only by its number in that
 *	table; every entry holds one reference to the BO, and a mapping into
 *	the process one more (knl_gpu_bo_map). A job is handed the BOs
 *	themselves and holds its own references until it is over, so that
 *	deleting a number, closing the context or ending the process while
 *	the GPU still uses a BO only lets go of the process's references.
 *
 *	The tables are changed under a spin lock and only for as long as
 *	that takes: making a BO, mapping it and waiting are done outside it,
 *	with a reference taken while the entry was looked at, and the entry
 *	is looked at again afterwards in case the process changed it
 *	meanwhile from another of its tasks.
 *
 *	A shared BO has a name (gp_shr_bo) that holds a reference of its
 *	own, until the number it was made from is deleted or its context
 *	closed. Another process takes it in under a number of its own.
 *
 *	Where the backend gives address spaces of its own (the V3D), each
 *	context has one: a page table of the GPU's that holds the context's
 *	BOs and nothing else, at addresses of that table. The context's jobs
 *	run in it, so a list or a shader of the process reaches only the
 *	BOs the process holds (13-gpu.md 13.5). A backend that gives none
 *	(the tests' own) puts the BOs into its one space; a backend that has
 *	them but refuses one (its check at start failed, or no memory) gets
 *	no context.
 *
 *	A context has at most GP_JOB_AHEAD jobs queued: the next waits for
 *	the oldest of them, so that one process cannot fill the queue in
 *	front of the kernel's jobs (the window compositing).
 *
 *	What a process hands by pointer and what it is answered go in and
 *	out through the kernel's own memory, once made sure of, with
 *	knl_ua_get and knl_ua_put (PAN).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/svc.h>
#include <ts/proc.h>
#include <ts/gpuapp.h>
#include <tm/tmonitor.h>
#include "sysman/pfalloc.h"
#include "gpudev.h"

#define GP_BO_FIRST	64		/* entries of a new context */
#define GP_BO_MAX	16384		/* entries a context grows to */
#define GP_NAME_MAX	64		/* shared names in all */
#define GP_WAIT_SLICE	( 50 * 1000 )	/* us: a wait looks between slices whether its process ends */
#define GP_JOB_AHEAD	4		/* jobs of a context queued at once */

typedef struct {
	T_GPUBO	*bo;			/* NULL: a free entry */
	BOOL	mapped;			/* the process has it mapped */
	UW	va;			/* where it is in the context's space; 0 not there */
#ifdef __x86_64__
	UD	va64;			/* where the process placed it (gp_bnd_bo); 0 nowhere */
#endif
} GPENT;

typedef struct {
	ID	pid;			/* 0: a free context */
	INT	nent;
	GPENT	*ent;
	T_GPUAS	*as;			/* its address space; NULL: the backend's one */
	D	ahead[GP_JOB_AHEAD];	/* the fences of its last jobs */
	UINT	next;
} GPCTX;

typedef struct {
	T_GPUBO	*bo;			/* NULL: a free name */
	INT	ctx;			/* the context and number it was shared from */
	UINT	handle;
	UINT	gen;
} GPNAME;

LOCAL T_SPLOCK	gp_lock;
LOCAL GPCTX	gp_ctx[GP_CTX_MAX];
LOCAL GPNAME	gp_name[GP_NAME_MAX];
LOCAL UINT	gp_gen;

LOCAL BOOL user_ok( CONST void *p, SZ len, BOOL write )
{
	return knl_prc_user_ok(ts_get_pid(), p, len, write);
}

/* The context, when it is the calling process's; under the lock */
LOCAL GPCTX *ctx_of( INT ctx, ID pid )
{
	if ( ctx <= 0 || ctx > GP_CTX_MAX || pid <= 0 ) return NULL;
	if ( gp_ctx[ctx - 1].pid != pid ) return NULL;
	return &gp_ctx[ctx - 1];
}

/* The entry of a number; under the lock */
LOCAL GPENT *ent_of( GPCTX *c, UINT handle )
{
	if ( c == NULL || handle == 0 || handle > (UINT)c->nent ) return NULL;
	if ( c->ent[handle - 1].bo == NULL ) return NULL;
	return &c->ent[handle - 1];
}

/* The BO of a number of the caller's, with a reference the caller gives back */
LOCAL T_GPUBO *bo_take( INT ctx, UINT handle )
{
	GPENT	*e;
	T_GPUBO	*bo = NULL;
	UINT	imask;

	ISpinLock(&gp_lock, &imask);
	e = ent_of(ctx_of(ctx, ts_get_pid()), handle);
	if ( e != NULL ) {
		bo = e->bo;
		knl_gpu_bo_get(bo);
	}
	ISpinUnlock(&gp_lock, &imask);
	return bo;
}

/* ---------------------------------------------------------------- mappings */

typedef struct {
	T_GPUBO	*bo;
	UBINT	va;
} MAPARG;

LOCAL ER map_in( void *space, void *arg )
{
	MAPARG	*m = (MAPARG *)arg;

	return knl_gpu_bo_map(m->bo, space, &m->va);
}

LOCAL ER map_out( void *space, void *arg )
{
	return knl_gpu_bo_unmap(((MAPARG *)arg)->bo, space);
}

/*
 * The process's mapping of a BO taken out and its reference given back.
 * When the space has gone already the pages went with it, and only the
 * reference is left to give back.
 */
LOCAL void unmap_from( ID pid, T_GPUBO *bo )
{
	MAPARG	m;

	m.bo = bo;
	m.va = 0;
	if ( knl_prc_space_call(pid, map_out, &m) < E_OK ) {
		knl_gpu_bo_put(bo);
	}
}

/* ---------------------------------------------------------------- names */

/*
 * The names shared from a number (handle 0: from any of the context's)
 * taken off the table; their BOs, whose references the caller gives
 * back, into out. Under the lock.
 */
LOCAL INT names_drop( INT ctx, UINT handle, T_GPUBO **out )
{
	INT	i, n = 0;

	for ( i = 0; i < GP_NAME_MAX; i++ ) {
		if ( gp_name[i].bo != NULL && gp_name[i].ctx == ctx
		  && ( handle == 0 || gp_name[i].handle == handle ) ) {
			out[n++] = gp_name[i].bo;
			gp_name[i].bo = NULL;
		}
	}
	return n;
}

/* ---------------------------------------------------------------- the table of a context */

/* Kfree, for a pointer that may be NULL (Kfree takes only what Kmalloc gave) */
LOCAL void free_if( void *p )
{
	if ( p != NULL ) {
		Kfree(p);
	}
}

/*
 * The BO entered in the context under a new number, which is answered
 * (> 0); the reference the caller holds goes to the entry. The table
 * doubles when it is full, the new one made with the lock let go of.
 */
LOCAL INT ent_add( INT ctx, ID pid, T_GPUBO *bo, UW va )
{
	GPCTX	*c;
	GPENT	*grown = NULL, *old = NULL;
	INT	i, want = 0;
	UINT	imask;

	for ( ;; ) {
		ISpinLock(&gp_lock, &imask);
		c = ctx_of(ctx, pid);
		if ( c == NULL ) {
			ISpinUnlock(&gp_lock, &imask);
			free_if(grown);
			return E_ID;
		}
		if ( grown != NULL && want > c->nent ) {
			knl_memcpy(grown, c->ent, (SZ)c->nent * (SZ)sizeof(GPENT));
			old = c->ent;
			c->ent = grown;
			c->nent = want;
			grown = NULL;
		}
		for ( i = 0; i < c->nent; i++ ) {
			if ( c->ent[i].bo == NULL ) {
				c->ent[i].bo = bo;
				c->ent[i].mapped = FALSE;
				c->ent[i].va = va;
#ifdef __x86_64__
				c->ent[i].va64 = 0;
#endif
				ISpinUnlock(&gp_lock, &imask);
				free_if(grown);
				free_if(old);
				return i + 1;
			}
		}
		want = c->nent * 2;
		ISpinUnlock(&gp_lock, &imask);
		free_if(grown);
		free_if(old);
		old = NULL;
		if ( want > GP_BO_MAX ) return E_LIMIT;
		grown = (GPENT *)Kmalloc((SZ)want * (SZ)sizeof(GPENT));
		if ( grown == NULL ) return E_NOMEM;
		knl_memset(grown, 0, (SZ)want * (SZ)sizeof(GPENT));
	}
}

/* ---------------------------------------------------------------- the calls */

LOCAL INT svc_gp_opn_gpu( void )
{
	ID	pid = ts_get_pid();
	GPENT	*ent;
	T_GPUAS	*as;
	INT	i;
	UINT	imask;
	ER	er;

	if ( pid <= 0 ) return E_OBJ;
	as = knl_gpu_as_create(&er);
	if ( as == NULL && er != E_NOSPT && er != E_NOEXS ) {
		return er;		/* the backend has spaces of its own and gives none */
	}
	ent = (GPENT *)Kmalloc(GP_BO_FIRST * sizeof(GPENT));
	if ( ent == NULL ) {
		knl_gpu_as_put(as);
		return E_NOMEM;
	}
	knl_memset(ent, 0, GP_BO_FIRST * sizeof(GPENT));
	ISpinLock(&gp_lock, &imask);
	for ( i = 0; i < GP_CTX_MAX; i++ ) {
		if ( gp_ctx[i].pid == 0 ) {
			knl_memset(&gp_ctx[i], 0, sizeof(GPCTX));
			gp_ctx[i].pid = pid;
			gp_ctx[i].nent = GP_BO_FIRST;
			gp_ctx[i].ent = ent;
			gp_ctx[i].as = as;
			ISpinUnlock(&gp_lock, &imask);
			return i + 1;
		}
	}
	ISpinUnlock(&gp_lock, &imask);
	Kfree(ent);
	knl_gpu_as_put(as);
	return E_LIMIT;
}

/* The context of pid closed: every BO it held let go of, with its mappings and names */
LOCAL ER ctx_close( INT ctx, ID pid )
{
	GPCTX	*c;
	GPENT	*ent;
	T_GPUAS	*as;
	T_GPUBO	*names[GP_NAME_MAX];
	INT	i, n, nn;
	UINT	imask;

	ISpinLock(&gp_lock, &imask);
	c = ctx_of(ctx, pid);
	if ( c == NULL ) {
		ISpinUnlock(&gp_lock, &imask);
		return E_ID;
	}
	ent = c->ent;
	n = c->nent;
	as = c->as;
	c->ent = NULL;
	c->nent = 0;
	c->as = NULL;
	c->pid = 0;
	nn = names_drop(ctx, 0, names);
	ISpinUnlock(&gp_lock, &imask);

	for ( i = 0; i < n; i++ ) {
		if ( ent[i].bo == NULL ) continue;
		if ( ent[i].mapped ) unmap_from(pid, ent[i].bo);
		knl_gpu_as_unbind(as, ent[i].bo, ent[i].va);
#ifdef __x86_64__
		knl_gpu_as_unbind_at(as, ent[i].bo, ent[i].va64);
#endif
		knl_gpu_bo_put(ent[i].bo);
	}
	for ( i = 0; i < nn; i++ ) knl_gpu_bo_put(names[i]);
	free_if(ent);
	knl_gpu_as_put(as);		/* the table goes once the queued jobs on it are over */
	return E_OK;
}

LOCAL ER svc_gp_cls_gpu( INT ctx )
{
	return ctx_close(ctx, ts_get_pid());
}

LOCAL ER svc_gp_ref_gpu( T_GPUINFO *info )
{
	T_GPUINFO	k;
	ER		er;

	if ( !user_ok(info, sizeof(*info), TRUE) ) return E_MACV;
	er = knl_gpu_ref(&k);
	if ( er >= E_OK ) knl_ua_put(info, &k, sizeof(k));
	return er;
}

/* What the process is told of a BO: its address in the context's space, or in the one space */
LOCAL void bo_tell( T_GPUBO *bo, UINT handle, T_GPUAS *as, UW va, T_GPBO *out )
{
	T_GPUBOINFO	i;

	(void)knl_gpu_bo_ref(bo, &i);
	out->handle = handle;
	out->offset = ( as != NULL ) ? va : i.offset;
	out->size = i.size;
}

/* The context's space, with a reference the caller gives back; NULL when it has none */
LOCAL T_GPUAS *as_take( INT ctx, ID pid, BOOL *p_mine )
{
	GPCTX	*c;
	T_GPUAS	*as = NULL;
	UINT	imask;

	ISpinLock(&gp_lock, &imask);
	c = ctx_of(ctx, pid);
	if ( c != NULL && c->as != NULL ) {
		as = c->as;
		knl_gpu_as_get(as);
	}
	ISpinUnlock(&gp_lock, &imask);
	*p_mine = ( c != NULL );
	return as;
}

/*
 * A BO entered in the context under a new number, in its space when it
 * has one; the reference the caller holds goes to the entry
 */
LOCAL ER bo_enter( INT ctx, T_GPUBO *bo, BOOL in_space, T_GPBO *out )
{
	ID	pid = ts_get_pid();
	T_GPUAS	*as;
	T_GPBO	k;
	BOOL	mine;
	UW	va = 0;
	INT	h;
	ER	er;

	as = as_take(ctx, pid, &mine);
	if ( !mine ) {
		knl_gpu_bo_put(bo);
		return E_ID;
	}
	if ( as != NULL && in_space ) {
		er = knl_gpu_as_bind(as, bo, &va);
		if ( er < E_OK ) {
			knl_gpu_as_put(as);
			knl_gpu_bo_put(bo);
			return er;
		}
	}
	h = ent_add(ctx, pid, bo, va);
	if ( h < E_OK ) {
		knl_gpu_as_unbind(as, bo, va);
		knl_gpu_as_put(as);
		knl_gpu_bo_put(bo);
		return h;
	}
	bo_tell(bo, (UINT)h, as, va, &k);
	knl_gpu_as_put(as);
	*out = k;
	return E_OK;
}

LOCAL ER svc_gp_cre_bo( INT ctx, SZ size, UINT flags, T_GPBO *out )
{
	T_GPUBO	*bo;
	T_GPBO	k;
	T_GPUAS	*as;
	BOOL	mine;
	ER	er;

	if ( !user_ok(out, sizeof(*out), TRUE) ) return E_MACV;
	if ( ( flags & ~GPU_BO_NOGPU ) != 0 ) return E_PAR;
	as = as_take(ctx, ts_get_pid(), &mine);
	if ( !mine ) return E_ID;
	/* a context of its own space: the BO goes there, never into the kernel's */
	bo = knl_gpu_bo_create(size, ( as != NULL ) ? ( flags | GPU_BO_NOGPU ) : flags, &er);
	knl_gpu_as_put(as);
	if ( bo == NULL ) return er;
	er = bo_enter(ctx, bo, ( flags & GPU_BO_NOGPU ) == 0, &k);
	if ( er >= E_OK ) knl_ua_put(out, &k, sizeof(k));
	return er;
}

LOCAL ER svc_gp_del_bo( INT ctx, UINT handle )
{
	ID	pid = ts_get_pid();
	GPENT	*e;
	T_GPUBO	*bo, *names[GP_NAME_MAX];
	T_GPUAS	*as;
	BOOL	mapped;
	UW	va;
#ifdef __x86_64__
	UD	va64;
#endif
	INT	i, nn;
	UINT	imask;

	ISpinLock(&gp_lock, &imask);
	e = ent_of(ctx_of(ctx, pid), handle);
	if ( e == NULL ) {
		ISpinUnlock(&gp_lock, &imask);
		return E_ID;
	}
	bo = e->bo;
	mapped = e->mapped;
	va = e->va;
#ifdef __x86_64__
	va64 = e->va64;
	e->va64 = 0;
#endif
	as = gp_ctx[ctx - 1].as;
	if ( as != NULL ) knl_gpu_as_get(as);
	e->bo = NULL;
	e->mapped = FALSE;
	e->va = 0;
	nn = names_drop(ctx, handle, names);
	ISpinUnlock(&gp_lock, &imask);

	if ( mapped ) unmap_from(pid, bo);
	/* a job of the context's still queued keeps the pages; the address it reached is gone */
	knl_gpu_as_unbind(as, bo, va);
#ifdef __x86_64__
	knl_gpu_as_unbind_at(as, bo, va64);
#endif
	knl_gpu_as_put(as);
	knl_gpu_bo_put(bo);
	for ( i = 0; i < nn; i++ ) knl_gpu_bo_put(names[i]);
	return E_OK;
}

LOCAL ER svc_gp_ref_bo( INT ctx, UINT handle, T_GPBO *out )
{
	GPENT	*e;
	T_GPUBO	*bo = NULL;
	T_GPUAS	*as = NULL;
	T_GPBO	k;
	UW	va = 0;
	UINT	imask;

	if ( !user_ok(out, sizeof(*out), TRUE) ) return E_MACV;
	ISpinLock(&gp_lock, &imask);
	e = ent_of(ctx_of(ctx, ts_get_pid()), handle);
	if ( e != NULL ) {
		bo = e->bo;
		va = e->va;
		as = gp_ctx[ctx - 1].as;
		knl_gpu_bo_get(bo);
	}
	ISpinUnlock(&gp_lock, &imask);
	if ( bo == NULL ) return E_ID;
	bo_tell(bo, handle, as, va, &k);
	knl_gpu_bo_put(bo);
	knl_ua_put(out, &k, sizeof(k));
	return E_OK;
}

/*
 * Mapped once for the process, which keeps the mapping until the number
 * is deleted; asked again, the same address. Two tasks of the process
 * mapping the same number at once: the second finds the mapping there
 * (E_OBJ from the layer) and is answered the address as well.
 */
LOCAL ER svc_gp_map_bo( INT ctx, UINT handle, void **addr )
{
	ID		pid = ts_get_pid();
	GPENT		*e;
	T_GPUBO		*bo;
	T_GPUBOINFO	i;
	MAPARG		m;
	BOOL		mapped, keep = FALSE;
	UINT		imask;
	ER		er;

	if ( !user_ok(addr, sizeof(*addr), TRUE) ) return E_MACV;
	ISpinLock(&gp_lock, &imask);
	e = ent_of(ctx_of(ctx, pid), handle);
	if ( e == NULL ) {
		ISpinUnlock(&gp_lock, &imask);
		return E_ID;
	}
	bo = e->bo;
	mapped = e->mapped;
	knl_gpu_bo_get(bo);
	ISpinUnlock(&gp_lock, &imask);

	if ( !mapped ) {
		m.bo = bo;
		m.va = 0;
		er = knl_prc_space_call(pid, map_in, &m);
		if ( er == E_OBJ ) {
			er = E_OK;		/* another task of the process mapped it */
		} else if ( er >= E_OK ) {
			ISpinLock(&gp_lock, &imask);
			e = ent_of(ctx_of(ctx, pid), handle);
			if ( e != NULL && e->bo == bo && !e->mapped ) {
				e->mapped = TRUE;
				keep = TRUE;
			}
			ISpinUnlock(&gp_lock, &imask);
			if ( !keep ) {
				unmap_from(pid, bo);	/* deleted meanwhile */
				knl_gpu_bo_put(bo);
				return E_ID;
			}
		}
		if ( er < E_OK ) {
			knl_gpu_bo_put(bo);
			return er;
		}
	}
	(void)knl_gpu_bo_ref(bo, &i);
	knl_gpu_bo_put(bo);
	{
		void	*a = (void *)i.uva;

		knl_ua_put(addr, &a, sizeof(a));
	}
	return E_OK;
}

/*
 * A wait in slices, so that the process ending meanwhile is not held up
 * by it (the call is inside knl_prc_call_enter): E_DISWAI then.
 */
LOCAL ER wait_sliced( ER (*one)( void *arg, TMO_U t ), void *arg, TMO_U tmout )
{
	TMO_U	t;
	ER	er;

	for ( ;; ) {
		t = ( tmout == TMO_FEVR || tmout > GP_WAIT_SLICE ) ? GP_WAIT_SLICE : tmout;
		er = one(arg, t);
		if ( er != E_TMOUT ) return er;
		if ( tmout != TMO_FEVR ) {
			tmout -= t;
			if ( tmout <= 0 ) return E_TMOUT;
		}
		if ( knl_prc_ending() ) return E_DISWAI;
	}
}

LOCAL ER fence_one( void *arg, TMO_U t )
{
	return knl_gpu_wait(*(D *)arg, t);
}

LOCAL ER bo_one( void *arg, TMO_U t )
{
	return knl_gpu_bo_wait((T_GPUBO *)arg, t);
}

LOCAL D svc_gp_sub_job( INT ctx, CONST T_GPUJOB *job, CONST UINT *handles, INT n )
{
	ID		pid = ts_get_pid();
	T_GPUJOB	k;
	T_GPUBO		**bo;
	UINT		*h;
	GPCTX		*c;
	GPENT		*e;
	T_GPUAS		*as = NULL;
	INT		i, got = 0;
	UINT		imask, slot = 0;
	D		fence, oldest = 0;

	if ( n < 0 || n > GP_JOB_BO_MAX ) return E_PAR;
	if ( !user_ok(job, sizeof(*job), FALSE) ) return E_MACV;
	if ( n > 0 && !user_ok(handles, (SZ)n * (SZ)sizeof(UINT), FALSE) ) return E_MACV;
	knl_ua_get(&k, job, sizeof(k));
	bo = (T_GPUBO **)Kmalloc(( (SZ)n + 1 ) * ( (SZ)sizeof(T_GPUBO *) + (SZ)sizeof(UINT) ));
	if ( bo == NULL ) return E_NOMEM;
	h = (UINT *)( bo + n + 1 );
	if ( n > 0 ) knl_ua_get(h, handles, (SZ)n * (SZ)sizeof(UINT));

	ISpinLock(&gp_lock, &imask);
	c = ctx_of(ctx, pid);
	if ( c != NULL ) {
		slot = c->next;
		oldest = c->ahead[slot];
	}
	ISpinUnlock(&gp_lock, &imask);
	if ( c == NULL ) {
		Kfree(bo);
		return E_ID;
	}
	/* the oldest of its queued jobs over first (whatever became of it) */
	if ( oldest > 0 && wait_sliced(fence_one, &oldest, TMO_FEVR) == E_DISWAI ) {
		Kfree(bo);
		return E_DISWAI;
	}

	ISpinLock(&gp_lock, &imask);
	c = ctx_of(ctx, pid);
	if ( c != NULL ) {
		as = c->as;
		if ( as != NULL ) knl_gpu_as_get(as);
	}
	for ( i = 0; c != NULL && i < n; i++ ) {
		e = ent_of(c, h[i]);
		if ( e == NULL ) break;
		bo[i] = e->bo;
		knl_gpu_bo_get(bo[i]);
		got++;
	}
	ISpinUnlock(&gp_lock, &imask);

	if ( c == NULL || got < n ) {
		fence = E_ID;
	} else {
		fence = knl_gpu_submit_as(&k, bo, n, as);
	}
	if ( fence > 0 ) {
		ISpinLock(&gp_lock, &imask);
		c = ctx_of(ctx, pid);
		if ( c != NULL ) {
			c->ahead[slot] = fence;
			c->next = ( slot + 1 ) % GP_JOB_AHEAD;
		}
		ISpinUnlock(&gp_lock, &imask);
	}
	knl_gpu_as_put(as);
	for ( i = 0; i < got; i++ ) knl_gpu_bo_put(bo[i]);
	Kfree(bo);
	return fence;
}

LOCAL ER svc_gp_wai_fen( INT ctx, D fence, TMO_U tmout )
{
	BOOL	mine;
	UINT	imask;

	ISpinLock(&gp_lock, &imask);
	mine = ( ctx_of(ctx, ts_get_pid()) != NULL );
	ISpinUnlock(&gp_lock, &imask);
	if ( !mine ) return E_ID;
	return wait_sliced(fence_one, &fence, tmout);
}

LOCAL ER svc_gp_wai_bo( INT ctx, UINT handle, TMO_U tmout )
{
	T_GPUBO	*bo = bo_take(ctx, handle);
	ER	er;

	if ( bo == NULL ) return E_ID;
	er = wait_sliced(bo_one, bo, tmout);
	knl_gpu_bo_put(bo);
	return er;
}

LOCAL INT svc_gp_shr_bo( INT ctx, UINT handle )
{
	T_GPUBO	*bo = bo_take(ctx, handle);
	INT	i, name = E_LIMIT;
	UINT	imask;

	if ( bo == NULL ) return E_ID;
	ISpinLock(&gp_lock, &imask);
	for ( i = 0; i < GP_NAME_MAX; i++ ) {
		if ( gp_name[i].bo == bo && gp_name[i].ctx == ctx && gp_name[i].handle == handle ) {
			name = (INT)( ( gp_name[i].gen << 8 ) | (UINT)( i + 1 ) );
			break;
		}
	}
	for ( i = 0; name < 0 && i < GP_NAME_MAX; i++ ) {
		if ( gp_name[i].bo == NULL ) {
			gp_name[i].bo = bo;		/* the reference taken above */
			gp_name[i].ctx = ctx;
			gp_name[i].handle = handle;
			gp_name[i].gen = ( ++gp_gen ) & 0x3FFFFF;
			name = (INT)( ( gp_name[i].gen << 8 ) | (UINT)( i + 1 ) );
			bo = NULL;
		}
	}
	ISpinUnlock(&gp_lock, &imask);
	if ( bo != NULL ) knl_gpu_bo_put(bo);		/* shared before, or no room */
	return name;
}

LOCAL ER svc_gp_imp_bo( INT ctx, INT name, T_GPBO *out )
{
	T_GPUBO	*bo = NULL;
	T_GPBO	k;
	INT	i = ( name & 0xFF ) - 1;
	UINT	imask;
	ER	er;

	if ( !user_ok(out, sizeof(*out), TRUE) ) return E_MACV;
	if ( name <= 0 || i < 0 || i >= GP_NAME_MAX ) return E_PAR;
	ISpinLock(&gp_lock, &imask);
	if ( gp_name[i].bo != NULL && gp_name[i].gen == ( (UINT)name >> 8 ) ) {
		bo = gp_name[i].bo;
		knl_gpu_bo_get(bo);
	}
	ISpinUnlock(&gp_lock, &imask);
	if ( bo == NULL ) return E_NOEXS;
	er = bo_enter(ctx, bo, TRUE, &k);
	if ( er >= E_OK ) knl_ua_put(out, &k, sizeof(k));
	return er;
}

#ifdef __x86_64__
#define VA48_MASK	( ( 1ULL << 48 ) - 1 )
#define EVICT_TELL	8		/* places taken over told on the console */

LOCAL UINT	gp_evicted;

/*
 * Another BO of the context whose place overlaps 'va' for 'npages': taken
 * out of its place (its entry no longer placed), with a reference the
 * caller gives back, its number and where it was; NULL when there is none.
 * Under the lock.
 */
LOCAL T_GPUBO *overlapped( GPCTX *c, UD va, UD npages, UINT *p_handle, UD *p_va )
{
	UD	lo = va & VA48_MASK, hi = lo + ( npages << PAGE_SHIFT );
	INT	i;

	for ( i = 0; c != NULL && i < c->nent; i++ ) {
		GPENT	*x = &c->ent[i];
		UD	xlo, xhi;

		if ( x->bo == NULL || x->va64 == 0 ) continue;
		xlo = x->va64 & VA48_MASK;
		xhi = xlo + ( knl_gpu_bo_npages(x->bo) << PAGE_SHIFT );
		if ( xlo >= hi || xhi <= lo ) continue;
		*p_handle = (UINT)( i + 1 );
		*p_va = x->va64;
		x->va64 = 0;
		knl_gpu_bo_get(x->bo);
		return x->bo;
	}
	return NULL;
}

/*
 * A BO of the context placed at an address of the process's choosing in
 * the context's space (a user side driver that places its buffers
 * itself), and taken from where it was. Placing it where it is already
 * changes nothing. Two tasks of the process placing the same number at
 * once: the last one's place holds, and the other's is taken out again.
 *
 * A BO of the context whose place the new one overlaps is first taken
 * out of the space whole, and is no longer placed: a driver that keeps
 * the BOs it no longer uses for later (Mesa's iris) may give the address
 * of one of them to another BO before it gives the first a new one.
 * Taking the first from its old place afterwards would otherwise clear
 * the entries of the BO now there, which then reads the scratch page of
 * zeroes and writes nowhere, while the process takes it for placed.
 * Nothing of a BO that is not placed stays in the space either, so its
 * pages, once it is deleted, are not reached through stale entries.
 */
LOCAL ER svc_gp_bnd_bo( INT ctx, UINT handle, UD va )
{
	ID	pid = ts_get_pid();
	GPENT	*e;
	T_GPUBO	*bo;
	T_GPUAS	*as;
	UD	old;
	UINT	imask;
	ER	er;

	if ( va == 0 ) return E_PAR;
	ISpinLock(&gp_lock, &imask);
	e = ent_of(ctx_of(ctx, pid), handle);
	if ( e == NULL || gp_ctx[ctx - 1].as == NULL ) {
		ISpinUnlock(&gp_lock, &imask);
		return ( e == NULL ) ? E_ID : E_NOSPT;
	}
	if ( e->va64 == va ) {
		ISpinUnlock(&gp_lock, &imask);
		return E_OK;
	}
	bo = e->bo;
	as = gp_ctx[ctx - 1].as;
	old = e->va64;
	e->va64 = 0;
	knl_gpu_bo_get(bo);
	knl_gpu_as_get(as);
	ISpinUnlock(&gp_lock, &imask);

	knl_gpu_as_unbind_at(as, bo, old);
	for ( ;; ) {
		T_GPUBO	*xbo;
		UINT	xh = 0;
		UD	xva = 0;
		UINT	nth = 0;

		ISpinLock(&gp_lock, &imask);
		xbo = overlapped(ctx_of(ctx, pid), va, knl_gpu_bo_npages(bo), &xh, &xva);
		if ( xbo != NULL ) nth = ++gp_evicted;
		ISpinUnlock(&gp_lock, &imask);
		if ( xbo == NULL ) break;
		knl_gpu_as_unbind_at(as, xbo, xva);
		if ( nth <= EVICT_TELL ) {
			tm_printf((UB *)"gpu: number %u placed at %lx (%ld pages) over number %u at %lx (%ld pages), "
				  "which is taken out of the space (%u so far)\n", handle, (unsigned long)va,
				  (long)knl_gpu_bo_npages(bo), xh, (unsigned long)xva, (long)knl_gpu_bo_npages(xbo),
				  nth);
		}
		knl_gpu_bo_put(xbo);
	}
	er = knl_gpu_as_bind_at(as, bo, va);
	if ( er >= E_OK ) {
		ISpinLock(&gp_lock, &imask);
		e = ent_of(ctx_of(ctx, pid), handle);
		if ( e != NULL && e->bo == bo && e->va64 == 0 ) {
			e->va64 = va;
			va = 0;
		}
		ISpinUnlock(&gp_lock, &imask);
		if ( va != 0 ) {
			knl_gpu_as_unbind_at(as, bo, va);	/* deleted or placed again meanwhile */
		}
	}
	knl_gpu_as_put(as);
	knl_gpu_bo_put(bo);
	return er;
}
#endif

#define F(fn)	((FP)(fn))

EXPORT const FP knl_gp_svc_tbl[TSN_GP_MAX + 1] = {
	[TSN_NUMBER(TSN_GP_OPN_GPU)]	= F(svc_gp_opn_gpu),
	[TSN_NUMBER(TSN_GP_CLS_GPU)]	= F(svc_gp_cls_gpu),
	[TSN_NUMBER(TSN_GP_REF_GPU)]	= F(svc_gp_ref_gpu),
	[TSN_NUMBER(TSN_GP_CRE_BO)]	= F(svc_gp_cre_bo),
	[TSN_NUMBER(TSN_GP_DEL_BO)]	= F(svc_gp_del_bo),
	[TSN_NUMBER(TSN_GP_MAP_BO)]	= F(svc_gp_map_bo),
	[TSN_NUMBER(TSN_GP_SUB_JOB)]	= F(svc_gp_sub_job),
	[TSN_NUMBER(TSN_GP_WAI_FEN)]	= F(svc_gp_wai_fen),
	[TSN_NUMBER(TSN_GP_WAI_BO)]	= F(svc_gp_wai_bo),
	[TSN_NUMBER(TSN_GP_SHR_BO)]	= F(svc_gp_shr_bo),
	[TSN_NUMBER(TSN_GP_IMP_BO)]	= F(svc_gp_imp_bo),
	[TSN_NUMBER(TSN_GP_REF_BO)]	= F(svc_gp_ref_bo),
#ifdef __x86_64__
	[TSN_NUMBER(TSN_GP_BND_BO)]	= F(svc_gp_bnd_bo),
#endif
};

/* What a process held of the GPU, let go of as it ends (proc.c) */
EXPORT void knl_gpu_prc_end( ID pid )
{
	INT	i;

	if ( pid <= 0 ) return;
	for ( i = 0; i < GP_CTX_MAX; i++ ) {
		if ( gp_ctx[i].pid == pid ) {
			(void)ctx_close(i + 1, pid);
		}
	}
}

/* For a test: the contexts open, and the BOs they hold */
EXPORT INT knl_gpu_ctx_count( INT *p_bos )
{
	INT	i, j, n = 0, bos = 0;
	UINT	imask;

	ISpinLock(&gp_lock, &imask);
	for ( i = 0; i < GP_CTX_MAX; i++ ) {
		if ( gp_ctx[i].pid == 0 ) continue;
		n++;
		for ( j = 0; j < gp_ctx[i].nent; j++ ) {
			if ( gp_ctx[i].ent[j].bo != NULL ) bos++;
		}
	}
	ISpinUnlock(&gp_lock, &imask);
	if ( p_bos != NULL ) *p_bos = bos;
	return n;
}
