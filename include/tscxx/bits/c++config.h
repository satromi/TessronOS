/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bits/c++config.h
 *	The toolchain's libstdc++ configuration with threads turned on
 *	(bits/gthr-default.h beside this file, design 17.19).
 *
 *	The toolchain's file is read first, as it is; what follows only says
 *	that there are threads, thread local storage and the POSIX calls
 *	lib/libpthread gives, so that std::mutex, std::condition_variable,
 *	std::thread, std::call_once and std::shared_mutex are declared.
 */

#include_next <bits/c++config.h>

#ifndef __TSCXX_CXX_CONFIG_H__
#define __TSCXX_CXX_CONFIG_H__

#undef _GLIBCXX_HAS_GTHREADS
#define _GLIBCXX_HAS_GTHREADS 1

#undef _GLIBCXX_HAVE_TLS
#define _GLIBCXX_HAVE_TLS 1

#undef _GTHREAD_USE_MUTEX_TIMEDLOCK
#define _GTHREAD_USE_MUTEX_TIMEDLOCK 1

#undef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
#define _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT 1
#undef _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK
#define _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK 1
#undef _GLIBCXX_USE_PTHREAD_RWLOCK_CLOCKLOCK
#define _GLIBCXX_USE_PTHREAD_RWLOCK_CLOCKLOCK 1
#undef _GLIBCXX_USE_PTHREAD_RWLOCK_T
#define _GLIBCXX_USE_PTHREAD_RWLOCK_T 1

#undef _GLIBCXX_USE_SCHED_YIELD
#define _GLIBCXX_USE_SCHED_YIELD 1
#undef _GLIBCXX_USE_NANOSLEEP
#define _GLIBCXX_USE_NANOSLEEP 1
#undef _GLIBCXX_USE_CLOCK_MONOTONIC
#define _GLIBCXX_USE_CLOCK_MONOTONIC 1
#undef _GLIBCXX_USE_CLOCK_REALTIME
#define _GLIBCXX_USE_CLOCK_REALTIME 1

#endif /* __TSCXX_CXX_CONFIG_H__ */
