/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_thread.c
 *	Threads, their thread local storage and their specific data.
 *
 *	A thread is a task of the process. The kernel gives the task a small
 *	stack of its own (TASK_STK); the task sets TPIDR_EL0 to the thread's
 *	control block and moves to the stack the thread was made with, a
 *	stretch of memory asked for with ts_map_mem that has a page below it
 *	that is never made, so running off the end faults. When the thread's
 *	function returns, or it calls pthread_exit, the task goes back to
 *	the kernel's stack; from there the thread's own stack can be given
 *	back while nothing runs on it.
 *
 *	TPIDR_EL0 is kept by the kernel for each task (design 9.3), so the
 *	compiler's thread local variables (the local exec model of a
 *	program linked statically) work as they do anywhere.
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <ts/umem.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sched.h>
#include "pth_int.h"

#define PAGE		4096UL
#define TASK_STK	( 16 * 1024 )		/* the kernel's stack for the task itself */

static inline size_t up( size_t v, size_t a )
{
	return ( v + a - 1 ) & ~( a - 1 );
}

/* ---------------------------------------------------------------- thread local storage */

/* The TLS image, from the link script */
extern char __tls_start[], __tls_filesz[], __tls_memsz[], __tls_align[];

size_t pth_tls_align( void )
{
	size_t	a = (size_t)__tls_align;

	return ( a < 16 ) ? 16 : a;
}

/* Bytes from the control block to the end of the thread's copy */
size_t pth_tls_size( void )
{
	return up(16, pth_tls_align()) + (size_t)__tls_memsz;
}

void *pth_tls_make( void *at, PTH *owner )
{
	char	*tp = (char *)at;
	char	*data = tp + up(16, pth_tls_align());

	((PTH **)tp)[0] = owner;
	((PTH **)tp)[1] = NULL;
	memcpy(data, __tls_start, (size_t)__tls_filesz);
	memset(data + (size_t)__tls_filesz, 0, (size_t)__tls_memsz - (size_t)__tls_filesz);
	return tp;
}

static inline void set_tp( void *tp )
{
	__asm__ volatile ( "msr tpidr_el0, %0" :: "r"(tp) : "memory" );
}

/* ---------------------------------------------------------------- the main thread */

static PTH	main_pth;

PTH *pth_main( void )
{
	return &main_pth;
}

void pth_init_main( void *stk_lo, void *stk_hi, void *tls )
{
	main_pth.tid = (int)tk_get_tid();
	main_pth.stk_lo = stk_lo;
	main_pth.stk_hi = stk_hi;
	main_pth.tls = tls;
	strcpy(main_pth.name, "main");
	pth_glock();				/* the one kernel mutex, made now */
	pth_gunlock();
}

/* ---------------------------------------------------------------- specific data */

static struct {
	int		used;
	unsigned	seq;
	void		(*dtor)( void * );
} keys[PTHREAD_KEYS_MAX];

int pthread_key_create( pthread_key_t *key, void (*dtor)( void * ) )
{
	int	i;

	pth_glock();
	for ( i = 0; i < PTHREAD_KEYS_MAX; i++ ) {
		if ( !keys[i].used ) {
			keys[i].used = 1;
			keys[i].seq++;
			keys[i].dtor = dtor;
			pth_gunlock();
			*key = (pthread_key_t)i;
			return 0;
		}
	}
	pth_gunlock();
	return EAGAIN;
}

int pthread_key_delete( pthread_key_t key )
{
	if ( key >= PTHREAD_KEYS_MAX ) return EINVAL;
	pth_glock();
	keys[key].used = 0;
	keys[key].dtor = NULL;
	pth_gunlock();
	return 0;
}

void *pthread_getspecific( pthread_key_t key )
{
	PTH	*me = pth_me();

	if ( key >= PTHREAD_KEYS_MAX || me == NULL || me->tsdseq[key] != keys[key].seq ) {
		return NULL;
	}
	return me->tsd[key];
}

int pthread_setspecific( pthread_key_t key, const void *val )
{
	PTH	*me = pth_me();

	if ( key >= PTHREAD_KEYS_MAX || !keys[key].used || me == NULL ) return EINVAL;
	me->tsd[key] = (void *)val;
	me->tsdseq[key] = keys[key].seq;
	return 0;
}

/* What a thread leaves behind runs as it ends: specific data, then thread_local objects */
static void run_dtors( PTH *me )
{
	int	round, i, again = 1;

	for ( round = 0; again && round < PTHREAD_DESTRUCTOR_ITERATIONS; round++ ) {
		again = 0;
		for ( i = 0; i < PTHREAD_KEYS_MAX; i++ ) {
			void	*v = me->tsd[i];
			void	(*d)( void * ) = keys[i].dtor;

			if ( v != NULL && keys[i].used && me->tsdseq[i] == keys[i].seq && d != NULL ) {
				me->tsd[i] = NULL;
				d(v);
				again = 1;
			}
		}
	}
	while ( me->dtors != NULL ) {
		PTH_DTOR *d = me->dtors;

		me->dtors = d->next;
		d->fn(d->obj);
		free(d);
	}
}

/*
 * thread_local objects with a destructor register it here (the C++
 * runtime's own copy keeps one list for the whole program, which would
 * run another thread's destructors in the wrong thread, or never).
 */
static int	main_dtors_armed;

static void main_dtors( void )
{
	run_dtors(&main_pth);
}

int __cxa_thread_atexit( void (*fn)( void * ), void *obj, void *dso )
{
	PTH		*me = pth_me();
	PTH_DTOR	*d;

	(void)dso;
	if ( me == NULL ) return -1;
	d = (PTH_DTOR *)malloc(sizeof(*d));
	if ( d == NULL ) return -1;
	d->fn = fn;
	d->obj = obj;
	d->next = me->dtors;
	me->dtors = d;
	if ( me == &main_pth && !main_dtors_armed ) {
		main_dtors_armed = 1;
		atexit(main_dtors);
	}
	return 0;
}

/* ---------------------------------------------------------------- threads */

/* Run fn(arg) on the stack whose top is sp, and come back (pth_stack.S) */
extern void *__ts_call_on_stack( void *sp, void *(*fn)( void * ), void *arg );

typedef struct {
	PTH	*p;
	void	*pad;
} TCBLOCAL;

static void *thread_body( void *arg )
{
	PTH	*t = (PTH *)arg;

	t->ret = t->start(t->arg);
	run_dtors(t);
	return NULL;
}

static void give_back( PTH *t )
{
	if ( t->map != NULL ) {
		(void)ts_unm_mem(t->map, (SZ)t->mapsz);
		t->map = NULL;
	}
}

/* Where pthread_exit goes back to: the task's own stack */
typedef struct {
	jmp_buf	jb;
} EXITPT;

static void thread_entry( INT stacd, void *exinf )
{
	PTH		*t = (PTH *)exinf;
	TCBLOCAL	local;
	EXITPT		ex;
	PTH		*j;
	ID		sem;
	int		det;

	(void)stacd;
	set_tp(t->tls);
	((void **)t->tls)[1] = &ex;		/* the second word of the control block */
	if ( setjmp(ex.jb) == 0 ) {
		(void)__ts_call_on_stack(t->stk_hi, thread_body, t);
	}

	/*
	 * Back on the task's own stack. The thread's stack and TLS block may
	 * go now; TPIDR_EL0 is moved to a control block on this stack first.
	 */
	pth_emutls_free(t);
	local.p = t;
	local.pad = NULL;
	set_tp(&local);

	pth_glock();
	t->done = 1;
	det = t->detached;
	j = t->joiner;
	t->joiner = NULL;
	pth_gunlock();

	if ( det ) {
		sem = t->sem;
		give_back(t);
		free(t);
		local.p = NULL;
		if ( sem > 0 ) (void)tk_del_sem(sem);
	} else if ( j != NULL ) {
		pth_unpark(j);
	}
	tk_exd_tsk();
}

int pthread_create( pthread_t *th, const pthread_attr_t *attr,
		    void *(*start)( void * ), void *arg )
{
	PTH	*t;
	T_CTSK	c;
	T_RTSK	r;
	size_t	stk, tls;
	void	*map = NULL;
	ID	tid;

	stk = ( attr != NULL && attr->__stacksize != 0 ) ? attr->__stacksize : PTH_DEF_STACK;
	if ( stk < PTHREAD_STACK_MIN ) stk = PTHREAD_STACK_MIN;
	stk = up(stk, PAGE);
	tls = up(pth_tls_size(), PAGE);

	t = (PTH *)calloc(1, sizeof(PTH));
	if ( t == NULL ) return EAGAIN;

	t->mapsz = PAGE + stk + tls;			/* a page never made, the stack, the TLS */
	if ( ts_map_mem(&map, (SZ)t->mapsz, 0, TS_MEM_NONE) < E_OK
	  || ts_ctl_mem((char *)map + PAGE, (SZ)( stk + tls ), TS_MEM_RW) < E_OK ) {
		if ( map != NULL ) (void)ts_unm_mem(map, (SZ)t->mapsz);
		free(t);
		return EAGAIN;
	}
	t->map = map;
	t->stk_lo = (char *)map + PAGE;
	t->stk_hi = (char *)t->stk_lo + stk;
	t->tls = pth_tls_make(t->stk_hi, t);
	t->start = start;
	t->arg = arg;
	t->detached = ( attr != NULL && attr->__detach == PTHREAD_CREATE_DETACHED );
	strcpy(t->name, "thread");

	if ( tk_ref_tsk(TSK_SELF, &r) < E_OK ) {
		r.tskpri = r.tskbpri = 16;
	}
	memset(&c, 0, sizeof(c));
	c.exinf = t;
	c.tskatr = TA_HLNG | TA_RNG3;
	c.task = (FP)thread_entry;
	c.itskpri = r.tskbpri;
	c.stksz = TASK_STK;
	tid = tk_cre_tsk(&c);
	if ( tid <= 0 ) {
		give_back(t);
		free(t);
		return EAGAIN;
	}
	t->tid = tid;
	*th = (pthread_t)t;
	if ( tk_sta_tsk(tid, 0) < E_OK ) {
		(void)tk_del_tsk(tid);
		give_back(t);
		free(t);
		return EAGAIN;
	}
	return 0;
}

/* The task has gone (tk_exd_tsk), so its number can be made again */
static void task_gone( ID tid )
{
	T_RTSK	r;
	int	i;

	for ( i = 0; i < 200 && tk_ref_tsk(tid, &r) >= E_OK; i++ ) {
		tk_dly_tsk(1);
	}
}

static void reap( PTH *t )
{
	task_gone(t->tid);
	give_back(t);
	if ( t->sem > 0 ) (void)tk_del_sem(t->sem);
	free(t);
}

int pthread_join( pthread_t th, void **ret )
{
	PTH	*t = (PTH *)th, *me = pth_me();

	if ( t == NULL || t == me ) return ( t == me ) ? EDEADLK : ESRCH;
	for ( ;; ) {
		pth_glock();
		if ( t->done ) {
			pth_gunlock();
			break;
		}
		if ( t->detached || t->joiner != NULL ) {
			pth_gunlock();
			return EINVAL;
		}
		t->joiner = me;
		pth_prepare(me);
		pth_gunlock();
		(void)pth_park(me, 0);
	}
	if ( ret != NULL ) *ret = t->ret;
	reap(t);
	return 0;
}

int pthread_detach( pthread_t th )
{
	PTH	*t = (PTH *)th;
	int	done;

	if ( t == NULL || t == &main_pth ) return ESRCH;
	pth_glock();
	done = t->done;
	t->detached = 1;
	pth_gunlock();
	if ( done ) {
		reap(t);
	}
	return 0;
}

void pthread_exit( void *ret )
{
	PTH	*me = pth_me();

	if ( me == NULL || me == &main_pth ) {
		exit(0);
	}
	me->ret = ret;
	run_dtors(me);
	longjmp(((EXITPT *)((void **)me->tls)[1])->jb, 1);
}

pthread_t pthread_self( void )
{
	return (pthread_t)pth_me();
}

int pthread_equal( pthread_t a, pthread_t b )
{
	return a == b;
}

int pthread_ts_tid( pthread_t th )
{
	return ( th != 0 ) ? ((PTH *)th)->tid : 0;
}

void pthread_ts_stack( void **lo, void **hi )
{
	PTH	*me = pth_me();

	*lo = ( me != NULL ) ? me->stk_lo : NULL;
	*hi = ( me != NULL ) ? me->stk_hi : NULL;
}

int pthread_setname_np( pthread_t th, const char *name )
{
	PTH	*t = (PTH *)th;

	if ( t == NULL ) return ESRCH;
	strncpy(t->name, name, PTH_NAME_MAX - 1);
	t->name[PTH_NAME_MAX - 1] = 0;
	return 0;
}

int pthread_getname_np( pthread_t th, char *name, size_t len )
{
	PTH	*t = (PTH *)th;

	if ( t == NULL ) return ESRCH;
	if ( len == 0 ) return ERANGE;
	strncpy(name, t->name, len - 1);
	name[len - 1] = 0;
	return 0;
}

int pthread_getattr_np( pthread_t th, pthread_attr_t *a )
{
	PTH	*t = (PTH *)th;

	if ( t == NULL ) return ESRCH;
	pthread_attr_init(a);
	a->__stackaddr = t->stk_lo;
	a->__stacksize = (size_t)( (char *)t->stk_hi - (char *)t->stk_lo );
	a->__detach = t->detached;
	return 0;
}

int pthread_setcancelstate( int state, int *old )
{
	(void)state;
	if ( old != NULL ) *old = PTHREAD_CANCEL_ENABLE;
	return 0;
}

int pthread_kill( pthread_t th, int sig )
{
	(void)th;
	return ( sig == 0 ) ? 0 : ENOSYS;
}

int pthread_sigmask( int how, const sigset_t *set, sigset_t *old )
{
	(void)how; (void)set; (void)old;
	return 0;
}

/* ---------------------------------------------------------------- attributes */

int pthread_attr_init( pthread_attr_t *a )
{
	memset(a, 0, sizeof(*a));
	a->__stacksize = PTH_DEF_STACK;
	return 0;
}

int pthread_attr_destroy( pthread_attr_t *a )
{
	(void)a;
	return 0;
}

int pthread_attr_setstacksize( pthread_attr_t *a, size_t size )
{
	if ( size < PTHREAD_STACK_MIN ) return EINVAL;
	a->__stacksize = size;
	return 0;
}

int pthread_attr_getstacksize( const pthread_attr_t *a, size_t *size )
{
	*size = a->__stacksize;
	return 0;
}

int pthread_attr_getstack( const pthread_attr_t *a, void **addr, size_t *size )
{
	*addr = a->__stackaddr;
	*size = a->__stacksize;
	return 0;
}

int pthread_attr_setstack( pthread_attr_t *a, void *addr, size_t size )
{
	(void)addr;
	return pthread_attr_setstacksize(a, size);	/* the stack is always made here */
}

int pthread_attr_setdetachstate( pthread_attr_t *a, int st )
{
	a->__detach = st;
	return 0;
}

int pthread_attr_getdetachstate( const pthread_attr_t *a, int *st )
{
	*st = a->__detach;
	return 0;
}

int pthread_attr_setguardsize( pthread_attr_t *a, size_t size )
{
	(void)a; (void)size;
	return 0;
}

int pthread_attr_getguardsize( const pthread_attr_t *a, size_t *size )
{
	(void)a;
	*size = PAGE;
	return 0;
}

int pthread_attr_setscope( pthread_attr_t *a, int scope )
{
	(void)a;
	return ( scope == PTHREAD_SCOPE_SYSTEM ) ? 0 : ENOTSUP;
}

int pthread_attr_setinheritsched( pthread_attr_t *a, int inh )
{
	(void)a; (void)inh;
	return 0;
}

/* Every thread runs at the priority the process started at */
int pthread_getschedparam( pthread_t th, int *policy, struct sched_param *param )
{
	(void)th;
	if ( policy != NULL ) *policy = SCHED_OTHER;
	if ( param != NULL ) param->sched_priority = 0;
	return 0;
}

int pthread_setschedparam( pthread_t th, int policy, const struct sched_param *param )
{
	(void)th; (void)policy; (void)param;
	return 0;
}

int pthread_setschedprio( pthread_t th, int prio )
{
	(void)th; (void)prio;
	return 0;
}

int sched_yield( void )
{
	(void)tk_rot_rdq(TPRI_RUN);
	return 0;
}
