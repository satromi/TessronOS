/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rumpglue.h
 *	Between the NetBSD rump kernel and the rest of TessronOS (design 12.6)
 *
 *	The rump kernel (tools/netbsd/build.sh) is one object whose only
 *	global names are its rump_ entry points. It is compiled with
 *	NetBSD's headers, which cannot meet T-Kernel's, so what is used of it
 *	here is declared with plain types: an int is an int, a pointer to one
 *	of its structures a void pointer, and a register a 64 bit word.
 */

#ifndef __RUMPGLUE_H__
#define __RUMPGLUE_H__

/* ---------------------------------------------------------------- the rump kernel */

/* Bring the rump kernel up; 0 or a NetBSD errno value */
IMPORT int	rump_init( void );

/*
 * A system call of the rump kernel: the arguments each in a 64 bit
 * word, as the kernel's argument structures lay them out. Answers 0 or
 * a NetBSD errno value; the results are in retval[0] (and [1]).
 */
IMPORT int	rump_syscall( int num, void *data, UD dlen, D *retval );

/* The calling task as a thread of the rump kernel, while it runs in it */
IMPORT void	rump_schedule( void );
IMPORT void	rump_unschedule( void );

/* ---------------------------------------------------------------- rumpuser.c */

/*
 * Around a wait outside the rump kernel by one of its threads: its
 * virtual processor and big lock are given up, then taken back.
 */
IMPORT void	knl_rump_unsched( INT *p_nlocks );
IMPORT void	knl_rump_sched( INT nlocks );

/* The system call numbers of NetBSD used by so_api.c (sys/sys/syscall.h) */
#define NB_SYS_read		3
#define NB_SYS_write		4
#define NB_SYS_close		6
#define NB_SYS_recvmsg		27
#define NB_SYS_sendmsg		28
#define NB_SYS_recvfrom		29
#define NB_SYS_accept		30
#define NB_SYS_getpeername	31
#define NB_SYS_getsockname	32
#define NB_SYS_ioctl		54
#define NB_SYS_fcntl		92
#define NB_SYS_connect		98
#define NB_SYS_bind		104
#define NB_SYS_setsockopt	105
#define NB_SYS_listen		106
#define NB_SYS_getsockopt	118
#define NB_SYS_sendto		133
#define NB_SYS_shutdown		134
#define NB_SYS_getrlimit	194
#define NB_SYS_setrlimit	195
#define NB_SYS___sysctl		202
#define NB_SYS_poll		209
#define NB_SYS___socket30	394

#endif /* __RUMPGLUE_H__ */
