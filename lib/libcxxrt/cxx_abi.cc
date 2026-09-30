/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cxx_abi.cc
 *	Parts of the C++ runtime (libsupc++) that the toolchain's copy,
 *	built for one thread, does for one thread only:
 *
 *	- The guards of function-local statics (__cxa_guard_*): a thread
 *	  that comes to a static another thread is making waits until it is
 *	  made, instead of making it again.
 *	- The exceptions being handled (__cxa_get_globals): one list for
 *	  each thread, in thread local storage.
 *
 *	The names are the runtime's, and this file comes before libstdc++ in
 *	the link, so those members of the library are not taken.
 */

#include <pthread.h>
#include <stdint.h>

namespace
{
	pthread_mutex_t	guard_m = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t	guard_c = PTHREAD_COND_INITIALIZER;

	/* the guard: byte 0 says made (the ABI's), byte 1 says being made */
	inline volatile uint8_t *gb( int64_t *g )
	{
		return reinterpret_cast<volatile uint8_t *>(g);
	}

	struct eh_globals {
		void		*caught;
		unsigned int	uncaught;
	};

	__thread eh_globals	eh;
}

extern "C" {

int __cxa_guard_acquire( int64_t *g )
{
	if ( __atomic_load_n(gb(g), __ATOMIC_ACQUIRE) != 0 ) {
		return 0;
	}
	pthread_mutex_lock(&guard_m);
	for ( ;; ) {
		if ( gb(g)[0] != 0 ) {
			pthread_mutex_unlock(&guard_m);
			return 0;
		}
		if ( gb(g)[1] == 0 ) {
			gb(g)[1] = 1;
			pthread_mutex_unlock(&guard_m);
			return 1;
		}
		pthread_cond_wait(&guard_c, &guard_m);
	}
}

void __cxa_guard_release( int64_t *g )
{
	pthread_mutex_lock(&guard_m);
	gb(g)[1] = 0;
	__atomic_store_n(gb(g), 1, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&guard_c);
	pthread_mutex_unlock(&guard_m);
}

void __cxa_guard_abort( int64_t *g )
{
	pthread_mutex_lock(&guard_m);
	gb(g)[1] = 0;
	pthread_cond_broadcast(&guard_c);
	pthread_mutex_unlock(&guard_m);
}

void *__cxa_get_globals()
{
	return &eh;
}

void *__cxa_get_globals_fast()
{
	return &eh;
}

}
