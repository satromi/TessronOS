/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pthread.h
 *	POSIX threads for a program running as a process (lib/libpthread,
 *	design 17.19).
 *
 *	A thread is a task of the process (tk_cre_tsk): a process has at
 *	most eight, the main task among them. Its stack is memory the
 *	process asked for (include/ts/umem.h) with a page that is never
 *	made below it, and its thread local storage is pointed at by
 *	TPIDR_EL0, which the kernel keeps for each task.
 *
 *	Mutexes, condition variables, read-write locks, once and the
 *	semaphores of <semaphore.h> live in the program's own memory and
 *	are taken with atomic operations while nobody waits. A thread that
 *	has to wait sleeps on a semaphore of the kernel that is its own
 *	(one per thread); the lists of who waits are kept under one mutex
 *	of the kernel for the whole process. So a program uses one kernel
 *	mutex and a semaphore per thread, whatever number of locks it has.
 */

#ifndef __TSPOSIX_PTHREAD_H__
#define __TSPOSIX_PTHREAD_H__

#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <signal.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _POSIX_THREADS
#define _POSIX_THREADS			200809L
#endif
#ifndef _POSIX_TIMEOUTS
#define _POSIX_TIMEOUTS			200809L
#endif
#ifndef _POSIX_READER_WRITER_LOCKS
#define _POSIX_READER_WRITER_LOCKS	200809L
#endif

/* The types are in <sys/_pthreadtypes.h>, which <sys/types.h> reads */

#define PTHREAD_MUTEX_NORMAL		0
#define PTHREAD_MUTEX_RECURSIVE		1
#define PTHREAD_MUTEX_ERRORCHECK	2
#define PTHREAD_MUTEX_DEFAULT		PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE_NP	PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_ERRORCHECK_NP	PTHREAD_MUTEX_ERRORCHECK

#define PTHREAD_CREATE_JOINABLE		0
#define PTHREAD_CREATE_DETACHED		1

#define PTHREAD_PROCESS_PRIVATE		0
#define PTHREAD_PROCESS_SHARED		1

/* Priority protocols of a mutex: taken, and every mutex is PTHREAD_PRIO_NONE */
#define PTHREAD_PRIO_NONE		0
#define PTHREAD_PRIO_INHERIT		1
#define PTHREAD_PRIO_PROTECT		2

#define PTHREAD_SCOPE_SYSTEM		0
#define PTHREAD_INHERIT_SCHED		0
#define PTHREAD_EXPLICIT_SCHED		1

#define PTHREAD_CANCEL_ENABLE		0
#define PTHREAD_CANCEL_DISABLE		1
#define PTHREAD_CANCELED		((void *)-1)

#define PTHREAD_KEYS_MAX		128
#define PTHREAD_DESTRUCTOR_ITERATIONS	4
#define PTHREAD_STACK_MIN		16384
#define PTHREAD_THREADS_MAX		8

#define PTHREAD_MUTEX_INITIALIZER	{ 0, PTHREAD_MUTEX_NORMAL, 0, 0, 0, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP \
					{ 0, PTHREAD_MUTEX_RECURSIVE, 0, 0, 0, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP \
					{ 0, PTHREAD_MUTEX_ERRORCHECK, 0, 0, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER	{ 0, 0, 0, 0 }
#define PTHREAD_RWLOCK_INITIALIZER	{ PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, \
					  PTHREAD_COND_INITIALIZER, 0, 0, 0, 0 }
#define PTHREAD_ONCE_INIT		{ 0 }


/* Threads */
int	pthread_create( pthread_t *th, const pthread_attr_t *attr,
			void *(*start)( void * ), void *arg );
int	pthread_join( pthread_t th, void **ret );
int	pthread_detach( pthread_t th );
void	pthread_exit( void *ret ) __attribute__((noreturn));
pthread_t pthread_self( void );
int	pthread_equal( pthread_t a, pthread_t b );
int	pthread_setname_np( pthread_t th, const char *name );
int	pthread_getname_np( pthread_t th, char *name, size_t len );
int	pthread_getattr_np( pthread_t th, pthread_attr_t *attr );
int	pthread_setcancelstate( int state, int *old );
int	pthread_getschedparam( pthread_t th, int *policy, struct sched_param *param );
int	pthread_setschedparam( pthread_t th, int policy, const struct sched_param *param );
int	pthread_setschedprio( pthread_t th, int prio );
/* pthread_kill and pthread_sigmask are declared by <signal.h>: no signal is ever sent */

int	pthread_attr_init( pthread_attr_t *a );
int	pthread_attr_destroy( pthread_attr_t *a );
int	pthread_attr_setstacksize( pthread_attr_t *a, size_t size );
int	pthread_attr_getstacksize( const pthread_attr_t *a, size_t *size );
int	pthread_attr_getstack( const pthread_attr_t *a, void **addr, size_t *size );
int	pthread_attr_setstack( pthread_attr_t *a, void *addr, size_t size );
int	pthread_attr_setdetachstate( pthread_attr_t *a, int st );
int	pthread_attr_getdetachstate( const pthread_attr_t *a, int *st );
int	pthread_attr_setguardsize( pthread_attr_t *a, size_t size );
int	pthread_attr_getguardsize( const pthread_attr_t *a, size_t *size );
int	pthread_attr_setscope( pthread_attr_t *a, int scope );
int	pthread_attr_setinheritsched( pthread_attr_t *a, int inh );

/* Mutexes */
int	pthread_mutex_init( pthread_mutex_t *m, const pthread_mutexattr_t *attr );
int	pthread_mutex_destroy( pthread_mutex_t *m );
int	pthread_mutex_lock( pthread_mutex_t *m );
int	pthread_mutex_trylock( pthread_mutex_t *m );
int	pthread_mutex_timedlock( pthread_mutex_t *m, const struct timespec *abstime );
int	pthread_mutex_clocklock( pthread_mutex_t *m, clockid_t clk, const struct timespec *abstime );
int	pthread_mutex_unlock( pthread_mutex_t *m );
int	pthread_mutexattr_init( pthread_mutexattr_t *a );
int	pthread_mutexattr_destroy( pthread_mutexattr_t *a );
int	pthread_mutexattr_settype( pthread_mutexattr_t *a, int type );
int	pthread_mutexattr_gettype( const pthread_mutexattr_t *a, int *type );
int	pthread_mutexattr_setpshared( pthread_mutexattr_t *a, int ps );
int	pthread_mutexattr_setprotocol( pthread_mutexattr_t *a, int protocol );
int	pthread_mutexattr_getprotocol( const pthread_mutexattr_t *a, int *protocol );

/* Condition variables */
int	pthread_cond_init( pthread_cond_t *c, const pthread_condattr_t *attr );
int	pthread_cond_destroy( pthread_cond_t *c );
int	pthread_cond_wait( pthread_cond_t *c, pthread_mutex_t *m );
int	pthread_cond_timedwait( pthread_cond_t *c, pthread_mutex_t *m,
				const struct timespec *abstime );
int	pthread_cond_clockwait( pthread_cond_t *c, pthread_mutex_t *m, clockid_t clk,
				const struct timespec *abstime );
int	pthread_cond_signal( pthread_cond_t *c );
int	pthread_cond_broadcast( pthread_cond_t *c );
int	pthread_condattr_init( pthread_condattr_t *a );
int	pthread_condattr_destroy( pthread_condattr_t *a );
int	pthread_condattr_setclock( pthread_condattr_t *a, clockid_t clk );
int	pthread_condattr_getclock( const pthread_condattr_t *a, clockid_t *clk );
int	pthread_condattr_setpshared( pthread_condattr_t *a, int ps );

/* Read-write locks */
int	pthread_rwlock_init( pthread_rwlock_t *l, const pthread_rwlockattr_t *attr );
int	pthread_rwlock_destroy( pthread_rwlock_t *l );
int	pthread_rwlock_rdlock( pthread_rwlock_t *l );
int	pthread_rwlock_tryrdlock( pthread_rwlock_t *l );
int	pthread_rwlock_timedrdlock( pthread_rwlock_t *l, const struct timespec *abstime );
int	pthread_rwlock_clockrdlock( pthread_rwlock_t *l, clockid_t clk, const struct timespec *abstime );
int	pthread_rwlock_wrlock( pthread_rwlock_t *l );
int	pthread_rwlock_trywrlock( pthread_rwlock_t *l );
int	pthread_rwlock_timedwrlock( pthread_rwlock_t *l, const struct timespec *abstime );
int	pthread_rwlock_clockwrlock( pthread_rwlock_t *l, clockid_t clk, const struct timespec *abstime );
int	pthread_rwlock_unlock( pthread_rwlock_t *l );
int	pthread_rwlockattr_init( pthread_rwlockattr_t *a );
int	pthread_rwlockattr_destroy( pthread_rwlockattr_t *a );

/* Spin locks and barriers */
int	pthread_spin_init( pthread_spinlock_t *s, int pshared );
int	pthread_spin_destroy( pthread_spinlock_t *s );
int	pthread_spin_lock( pthread_spinlock_t *s );
int	pthread_spin_trylock( pthread_spinlock_t *s );
int	pthread_spin_unlock( pthread_spinlock_t *s );
int	pthread_barrier_init( pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count );
int	pthread_barrier_destroy( pthread_barrier_t *b );
int	pthread_barrier_wait( pthread_barrier_t *b );
#define PTHREAD_BARRIER_SERIAL_THREAD	(-1)

/* Once and thread specific data */
int	pthread_once( pthread_once_t *once, void (*fn)( void ) );
int	pthread_key_create( pthread_key_t *key, void (*dtor)( void * ) );
int	pthread_key_delete( pthread_key_t key );
void	*pthread_getspecific( pthread_key_t key );
int	pthread_setspecific( pthread_key_t key, const void *val );

/*
 * What this library adds: the task of a thread, and the stack of the
 * calling thread (its lowest and highest addresses).
 */
int	pthread_ts_tid( pthread_t th );
void	pthread_ts_stack( void **lo, void **hi );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_PTHREAD_H__ */
