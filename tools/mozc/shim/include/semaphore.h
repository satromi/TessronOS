/*
 *	semaphore.h -- counting semaphores for one thread (ts_posix.cc)
 *	Copyright (C) 2026 satromi
 *	This software is distributed under the T-License 2.2.
 */
#ifndef TS_SEMAPHORE_H
#define TS_SEMAPHORE_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int	count;
} sem_t;

#define SEM_VALUE_MAX	0x7fffffff

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_post(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_timedwait(sem_t *sem, const struct timespec *abstime);
int sem_getvalue(sem_t *sem, int *sval);

#ifdef __cplusplus
}
#endif
#endif
