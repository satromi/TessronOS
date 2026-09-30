/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_locks.c
 *	The locks the C library (newlib, built with retargetable locking)
 *	takes around the heap, the streams, the environment, the time zone
 *	and the exit handlers, made of the mutexes of lib/libpthread.
 *
 *	Every one is recursive: the library takes some of them again while
 *	it holds them (a stream flushed while the list of streams is locked).
 *	The library's own do-nothing versions (lock.o) stay out of the link
 *	because every name it has is defined here.
 */

#include <pthread.h>
#include <stdlib.h>
#include <sys/lock.h>

struct __lock {
	pthread_mutex_t	m;
};

#define STATIC_LOCK(name) \
	struct __lock __lock___##name = { PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP }

STATIC_LOCK(sinit_recursive_mutex);
STATIC_LOCK(sfp_recursive_mutex);
STATIC_LOCK(atexit_recursive_mutex);
STATIC_LOCK(at_quick_exit_mutex);
STATIC_LOCK(malloc_recursive_mutex);
STATIC_LOCK(env_recursive_mutex);
STATIC_LOCK(tz_mutex);
STATIC_LOCK(dd_hash_mutex);
STATIC_LOCK(arc4random_mutex);

static void lock_make( _LOCK_T *lock )
{
	pthread_mutexattr_t	a;
	struct __lock		*l = (struct __lock *)malloc(sizeof(*l));

	if ( l != NULL ) {
		pthread_mutexattr_init(&a);
		pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&l->m, &a);
	}
	*lock = l;
}

void __retarget_lock_init( _LOCK_T *lock )		{ lock_make(lock); }
void __retarget_lock_init_recursive( _LOCK_T *lock )	{ lock_make(lock); }

void __retarget_lock_close( _LOCK_T lock )
{
	if ( lock != NULL ) free(lock);
}

void __retarget_lock_close_recursive( _LOCK_T lock )
{
	if ( lock != NULL ) free(lock);
}

void __retarget_lock_acquire( _LOCK_T lock )
{
	if ( lock != NULL ) pthread_mutex_lock(&lock->m);
}

void __retarget_lock_acquire_recursive( _LOCK_T lock )
{
	if ( lock != NULL ) pthread_mutex_lock(&lock->m);
}

int __retarget_lock_try_acquire( _LOCK_T lock )
{
	return ( lock != NULL ) ? pthread_mutex_trylock(&lock->m) : 0;
}

int __retarget_lock_try_acquire_recursive( _LOCK_T lock )
{
	return ( lock != NULL ) ? pthread_mutex_trylock(&lock->m) : 0;
}

void __retarget_lock_release( _LOCK_T lock )
{
	if ( lock != NULL ) pthread_mutex_unlock(&lock->m);
}

void __retarget_lock_release_recursive( _LOCK_T lock )
{
	if ( lock != NULL ) pthread_mutex_unlock(&lock->m);
}
