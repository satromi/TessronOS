/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bits/gthr-default.h
 *	The thread interface libstdc++ is written against (__gthread_*),
 *	on lib/libpthread (design 17.19).
 *
 *	The toolchain's libstdc++ was configured for one thread and has no
 *	std::mutex, std::condition_variable or std::thread. With this
 *	directory ahead of the toolchain's on the include path, its headers
 *	find this file and bits/c++config.h beside it, which turns the
 *	threads on; the few functions that are not in headers are in
 *	lib/libcxxrt (cxx_thread.cc).
 */

#ifndef __TSCXX_GTHR_DEFAULT_H__
#define __TSCXX_GTHR_DEFAULT_H__

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <errno.h>

#define __GTHREADS		1
#define __GTHREADS_CXX0X	1

typedef pthread_t		__gthread_t;
typedef pthread_key_t		__gthread_key_t;
typedef pthread_once_t		__gthread_once_t;
typedef pthread_mutex_t		__gthread_mutex_t;
typedef pthread_mutex_t		__gthread_recursive_mutex_t;
typedef pthread_cond_t		__gthread_cond_t;
typedef struct timespec		__gthread_time_t;

#define __GTHREAD_HAS_COND	1
#define __GTHREAD_MUTEX_INIT		PTHREAD_MUTEX_INITIALIZER
#define __GTHREAD_RECURSIVE_MUTEX_INIT	PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define __GTHREAD_ONCE_INIT		PTHREAD_ONCE_INIT
#define __GTHREAD_COND_INIT		PTHREAD_COND_INITIALIZER
#define __GTHREAD_TIME_INIT		{ 0, 0 }

#ifdef __cplusplus
#define __GTHREAD_INLINE	inline __attribute__((__always_inline__))
#else
#define __GTHREAD_INLINE	static inline
#endif

__GTHREAD_INLINE int __gthread_active_p( void )
{
	return 1;
}

__GTHREAD_INLINE int __gthread_create( __gthread_t *t, void *(*fn)( void * ), void *arg )
{
	return pthread_create(t, 0, fn, arg);
}

__GTHREAD_INLINE int __gthread_join( __gthread_t t, void **ret )
{
	return pthread_join(t, ret);
}

__GTHREAD_INLINE int __gthread_detach( __gthread_t t )
{
	return pthread_detach(t);
}

__GTHREAD_INLINE int __gthread_equal( __gthread_t a, __gthread_t b )
{
	return pthread_equal(a, b);
}

__GTHREAD_INLINE __gthread_t __gthread_self( void )
{
	return pthread_self();
}

__GTHREAD_INLINE int __gthread_yield( void )
{
	return sched_yield();
}

__GTHREAD_INLINE int __gthread_once( __gthread_once_t *o, void (*fn)( void ) )
{
	return pthread_once(o, fn);
}

__GTHREAD_INLINE int __gthread_key_create( __gthread_key_t *k, void (*dtor)( void * ) )
{
	return pthread_key_create(k, dtor);
}

__GTHREAD_INLINE int __gthread_key_delete( __gthread_key_t k )
{
	return pthread_key_delete(k);
}

__GTHREAD_INLINE void *__gthread_getspecific( __gthread_key_t k )
{
	return pthread_getspecific(k);
}

__GTHREAD_INLINE int __gthread_setspecific( __gthread_key_t k, const void *v )
{
	return pthread_setspecific(k, v);
}

__GTHREAD_INLINE int __gthread_mutex_init_function_tf( __gthread_mutex_t *m )
{
	return pthread_mutex_init(m, 0);
}

__GTHREAD_INLINE int __gthread_mutex_destroy( __gthread_mutex_t *m )
{
	return pthread_mutex_destroy(m);
}

__GTHREAD_INLINE int __gthread_mutex_lock( __gthread_mutex_t *m )
{
	return pthread_mutex_lock(m);
}

__GTHREAD_INLINE int __gthread_mutex_trylock( __gthread_mutex_t *m )
{
	return pthread_mutex_trylock(m);
}

__GTHREAD_INLINE int __gthread_mutex_timedlock( __gthread_mutex_t *m, const __gthread_time_t *t )
{
	return pthread_mutex_timedlock(m, t);
}

__GTHREAD_INLINE int __gthread_mutex_unlock( __gthread_mutex_t *m )
{
	return pthread_mutex_unlock(m);
}

__GTHREAD_INLINE int __gthread_recursive_mutex_lock( __gthread_recursive_mutex_t *m )
{
	return pthread_mutex_lock(m);
}

__GTHREAD_INLINE int __gthread_recursive_mutex_trylock( __gthread_recursive_mutex_t *m )
{
	return pthread_mutex_trylock(m);
}

__GTHREAD_INLINE int __gthread_recursive_mutex_timedlock( __gthread_recursive_mutex_t *m,
							   const __gthread_time_t *t )
{
	return pthread_mutex_timedlock(m, t);
}

__GTHREAD_INLINE int __gthread_recursive_mutex_unlock( __gthread_recursive_mutex_t *m )
{
	return pthread_mutex_unlock(m);
}

__GTHREAD_INLINE int __gthread_recursive_mutex_destroy( __gthread_recursive_mutex_t *m )
{
	return pthread_mutex_destroy(m);
}

__GTHREAD_INLINE int __gthread_cond_broadcast( __gthread_cond_t *c )
{
	return pthread_cond_broadcast(c);
}

__GTHREAD_INLINE int __gthread_cond_signal( __gthread_cond_t *c )
{
	return pthread_cond_signal(c);
}

__GTHREAD_INLINE int __gthread_cond_wait( __gthread_cond_t *c, __gthread_mutex_t *m )
{
	return pthread_cond_wait(c, m);
}

__GTHREAD_INLINE int __gthread_cond_timedwait( __gthread_cond_t *c, __gthread_mutex_t *m,
						const __gthread_time_t *t )
{
	return pthread_cond_timedwait(c, m, t);
}

__GTHREAD_INLINE int __gthread_cond_wait_recursive( __gthread_cond_t *c,
						     __gthread_recursive_mutex_t *m )
{
	return pthread_cond_wait(c, m);
}

__GTHREAD_INLINE int __gthread_cond_destroy( __gthread_cond_t *c )
{
	return pthread_cond_destroy(c);
}

#endif /* __TSCXX_GTHR_DEFAULT_H__ */
