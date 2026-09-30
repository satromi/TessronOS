/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	semaphore.h
 *	POSIX semaphores between the threads of one process (lib/libpthread).
 *	Unnamed only; one shared between processes is refused (ENOSYS).
 */

#ifndef __TSPOSIX_SEMAPHORE_H__
#define __TSPOSIX_SEMAPHORE_H__

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	pthread_mutex_t	__m;
	pthread_cond_t	__c;
	unsigned	__value;
	unsigned	__pad;
} sem_t;

#define SEM_FAILED	((sem_t *)0)

int	sem_init( sem_t *s, int pshared, unsigned value );
int	sem_destroy( sem_t *s );
int	sem_wait( sem_t *s );
int	sem_trywait( sem_t *s );
int	sem_timedwait( sem_t *s, const struct timespec *abstime );
int	sem_clockwait( sem_t *s, clockid_t clk, const struct timespec *abstime );
int	sem_post( sem_t *s );
int	sem_getvalue( sem_t *s, int *v );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_SEMAPHORE_H__ */
