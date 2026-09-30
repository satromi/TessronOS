/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_sync.c
 *	Mutexes, condition variables, read-write locks, once, spin locks,
 *	barriers and the semaphores of <semaphore.h>.
 *
 *	A mutex is a word: 0 free, 1 held, 2 held with someone waiting.
 *	Taking a free one and giving back one nobody waits for are one
 *	atomic operation each; the kernel is called only when there is
 *	someone to wait or to wake (pth_wait.c).
 */

#include <errno.h>
#include <string.h>
#include <semaphore.h>
#include "pth_int.h"

#define SPINS	64

static inline int xchg( volatile int *p, int v )
{
	return __atomic_exchange_n(p, v, __ATOMIC_ACQUIRE);
}

static inline int cas( volatile int *p, int old, int nv )
{
	return __atomic_compare_exchange_n(p, &old, nv, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline unsigned long self_id( void )
{
	return (unsigned long)pth_me();
}

/* ---------------------------------------------------------------- mutexes */

int pthread_mutexattr_init( pthread_mutexattr_t *a )
{
	a->__type = PTHREAD_MUTEX_DEFAULT;
	a->__pshared = 0;
	return 0;
}

int pthread_mutexattr_destroy( pthread_mutexattr_t *a )
{
	(void)a;
	return 0;
}

int pthread_mutexattr_settype( pthread_mutexattr_t *a, int type )
{
	if ( type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK ) return EINVAL;
	a->__type = type;
	return 0;
}

int pthread_mutexattr_gettype( const pthread_mutexattr_t *a, int *type )
{
	*type = a->__type;
	return 0;
}

int pthread_mutexattr_setpshared( pthread_mutexattr_t *a, int ps )
{
	return ( ps == PTHREAD_PROCESS_PRIVATE ) ? ( a->__pshared = ps, 0 ) : ENOTSUP;
}

/*
 * A mutex's priority protocol: the tasks of a process wait on their own
 * semaphores, not on a kernel mutex, so no priority is lent; the request
 * is taken and the protocol stays PTHREAD_PRIO_NONE.
 */
int pthread_mutexattr_setprotocol( pthread_mutexattr_t *a, int protocol )
{
	(void)a;
	return ( protocol >= PTHREAD_PRIO_NONE && protocol <= PTHREAD_PRIO_PROTECT ) ? 0 : EINVAL;
}

int pthread_mutexattr_getprotocol( const pthread_mutexattr_t *a, int *protocol )
{
	(void)a;
	*protocol = PTHREAD_PRIO_NONE;
	return 0;
}

int pthread_mutex_init( pthread_mutex_t *m, const pthread_mutexattr_t *attr )
{
	memset(m, 0, sizeof(*m));
	m->__type = ( attr != NULL ) ? attr->__type : PTHREAD_MUTEX_DEFAULT;
	return 0;
}

int pthread_mutex_destroy( pthread_mutex_t *m )
{
	return ( m->__state != 0 ) ? EBUSY : 0;
}

/* The mutex taken, waiting until the monotonic time dl (0: for ever) */
int pth_mutex_lock_dl( pthread_mutex_t *m, uint64_t dl )
{
	PTH	*me = pth_me();
	int	i;

	if ( m->__type != PTHREAD_MUTEX_NORMAL && m->__owner == (unsigned long)me && me != NULL ) {
		if ( m->__type == PTHREAD_MUTEX_ERRORCHECK ) return EDEADLK;
		m->__count++;
		return 0;
	}
	if ( !cas(&m->__state, 0, 1) ) {
		for ( i = 0; i < SPINS; i++ ) {
			__asm__ volatile ( "yield" );
			if ( m->__state == 0 && cas(&m->__state, 0, 1) ) goto got;
		}
		for ( ;; ) {
			pth_glock();
			if ( xchg(&m->__state, 2) == 0 ) {
				pth_gunlock();
				break;			/* taken; others may still wait */
			}
			pth_enq((void **)&m->__head, me);
			pth_gunlock();
			if ( pth_park(me, dl) != 0 ) {
				int	was;

				pth_glock();
				was = pth_remove((void **)&m->__head, me);
				pth_gunlock();
				if ( was ) {
					me->queued = 0;
					return ETIMEDOUT;
				}
				(void)pth_park(me, 0);	/* the wake on its way */
			}
		}
	}
    got:
	m->__owner = (unsigned long)me;
	m->__count = 1;
	return 0;
}

int pthread_mutex_lock( pthread_mutex_t *m )
{
	return pth_mutex_lock_dl(m, 0);
}

int pthread_mutex_timedlock( pthread_mutex_t *m, const struct timespec *abstime )
{
	return pth_mutex_lock_dl(m, pth_deadline(CLOCK_REALTIME, abstime));
}

int pthread_mutex_clocklock( pthread_mutex_t *m, clockid_t clk, const struct timespec *abstime )
{
	return pth_mutex_lock_dl(m, pth_deadline(clk, abstime));
}

int pthread_mutex_trylock( pthread_mutex_t *m )
{
	PTH	*me = pth_me();

	if ( m->__type == PTHREAD_MUTEX_RECURSIVE && m->__owner == (unsigned long)me && me != NULL ) {
		m->__count++;
		return 0;
	}
	if ( !cas(&m->__state, 0, 1) ) {
		return EBUSY;
	}
	m->__owner = (unsigned long)me;
	m->__count = 1;
	return 0;
}

int pthread_mutex_unlock( pthread_mutex_t *m )
{
	if ( m->__type != PTHREAD_MUTEX_NORMAL ) {
		if ( m->__owner != self_id() ) return EPERM;
		if ( --m->__count > 0 ) return 0;
	}
	m->__owner = 0;
	if ( __atomic_exchange_n(&m->__state, 0, __ATOMIC_RELEASE) == 2 ) {
		PTH	*t;

		pth_glock();
		t = pth_deq((void **)&m->__head);
		pth_gunlock();
		if ( t != NULL ) pth_unpark(t);
	}
	return 0;
}

/* ---------------------------------------------------------------- condition variables */

int pthread_condattr_init( pthread_condattr_t *a )
{
	a->__clock = CLOCK_REALTIME;
	a->__pshared = 0;
	return 0;
}

int pthread_condattr_destroy( pthread_condattr_t *a )
{
	(void)a;
	return 0;
}

int pthread_condattr_setclock( pthread_condattr_t *a, clockid_t clk )
{
	if ( clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC ) return EINVAL;
	a->__clock = (int)clk;
	return 0;
}

int pthread_condattr_getclock( const pthread_condattr_t *a, clockid_t *clk )
{
	*clk = (clockid_t)a->__clock;
	return 0;
}

int pthread_condattr_setpshared( pthread_condattr_t *a, int ps )
{
	return ( ps == PTHREAD_PROCESS_PRIVATE ) ? ( a->__pshared = ps, 0 ) : ENOTSUP;
}

int pthread_cond_init( pthread_cond_t *c, const pthread_condattr_t *attr )
{
	memset(c, 0, sizeof(*c));
	c->__clock = ( attr != NULL ) ? attr->__clock : CLOCK_REALTIME;
	return 0;
}

int pthread_cond_destroy( pthread_cond_t *c )
{
	return ( c->__head != NULL ) ? EBUSY : 0;
}

static int cond_wait_dl( pthread_cond_t *c, pthread_mutex_t *m, uint64_t dl )
{
	PTH	*me = pth_me();
	int	r = 0, count = m->__count;

	pth_glock();
	pth_enq((void **)&c->__head, me);
	pth_gunlock();

	/* let the mutex go whole, however deep a recursive one is held */
	m->__count = 1;
	(void)pthread_mutex_unlock(m);

	if ( pth_park(me, dl) != 0 ) {
		int	was;

		pth_glock();
		was = pth_remove((void **)&c->__head, me);
		pth_gunlock();
		if ( was ) {
			me->queued = 0;
			r = ETIMEDOUT;
		} else {
			(void)pth_park(me, 0);
		}
	}
	(void)pth_mutex_lock_dl(m, 0);
	m->__count = count;
	return r;
}

int pthread_cond_wait( pthread_cond_t *c, pthread_mutex_t *m )
{
	return cond_wait_dl(c, m, 0);
}

int pthread_cond_timedwait( pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime )
{
	return cond_wait_dl(c, m, pth_deadline((clockid_t)c->__clock, abstime));
}

int pthread_cond_clockwait( pthread_cond_t *c, pthread_mutex_t *m, clockid_t clk,
			    const struct timespec *abstime )
{
	return cond_wait_dl(c, m, pth_deadline(clk, abstime));
}

int pthread_cond_signal( pthread_cond_t *c )
{
	PTH	*t;

	if ( __atomic_load_n(&c->__head, __ATOMIC_ACQUIRE) == NULL ) {
		return 0;
	}
	pth_glock();
	t = pth_deq((void **)&c->__head);
	pth_gunlock();
	if ( t != NULL ) pth_unpark(t);
	return 0;
}

int pthread_cond_broadcast( pthread_cond_t *c )
{
	PTH	*t, *next;

	if ( __atomic_load_n(&c->__head, __ATOMIC_ACQUIRE) == NULL ) {
		return 0;
	}
	pth_glock();
	t = (PTH *)c->__head;
	c->__head = c->__tail = NULL;
	pth_gunlock();
	for ( ; t != NULL; t = next ) {
		next = t->qnext;
		t->qnext = NULL;
		pth_unpark(t);
	}
	return 0;
}

/* ---------------------------------------------------------------- read-write locks */

int pthread_rwlockattr_init( pthread_rwlockattr_t *a )
{
	a->__pshared = 0;
	return 0;
}

int pthread_rwlockattr_destroy( pthread_rwlockattr_t *a )
{
	(void)a;
	return 0;
}

int pthread_rwlock_init( pthread_rwlock_t *l, const pthread_rwlockattr_t *attr )
{
	(void)attr;
	memset(l, 0, sizeof(*l));
	l->__rc.__clock = CLOCK_MONOTONIC;
	l->__wc.__clock = CLOCK_MONOTONIC;
	return 0;
}

int pthread_rwlock_destroy( pthread_rwlock_t *l )
{
	return ( l->__readers != 0 || l->__writer != 0 ) ? EBUSY : 0;
}

static int rd_dl( pthread_rwlock_t *l, uint64_t dl, int try )
{
	int	r = 0;

	pth_mutex_lock_dl(&l->__m, 0);
	while ( l->__writer || l->__wwait > 0 ) {
		if ( try ) { r = EBUSY; break; }
		r = cond_wait_dl(&l->__rc, &l->__m, dl);
		if ( r != 0 ) break;
	}
	if ( r == 0 ) l->__readers++;
	pthread_mutex_unlock(&l->__m);
	return r;
}

static int wr_dl( pthread_rwlock_t *l, uint64_t dl, int try )
{
	int	r = 0;

	pth_mutex_lock_dl(&l->__m, 0);
	l->__wwait++;
	while ( l->__writer || l->__readers > 0 ) {
		if ( try ) { r = EBUSY; break; }
		r = cond_wait_dl(&l->__wc, &l->__m, dl);
		if ( r != 0 ) break;
	}
	l->__wwait--;
	if ( r == 0 ) {
		l->__writer = 1;
	} else if ( l->__wwait == 0 ) {
		pthread_cond_broadcast(&l->__rc);	/* readers held back for it */
	}
	pthread_mutex_unlock(&l->__m);
	return r;
}

int pthread_rwlock_rdlock( pthread_rwlock_t *l )	{ return rd_dl(l, 0, 0); }
int pthread_rwlock_tryrdlock( pthread_rwlock_t *l )	{ return rd_dl(l, 0, 1); }
int pthread_rwlock_wrlock( pthread_rwlock_t *l )	{ return wr_dl(l, 0, 0); }
int pthread_rwlock_trywrlock( pthread_rwlock_t *l )	{ return wr_dl(l, 0, 1); }

int pthread_rwlock_timedrdlock( pthread_rwlock_t *l, const struct timespec *t )
{
	return rd_dl(l, pth_deadline(CLOCK_REALTIME, t), 0);
}

int pthread_rwlock_timedwrlock( pthread_rwlock_t *l, const struct timespec *t )
{
	return wr_dl(l, pth_deadline(CLOCK_REALTIME, t), 0);
}

int pthread_rwlock_clockrdlock( pthread_rwlock_t *l, clockid_t clk, const struct timespec *t )
{
	return rd_dl(l, pth_deadline(clk, t), 0);
}

int pthread_rwlock_clockwrlock( pthread_rwlock_t *l, clockid_t clk, const struct timespec *t )
{
	return wr_dl(l, pth_deadline(clk, t), 0);
}

int pthread_rwlock_unlock( pthread_rwlock_t *l )
{
	pth_mutex_lock_dl(&l->__m, 0);
	if ( l->__writer ) {
		l->__writer = 0;
	} else if ( l->__readers > 0 ) {
		l->__readers--;
	}
	if ( l->__readers == 0 ) {
		if ( l->__wwait > 0 ) {
			pthread_cond_signal(&l->__wc);
		} else {
			pthread_cond_broadcast(&l->__rc);
		}
	}
	pthread_mutex_unlock(&l->__m);
	return 0;
}

/* ---------------------------------------------------------------- once */

static pthread_mutex_t	once_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	once_c = PTHREAD_COND_INITIALIZER;

int pthread_once( pthread_once_t *once, void (*fn)( void ) )
{
	if ( __atomic_load_n(&once->__state, __ATOMIC_ACQUIRE) == 2 ) {
		return 0;
	}
	pthread_mutex_lock(&once_m);
	while ( once->__state == 1 ) {
		pthread_cond_wait(&once_c, &once_m);
	}
	if ( once->__state == 2 ) {
		pthread_mutex_unlock(&once_m);
		return 0;
	}
	once->__state = 1;
	pthread_mutex_unlock(&once_m);

	fn();

	pthread_mutex_lock(&once_m);
	__atomic_store_n(&once->__state, 2, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&once_c);
	pthread_mutex_unlock(&once_m);
	return 0;
}

/* ---------------------------------------------------------------- spin locks, barriers */

int pthread_spin_init( pthread_spinlock_t *s, int pshared )
{
	(void)pshared;
	return pthread_mutex_init(s, NULL);
}

int pthread_spin_destroy( pthread_spinlock_t *s )	{ return pthread_mutex_destroy(s); }
int pthread_spin_lock( pthread_spinlock_t *s )		{ return pthread_mutex_lock(s); }
int pthread_spin_trylock( pthread_spinlock_t *s )	{ return pthread_mutex_trylock(s); }
int pthread_spin_unlock( pthread_spinlock_t *s )	{ return pthread_mutex_unlock(s); }

int pthread_barrier_init( pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count )
{
	(void)a;
	if ( count == 0 ) return EINVAL;
	memset(b, 0, sizeof(*b));
	b->__count = b->__left = count;
	return 0;
}

int pthread_barrier_destroy( pthread_barrier_t *b )
{
	(void)b;
	return 0;
}

int pthread_barrier_wait( pthread_barrier_t *b )
{
	unsigned gen;

	pthread_mutex_lock(&b->__m);
	gen = b->__gen;
	if ( --b->__left == 0 ) {
		b->__gen++;
		b->__left = b->__count;
		pthread_cond_broadcast(&b->__c);
		pthread_mutex_unlock(&b->__m);
		return PTHREAD_BARRIER_SERIAL_THREAD;
	}
	while ( gen == b->__gen ) {
		pthread_cond_wait(&b->__c, &b->__m);
	}
	pthread_mutex_unlock(&b->__m);
	return 0;
}

/* ---------------------------------------------------------------- semaphores */

int sem_init( sem_t *s, int pshared, unsigned value )
{
	if ( pshared ) {
		errno = ENOSYS;
		return -1;
	}
	memset(s, 0, sizeof(*s));
	s->__value = value;
	s->__c.__clock = CLOCK_REALTIME;
	return 0;
}

int sem_destroy( sem_t *s )
{
	(void)s;
	return 0;
}

static int sem_wait_dl( sem_t *s, uint64_t dl, int try )
{
	int	r = 0;

	pthread_mutex_lock(&s->__m);
	while ( s->__value == 0 ) {
		if ( try ) { r = EAGAIN; break; }
		r = cond_wait_dl(&s->__c, &s->__m, dl);
		if ( r != 0 ) break;
	}
	if ( r == 0 ) s->__value--;
	pthread_mutex_unlock(&s->__m);
	if ( r != 0 ) {
		errno = r;
		return -1;
	}
	return 0;
}

int sem_wait( sem_t *s )		{ return sem_wait_dl(s, 0, 0); }
int sem_trywait( sem_t *s )		{ return sem_wait_dl(s, 0, 1); }

int sem_timedwait( sem_t *s, const struct timespec *abstime )
{
	return sem_wait_dl(s, pth_deadline(CLOCK_REALTIME, abstime), 0);
}

int sem_clockwait( sem_t *s, clockid_t clk, const struct timespec *abstime )
{
	return sem_wait_dl(s, pth_deadline(clk, abstime), 0);
}

int sem_post( sem_t *s )
{
	pthread_mutex_lock(&s->__m);
	s->__value++;
	pthread_cond_signal(&s->__c);
	pthread_mutex_unlock(&s->__m);
	return 0;
}

int sem_getvalue( sem_t *s, int *v )
{
	*v = (int)s->__value;
	return 0;
}

/*
 * Handlers around fork: a process is never forked, so they are taken and
 * never called.
 */
int pthread_atfork( void (*prepare)( void ), void (*parent)( void ), void (*child)( void ) )
{
	(void)prepare; (void)parent; (void)child;
	return 0;
}
