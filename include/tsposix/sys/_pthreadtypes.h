/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/_pthreadtypes.h
 *	The types of the POSIX threads of lib/libpthread (include/tsposix/
 *	pthread.h). The C library's <sys/types.h> reads this file; being
 *	ahead of the library's own on the include path, it gives the types
 *	their shape here and not the library's placeholder one.
 */

#ifndef __TSPOSIX_SYS_PTHREADTYPES_H__
#define __TSPOSIX_SYS_PTHREADTYPES_H__
#define _SYS__PTHREADTYPES_H_

#include <stddef.h>

typedef unsigned long	pthread_t;		/* the thread's record */
typedef unsigned int	pthread_key_t;

typedef struct {
	size_t	__stacksize;
	int	__detach;
	int	__prio;
	void	*__stackaddr;
} pthread_attr_t;

typedef struct {
	volatile int	__state;	/* 0 free, 1 held, 2 held and waited for */
	int		__type;
	volatile unsigned long __owner;
	int		__count;
	int		__pad;
	void		*__head, *__tail;	/* who waits */
} pthread_mutex_t;

typedef struct {
	int	__type;
	int	__pshared;
} pthread_mutexattr_t;

typedef struct {
	void	*__head, *__tail;	/* who waits */
	int	__clock;
	int	__pad;
} pthread_cond_t;

typedef struct {
	int	__clock;
	int	__pshared;
} pthread_condattr_t;

typedef struct {
	pthread_mutex_t	__m;
	pthread_cond_t	__rc, __wc;
	int		__readers;	/* holding it to read */
	int		__writer;	/* holding it to write */
	int		__wwait;	/* waiting to write */
	int		__pad;
} pthread_rwlock_t;

typedef struct {
	int	__pshared;
} pthread_rwlockattr_t;

typedef struct {
	volatile int	__state;	/* 0 not yet, 1 running, 2 done */
} pthread_once_t;

typedef pthread_mutex_t	pthread_spinlock_t;

typedef struct {
	pthread_mutex_t	__m;
	pthread_cond_t	__c;
	unsigned	__count, __left, __gen, __pad;
} pthread_barrier_t;

typedef struct {
	int	__pshared;
} pthread_barrierattr_t;

#endif /* __TSPOSIX_SYS_PTHREADTYPES_H__ */
