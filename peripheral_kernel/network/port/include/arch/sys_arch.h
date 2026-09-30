/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_arch.h
 *	The shapes of the things lwIP asks the kernel for (design 12.6).
 *
 *	A semaphore and a mutex are kernel objects, so each is an ID and a
 *	flag saying whether it was made. A mailbox is a ring of pointers
 *	with a semaphore on each end, because lwIP wants a fixed depth and
 *	an empty/full distinction that the T-Kernel mailbox does not give.
 */

#ifndef __TS_LWIP_ARCH_SYS_ARCH_H__
#define __TS_LWIP_ARCH_SYS_ARCH_H__

#define SYS_MBOX_NULL		NULL
#define SYS_SEM_NULL		NULL

#define SYS_ARCH_MBOX_SIZE	16

typedef struct {
	int	id;			/* the kernel semaphore, 0 when unused */
} sys_sem_t;

typedef struct {
	int	id;			/* the kernel mutex, 0 when unused */
} sys_mutex_t;

typedef struct {
	int	id;			/* a semaphore counting what is in it */
	int	free_id;		/* and one counting the room left */
	int	head, tail;
	int	size;
	void	*msg[SYS_ARCH_MBOX_SIZE];
} sys_mbox_t;

typedef int sys_thread_t;		/* the task ID */

typedef unsigned int sys_prot_t;

/* Every one of these is valid only when its identifier is not zero */
#define sys_sem_valid(s)	(((s) != NULL) && ((s)->id > 0))
#define sys_sem_set_invalid(s)	do { if ((s) != NULL) { (s)->id = 0; } } while (0)
#define sys_mutex_valid(m)	(((m) != NULL) && ((m)->id > 0))
#define sys_mutex_set_invalid(m) do { if ((m) != NULL) { (m)->id = 0; } } while (0)
#define sys_mbox_valid(mb)	(((mb) != NULL) && ((mb)->id > 0))
#define sys_mbox_set_invalid(mb) do { if ((mb) != NULL) { (mb)->id = 0; } } while (0)

#endif /* __TS_LWIP_ARCH_SYS_ARCH_H__ */
